#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/types.h>

// DEFINE
#define EDITOR_VERSION "0.0.1"
#define CTRL_KEY(key) ((key) & 0x1f)

enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    PAGE_UP,
    PAGE_DOWN,
    HOME_KEY,
    END_KEY,
    DEL_KEY
};

// Information
typedef struct erow { // erow -> editor row
    int size;
    char *chars;
} erow;

struct editorConfig {
    int coordX, coordY;
    int rowOffset;
    int colOffset;
    int screenrows;
    int screencols;
    struct termios orig_termios;
    int numrows;
    erow *row;
};

// global variable state
struct editorConfig E;


// TERMINAL CONTROL

// kill program on error
void die(const char *s) {
    // clear screen when program exits
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    // looks at global errno and prints a message for it
    perror(s);
    // exit with status of 1
    exit(1);
}

// turn off raw mode when user exits program
void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
        die("tcsetattr");
    }
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
        die("tcgetattr");
    };
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;

    // turn off certain signals, such as ctrl-c, echo
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    // turn off ctrl-s and ctrl-q, make ctrl-m 13
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_cflag |= (CS8);
    //turn off output processing
    raw.c_oflag &= ~(OPOST);

    // min number of bytes of input before read can return
    raw.c_cc[VMIN] = 0;
    // max amount of time before read returns. returns 0 on timeout
    raw.c_cc[VTIME] = 1;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    }
}

int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            die("read");
        }
    }

    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) {
            return '\x1b';
        }

        if (read(STDIN_FILENO, &seq[1], 1) != 1) {
            return '\x1b';
        }

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
                    return '\x1b';
                }
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1':
                            return HOME_KEY;
                        case '3':
                            return DEL_KEY;
                        case '4':
                            return END_KEY;
                        case '5':
                            return PAGE_UP;
                        case '6':
                            return PAGE_DOWN;
                        case '7':
                            return HOME_KEY;
                        case '8':
                            return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A':
                        return ARROW_UP;
                    case 'B':
                        return ARROW_DOWN;
                    case 'C':
                        return ARROW_RIGHT;
                    case 'D':
                        return ARROW_LEFT;
                    case 'H':
                        return HOME_KEY;
                    case 'F':
                        return END_KEY;
                }
            }
        } else if (seq[0] == '0') {
            switch (seq[1]) {
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
            }
        }
        
        return '\x1b';
    } else {
        return c;
    }
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;
    
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return -1;
    }

    while (i < sizeof(buf) -1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) {
            break;
        }
        if (buf[i] == 'R') {
            break;
        }
        i++;
    }

    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') {
        return -1;
    }

    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
        return -1;
    }

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    // ioctl number of columans wide and rows high terminal is
    // icotl may not work on all systems
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // move cursor to bottom right, using 999 to move it C and B prevent it from going beyond the screen
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return -1;
        }
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

// ROWS

void editorAppendRow(char *s, size_t len) {
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.numrows++;
}

// file handling
void editorOpen(char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        die("fopen");
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    while((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
            linelen--;
        }
        editorAppendRow(line, linelen);
    }
    
    free(line);
    fclose(fp);   
}

// Append Buffer -> pointer to buffer in memory
struct abuf {
    char *b;
    int len;
};
// empty buffer
#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char*s, int len) {
    // realloc gives a block of memory to hold the new string. size of the current string + size of string we are appending
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) {
        return;
    }
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}


// INPUT
void editorMoveCursor(int key) {
    erow *row = (E.coordY >= E.numrows) ? NULL : &E.row[E.coordY];
    switch (key) {
        case ARROW_LEFT:
            if (E.coordX != 0) {
                E.coordX--;
            } else if (E.coordY > 0) { // if <- at start of line, move up
                E.coordY--;
                E.coordX = E.row[E.coordY].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.coordX < row->size) {
                E.coordX++;
            } else if (row && E.coordX == row->size) { // if -> at end of line, move down
                E.coordY++;
                E.coordX = 0;
            }
            break;
        case ARROW_UP:
            if (E.coordY != 0) {
                E.coordY--;
            }
            break;
        case ARROW_DOWN:
            // allow scrolling past bottom of screen, but not past bottom of file
            if (E.coordY < E.numrows) {
                E.coordY++;
            }
            break;
    }

    row = (E.coordY >= E.numrows) ? NULL : &E.row[E.coordY];
    int rowLength = row ? row->size : 0;
    if (E.coordX > rowLength) {
        E.coordX = rowLength;
    }
}

void editorProcessKeypress() {
    int c = editorReadKey();

    switch(c) {
        case CTRL_KEY('q'):
            // clear screen then exist when ctrl-q
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case HOME_KEY:
            E.coordX = 0;
            break;
        case END_KEY:
            E.coordX = E.screencols - 1;
            break;
        case PAGE_UP:
        case PAGE_DOWN:
            {
                int times = E.screenrows;
                while (times--) {
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
                }
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
    }
}

// OUTPUT
void editorScroll() {
    // check if cursor is above visible window. If so, move to where cursor is
    if (E.coordY < E.rowOffset) {
        E.rowOffset = E.coordY;
    }
    // check if cursor is below visible window. If so, move 
    if (E.coordY >= E.rowOffset + E.screenrows) {
        E.rowOffset = E.coordY - E.screenrows + 1;
    }
    if (E.coordX < E.colOffset) {
        E.colOffset = E.coordX;
    }
    if (E.coordX >= E.colOffset + E.screencols) {
        E.colOffset = E.coordX - E.screencols + 1;
    }
}

void editorDrawRows(struct abuf *ab) {
    int y;
    // dynamically set screenrows at start
    for (y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowOffset;
        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "Sanky Editor -- Version %s", EDITOR_VERSION);

                if (welcomelen > E.screencols) {
                    welcomelen = E.screencols;
                }
                // centers a string
                int padding = (E.screencols - welcomelen) / 2;

                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }

                while (padding--) {
                    abAppend(ab, " ", 1);
                }
                abAppend(ab, welcome, welcomelen);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            int len = E.row[filerow].size - E.colOffset;
            // len = 0 prevents colOffset from making len a negative number/past the end of line
            if (len < 0) {
                len = 0;
            }
            if (len > E.screencols) {
                len = E.screencols;
            }
            abAppend(ab, &E.row[filerow].chars[E.colOffset], len);
        }
       

        // clear line
        abAppend(ab, "\x1b[K", 3);

        if (y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    editorScroll();
    // initialize new abuf ab
    struct abuf ab = ABUF_INIT;
    
    abAppend(&ab, "\x1b[?251", 6);
    
    // reposition cursor to the top left
    abAppend(&ab, "\x1b[H", 3);
    abAppend(&ab, "\x1b[?25h", 6);

    editorDrawRows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.coordY - E.rowOffset) + 1, (E.coordX - E.colOffset) + 1);
    abAppend(&ab, buf, strlen(buf));
    
    abAppend(&ab, "\x1b[?25h", 6);
    // write buffer contents to stdout, then free the memory used by abuf
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

// initialize

void initEditor() {
    E.coordX = 0; // horizontal coordinate
    E.coordY = 0; // vertical coordinate
    E.numrows = 0;
    E.rowOffset = 0;
    E.colOffset;
    E.row = NULL;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
        die("getWindowSize");
    }
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}