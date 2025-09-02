#include <asm-generic/errno-base.h>
#include <asm-generic/ioctls.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <uchar.h>
#include <unistd.h>

#define ctrl(k) ((k) & 0x1f)
#define version "0.0"
#define tabstop 4
#define quitConf 0

enum navKeys {
  bkspc = 127,
  left = 1000,
  right,
  up,
  down,
  del,
  home,
  endkey,
  pg_up,
  pg_down,
};

// info
typedef struct edRow {
  int size;
  int rend_size;
  char *chara;
  char *render;
} edRow;

struct editorconfig {
  int curX, curY;
  int rendX;
  int screenRow;
  int screenCol;
  int rowOffset;
  int colOffset;
  int numRow;
  edRow *row;
  char *fname;
  int modif;
  char statusmsg[40];
  time_t statusmsg_time;
  struct termios orig_termios;
};
struct editorconfig E;

// buffer to write at once
struct appendBuf {
  char *c;
  int len;
};
#define appendBuf_init {NULL, 0} /* empty */
void editorDrawStatbar(struct appendBuf *ab);
void editorDrawmsgbar(struct appendBuf *ab);
void editorStatusmsg(const char *frmt, ...);
char *editorPrompt(char *prompt);

void abAppend(struct appendBuf *ab, const char *s, int len) {
  char *new = realloc(ab->c, ab->len + len);
  if (new == NULL)
    return;
  memcpy(&new[ab->len], s, len);
  ab->c = new;
  ab->len += len;
}

void abFree(struct appendBuf *ab) { free(ab->c); }

// i/o for files
char *editorRowtoStr(int *bufrlen) {
  int lentotal = 0;
  int i;
  for (i = 0; i < E.numRow; i++)
    lentotal += E.row[i].size + 1;

  *bufrlen = lentotal;

  char *bufr = malloc(lentotal);
  char *tmp = bufr;
  for (i = 0; i < E.numRow; i++) {
    memcpy(tmp, E.row[i].chara, E.row[i].size);
    tmp += E.row[i].size;
    *tmp = '\n';
    tmp++;
  }
  return bufr;
}
void editorSave(void) {
  if (E.fname == NULL) {
    E.fname = editorPrompt("save as %s");
    if (E.fname == NULL)
      editorStatusmsg("not savin ");

    return;
  }

  int len;
  char *bufr = editorRowtoStr(&len);
  int fh = open(E.fname, O_RDWR | O_CREAT, 0644);
  if (fh != -1) {
    if (ftruncate(fh, len) != -1) {
      if (write(fh, bufr, len) == len) {
        E.modif = 0;
        close(fh);
        free(bufr);
        editorStatusmsg(" '%s' %dL, %dB written", E.fname, E.numRow, len);
        return;
      }
    }
    close(fh);
  }
  free(bufr);
  editorStatusmsg("error :%s", strerror(errno));
}
void editorInsertRow(int idx, char *s, size_t len);
void die(const char *s);

void editorOpen(char *fname) {
  free(E.fname);
  E.fname = strdup(fname);
  FILE *fh = fopen(fname, "r");
  if (!fh)
    die("fopen");

  char *line = NULL;
  size_t linemax = 0;

  ssize_t linelen;
  while ((linelen = getline(&line, &linemax, fh)) != -1) {

    while (linelen > 0 &&
           (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
      linelen--;
    editorInsertRow(E.numRow, line, linelen);
  }

  free(line);
  fclose(fh);
  E.modif = 0;
}

// E.row->size = linelen;
// E.row->chara = malloc(linelen + 1);
// memcpy(E.row->chara, line, linelen);
// E.row->chara[linelen] = '\0';
// E.numRow = 1;

// free(line);
// fclose(fh);
// E.modif = 0;

// functions

void disable_raw(void);
void enable_raw(void);
int editorRead(void);
void editorDrawrows(struct appendBuf *ab);
void editorRefreshScreen(void);
void editorProcesskeys(void);
void editorCursorMove(int key);
void editorDrawStatbar(struct appendBuf *ab);
void editorDrawmsgbar(struct appendBuf *ab);
void editorScroll(void);
int editorCurXtoRendX(edRow *row, int curX);
int getCursorPos(int *row, int *col);
int getTermSize(int *r, int *c);
void initeditor(void);
void editorUpdateRow(edRow *row);

void editorRowInsertChara(edRow *row, int idx, int chara);

void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
  perror(s);
  exit(1);
}

// terminal stuff

void disable_raw(void) {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enable_raw(void) {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    die("tcgetattr");
  atexit(disable_raw);

  struct termios raw = E.orig_termios;
  // tcgetattr(STDIN_FILENO, &raw);

  raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
  raw.c_cflag |= CS8;
  // raw.c_cflag &= ~CSIZE;
  raw.c_oflag &= ~(OPOST);
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr");
}

int editorRead(void) {
  int n;
  char c;
  while ((n = read(STDIN_FILENO, &c, 1)) != 1) {
    if (n == -1 && errno != EAGAIN)
      die("read");
  }

  if (c == '\x1b') {
    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1)
      return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
      return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1)
          return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
          case '1':
            return home;
          case '3':
            return del;
          case '4':
            return endkey;
          case '5':
            return pg_up;
          case '6':
            return pg_down;
          case '7':
            return home;
          case '8':
            return endkey;
          }
        }
      } else {
        switch (seq[1]) {
        case 'A':
          return up;
        case 'B':
          return down;
        case 'C':
          return right;
        case 'D':
          return left;
        case 'H':
          return home;
        case 'F':
          return endkey;
        }
      }
    } else if (seq[0] == 0) {
      switch (seq[1]) {
      case 'H':
        return home;
      case 'F':
        return endkey;
      }
    }

    return '\x1b';

  } else {
    return c;
  }
}

// output fn

void editorScroll(void) {

  E.rendX = 0;
  if (E.curY < E.numRow) {
    E.rendX = editorCurXtoRendX(&E.row[E.curY], E.curX);
  }
  if (E.curY < E.rowOffset) {
    E.rowOffset = E.curY;
  }
  if (E.curY >= E.screenRow + E.rowOffset) {
    E.rowOffset = E.curY - E.screenRow + 1;
  }
  if (E.rendX < E.colOffset)
    E.colOffset = E.rendX;
  if (E.rendX >= E.screenCol + E.colOffset)
    E.colOffset = E.rendX - E.screenCol + 1;
}

void editorDrawrows(struct appendBuf *ab) {
  int y;
  for (y = 0; y < E.screenRow; y++) {

    int filerow = y + E.rowOffset;
    if (filerow >= E.numRow) {
      if (y == E.screenRow / 3 && E.numRow == 0) {
        char welcome[80];
        int welcomeLen = snprintf(welcome, sizeof(welcome),
                                  "Arnv text editor --version %s", version);
        if (welcomeLen > E.screenCol)
          welcomeLen = E.screenCol;
        int padding = (E.screenCol - welcomeLen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while (padding--) {
          abAppend(ab, " ", 1);
        }
        abAppend(ab, welcome, welcomeLen);
      }
    }

    else {
      int len = E.row[filerow].rend_size - E.colOffset;
      if (len < 0)
        len = 0;
      if (len > E.screenCol)
        len = E.screenCol;
      abAppend(ab, &E.row[filerow].render[E.colOffset], len);
    }

    abAppend(ab, "\x1b[K", 3);

    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatbar(struct appendBuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char lstatus[40], cstatus[20], rstatus[40];
  int len = snprintf(lstatus, sizeof(lstatus), "%.20s %s",
                     E.fname ? E.fname : "[nameless]", (E.modif ? " ~ " : " "));

  int progPercent = (E.numRow == 0) ? 0 : E.curY * 100 / E.numRow;

  int clen = 0;
  if (E.curY == 0) {
    clen = snprintf(cstatus, sizeof(cstatus), "  Top  ");
  } else if (E.curY == E.numRow - 1 || E.curY == E.numRow) {
    clen = snprintf(cstatus, sizeof(cstatus), "  Bot  ");
  } else {
    clen = snprintf(cstatus, sizeof(cstatus), "  %d%%  ", progPercent);
  }
  int rlen =
      snprintf(rstatus, sizeof(rstatus), " < %d:%d", E.curY + 1, E.curX + 1);

  if (len > E.screenCol)
    len = E.screenCol;
  abAppend(ab, lstatus, len);
  int empty = E.screenCol - len - (rlen + clen + 1);
  while (empty-- > 0)
    abAppend(ab, " ", 1);

  abAppend(ab, "\x1b[1;36;40m", 10);
  abAppend(ab, cstatus, clen);
  abAppend(ab, "\x1b[0m", 4);
  abAppend(ab, "\x1b[1;30;43m", 10);
  // abAppend(ab, "\x1b[40", 4);

  abAppend(ab, rstatus, rlen);

  // while (len < E.screenCol) {
  //   if (E.screenCol - len == rlen) {
  //     abAppend(ab, rstatus, rlen);
  //
  //   } else if (E.screenCol - len - rlen == clen) {
  //     abAppend(ab, cstatus, clen);
  //     continue;
  //   } else {
  //     abAppend(ab, " ", 1);
  //     len++;
  //   }
  // }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}
void editorRefreshScreen(void) {

  editorScroll();
  struct appendBuf ab = appendBuf_init;

  abAppend(&ab, "\x1b[48;2;20;27;30m", strlen("\x1b[48;2;0;128;128m"));
  abAppend(&ab, "\x1b[38;2;218;218;218m", strlen("\x1b[48;2;218;218;218m"));

  abAppend(&ab, "\x1b[?25l", 6);
  // abAppend(&ab, "\x1b[2J", 4);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawrows(&ab);
  editorDrawStatbar(&ab);
  editorDrawmsgbar(&ab);
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.curY - E.rowOffset) + 1,
           (E.rendX - E.colOffset) + 1);
  abAppend(&ab, buf, strlen(buf));

  // abAppend(&ab, "\x1b[H", 3); this brought the cursor to home or (0,0)
  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.c, ab.len);
  abFree(&ab);
}

void editorInsertRow(int idx, char *s, size_t len) {
  if (idx < 0 || idx > E.numRow)
    return;
  E.row = realloc(E.row, sizeof(edRow) * (E.numRow + 1));
  memmove(&E.row[idx + 1], &E.row[idx], sizeof(edRow) * (E.numRow - idx));

  // int index = E.numRow;
  E.row[idx].size = len;

  // E.row->size = len;
  E.row[idx].chara = malloc(len + 1);
  memcpy(E.row[idx].chara, s, len);
  E.row[idx].chara[len] = '\0';

  E.row[idx].rend_size = 0;
  E.row[idx].render = NULL;

  editorUpdateRow(&E.row[idx]);
  E.numRow++;
  E.modif++;
}

void editorFreeRow(edRow *row) {
  free(row->render);
  free(row->chara);
}
void editorDeleteRow(int idx) {
  if (idx < 0 || idx > E.numRow)
    return;

  editorFreeRow(&E.row[idx]);
  memmove(&E.row[idx], &E.row[idx + 1], sizeof(edRow) * (E.numRow - idx - 1));
  E.numRow--;
  E.modif++;
}
void editorUpdateRow(edRow *row) {
  int tab = 0;
  int i;
  for (i = 0; i < row->size; i++)
    if (row->chara[i] == '\t')
      tab++;

  free(row->render);
  row->render = malloc(row->size + tab * (tabstop - 1) + 1);

  int idx;
  idx = 0;
  for (i = 0; i < row->size; i++) {
    if (row->chara[i] == '\t') {
      row->render[idx++] = ' ';
      while (idx % tabstop != 0)
        row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chara[i];
    }
  }
  row->render[idx] = '\0';
  row->rend_size = idx;
}
int editorCurXtoRendX(edRow *row, int curX) {
  int rendX = 0;
  int i;
  for (i = 0; i < curX; i++) {
    if (row->chara[i] == '\t')
      rendX += (tabstop - 1) - (rendX % tabstop);
    rendX++;
  }
  return rendX;
}
void editorStatusmsg(const char *frmt, ...) {
  va_list ap;
  va_start(ap, frmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), frmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}
void editorDrawmsgbar(struct appendBuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int lenmsg = strlen(E.statusmsg);
  if (lenmsg > E.screenCol)
    lenmsg = E.screenCol;
  if (lenmsg && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, lenmsg);
}
void editorRowInsertChara(edRow *row, int idx, int chara) {
  if (idx < 0 || idx > row->size)
    idx = row->size;
  row->chara = realloc(row->chara, row->size + 2);
  memmove(&row->chara[idx + 1], &row->chara[idx], row->size - idx + 1);
  row->size++;
  row->chara[idx] = chara;
  editorUpdateRow(row);
  E.modif++;
}
void editorRowDelChar(edRow *row, int idx) {
  if (idx < 0 || idx > row->size)
    return;
  memmove(&row->chara[idx], &row->chara[idx], row->size - idx);
  row->size--;
  editorUpdateRow(row);
  E.modif++;
}

// editor input fun
void editorAppendStr(edRow *row, char *c, size_t len);
void editorInsertChar(int ch) {
  if (E.curY == E.numRow) {
    editorInsertRow(E.numRow, "", 0);
  }
  editorRowInsertChara(&E.row[E.curY], E.curX, ch);
  E.curX++;
}
void editorDelChar(void) {
  if (E.curY == E.numRow)
    return;
  if (E.curY == 0 && E.curX == 0)
    return;

  edRow *row = &E.row[E.curY];
  if (E.curX > 0) {
    editorRowDelChar(row, E.curX - 1);
    E.curX--;
  } else {
    E.curX = E.row[E.curY - 1].size;
    editorAppendStr(&E.row[E.curY - 1], row->chara, row->size);
    editorDeleteRow(E.curY);
    E.curY--;
  }
}
void editorAppendStr(edRow *row, char *c, size_t len) {
  row->chara = realloc(row->chara, row->size + len + 1);
  memcpy(&row->chara[row->size], c, len);
  row->size += len + 1;
  row->chara[row->size] = '\0';
  editorUpdateRow(row);
  E.modif++;
}
void editorInsertNewLine(void) {
  if (E.curX == 0)
    editorInsertRow(E.curY, "", 0);
  else {
    edRow *row = &E.row[E.curY];
    editorInsertRow(E.curY + 1, &row->chara[E.curX], row->size - E.curX);
    row = &E.row[E.curY];
    row->size = E.curX;
    row->chara[row->size] = '\0';
    editorUpdateRow(row);
  }
  E.curY++;
  E.curX = 0;
}

char *editorPrompt(char *prompt) {
  size_t bufsize = 128;
  char *buf = malloc(sizeof(bufsize));

  size_t buflen = 0;
  buf[0] = '\0';

  while (1) {
    editorStatusmsg(prompt, buf);
    editorRefreshScreen();

    int c = editorRead();
    if (c == del || c == ctrl('h') || c == bkspc) {
      if (buflen != 0)
        buf[--buflen] = '\0';
    } else if (c == '\x1b') {
      editorStatusmsg("");
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (buflen != 0) {
        editorStatusmsg("");
        return buf;
      }

    } else if (!iscntrl(c) && c < 128) {
      if (buflen == bufsize - 1)
        bufsize *= 2;
      buf = realloc(buf, sizeof(bufsize));
    }
    buf[buflen++] = c;
    buf[buflen] = '\0';
  }
}
// input fn

void editorProcesskeys(void) {
  static int quitCount = quitConf;

  int c = editorRead();
  switch (c) {
  case '\r':
    editorInsertNewLine();
    break;
  case ctrl('w'):
    editorSave();
    break;
  case ctrl('q'):
    if (E.modif && quitCount > 0) {
      editorStatusmsg(
          "unsaved changes, quit %d more times to quit without saving ",
          quitCount);
      quitCount--;
      return;
    }
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;
  case home:
    E.curX = 0;
    break;
  case endkey:
    if (E.curY < E.numRow)
      E.curX = E.row[E.curY].size;
    break;
  case ctrl('h'):
  case bkspc:
  case del:
    // ye bhi baaki
    if (c == del)
      editorCursorMove(right);
    editorDelChar();

    break;

  case pg_up:
  case pg_down: {
    if (c == pg_up) {
      E.curY = E.rowOffset;
    } else if (c == pg_down) {
      E.curY = E.screenRow + E.rowOffset + 1;
      if (E.curY > E.numRow)
        E.curY = E.numRow;
    }

    int l = E.screenRow;
    while (l--)
      editorCursorMove(c == pg_up ? up : down);
    break;
  }

  case up:
  case down:
  case left:
  case right:
    editorCursorMove(c);
    break;
  case ctrl('l'):
  case '\x1b':
    // baaki
    break;
  default:
    editorInsertChar(c);
    break;
    // todo some keys like d,f move the cursor and x or c causes the segfault
  }
}

void editorCursorMove(int key) {
  edRow *row = (E.curY >= E.numRow) ? NULL : &E.row[E.curY];
  switch (key) {

  case left:
    if (E.curX != 0) {
      E.curX--;
    } else if (E.curY > 0) {
      E.curY--;
      E.curX = E.row[E.curY].size;
    }
    break;
  case right:
    if (row && E.curX < row->size) {
      E.curX++;
    } else if (row && E.curX == row->size) {
      E.curY++;
      E.curX = 0;
    }
    break;

  case up:
    if (E.curY != 0) {
      E.curY--;
    }
    break;

  case down:
    if (E.curY < E.numRow) {
      E.curY++;
    }
    break;
  }
  row = (E.curY >= E.numRow) ? NULL : &E.row[E.curY];
  int rowlen = row ? row->size : 0;
  if (E.curX > rowlen) {
    E.curX = rowlen;
  }
}

// initialising the editor
int getCursorPos(int *row, int *col) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    return -1;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1)
      break;
    if (buf[i] == 'R')
      break;
    i++;
  }
  buf[i] = '\0';
  if (buf[0] != '\x1b' || buf[1] != '[')
    return -1;
  if (sscanf(&buf[2], "%d;%d", row, col) != 2)
    return -1;
  return 0;
}
int getTermSize(int *r, int *c) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
      return -1;
    return getCursorPos(r, c);
  } else {
    *c = ws.ws_col;
    *r = ws.ws_row;
    return 0;
  }
}

void initeditor(void) {
  E.curX = 0;
  E.rendX = 0;
  E.curY = 0;
  E.numRow = 0;
  E.row = NULL;
  E.rowOffset = 0;
  E.colOffset = 0;
  E.fname = NULL;
  E.modif = 0;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  if (getTermSize(&E.screenRow, &E.screenCol) == -1)
    die("getTermSize");
  E.screenRow -= 2;
}

// main fn

int main(int argc, char *argv[]) {
  enable_raw();
  initeditor();

  if (argc >= 2)
    editorOpen(argv[1]);

  editorStatusmsg("halo :D");

  while (1) {
    editorRefreshScreen();
    editorProcesskeys();
  }
  return 0;
}
