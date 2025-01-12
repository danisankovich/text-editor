#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <stdarg.h>

// DEFINE
#define EDITOR_VERSION "0.0.1"
#define TAB_LENGTH_STOP 8
#define CTRL_KEY(key) ((key) & 0x1f)
#define REMAINING_QUIT_ATTEMPTS 3
#define HL_HIGHLIGHT_NUMBERS (1<<0)

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    PAGE_UP,
    PAGE_DOWN,
    HOME_KEY,
    END_KEY,
    DEL_KEY,
};

enum editorHighlight {
    HL_NORMAL = 0,
    HL_NUMBER,
    HL_MATCH
};

// Information

struct editorSyntax {
    char *filetype;
    char **filematch;
    int flags;
};

typedef struct erow { // erow -> editor row
    int size;
    int renderSize; // content size of render
    char *chars;
    char *render;
    unsigned char *highlight;
} erow;

struct editorConfig {
    int coordX, coordY;
    int renderX; // bc can't assume a character takes up only one column
    int rowOffset;
    int colOffset;
    int screenrows;
    int screencols;
    struct editorSyntax *syntax;
    struct termios orig_termios;
    int numrows;
    erow *row;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    int isDirty;
};

// Filetypes

char *C_HL_extensions[] = { ".c", ".h", ".cpp", NULL };

// HL_DB -> highlight database
struct editorSyntax HL_DB[] = {
    {
        "c",
        C_HL_extensions,
        HL_HIGHLIGHT_NUMBERS
    },
};

// store length of highlight database array
#define HL_DB_ENTRIES (sizeof(HL_DB) / sizeof(HL_DB[0]))

// global variable state
struct editorConfig E;


// TERMINAL CONTROL

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

void editorRefreshScreen();

char *editorPrompt(char *prompt, void(*callback)(char *, int));

// kill program on error
void end(const char *s) {
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
        end("tcsetattr");
    }
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
        end("tcgetattr");
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
        end("tcsetattr");
    }
}

int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            end("read");
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

// Sytanx-styles

// check if character is space, null byte, or certain special character
int is_separator(int c) {
    return isspace(c) || c =='\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorSyntaxStyle(erow *row) {
    row->highlight = realloc(row->highlight, row->renderSize);
    
    // set all characters to highlight normal by default
    memset(row->highlight, HL_NORMAL, row->renderSize);

    if (E.syntax == NULL) return; // if no filetype, return immediately

    int prev_sep = 1; // default to true
    int i = 0;
    while (i < row->renderSize) {
        char c = row->render[i];
        unsigned char prev_highlight = (i > 0) ? row->highlight[i - 1] : HL_NORMAL;

        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) { // check if numbers should be highlighted for the given filetype 
            if ((isdigit(c) && (prev_sep || prev_highlight == HL_NUMBER)) || (c == '.' && prev_highlight == HL_NUMBER)) {
                row->highlight[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        prev_sep = is_separator(c);

        i++;
    }
}

int editorSyntaxColoring(int highlight) {
    switch(highlight) {
        case HL_NUMBER: 
            return 31;
        case HL_MATCH: 
            return 34;
        default:
            return 37;
    }
}

void editorSelectSyntaxHighlight() {
    E.syntax = NULL;
    if (E.filename == NULL) return;

    // strrchr -> returns pointer to the last occurence of a character in a string
    char *ext = strrchr(E.filename, '.');

    for (unsigned int j = 0; j < HL_DB_ENTRIES; j++) {
        struct editorSyntax *s = &HL_DB[j];
        unsigned int i = 0;

        while (s->filematch[i]) {
            int is_ext = (s->filematch[i][0] == '.');
            
            // strcmp -> returns 0 if two strings are equal
            if ((is_ext && ext && !strcmp(ext, s->filematch[i])) || !(is_ext && strstr(E.filename, s->filematch[i]))) {
                E.syntax = s;
                return;
            }
            i++;
        }
    }
}

// ROWS

void editorUpdateRow(erow *row) {
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            tabs++;
        }
    }

    free(row->render);
    row->render = malloc(row->size + tabs*(TAB_LENGTH_STOP - 1) + 1); // max num of characters needed for tab (8)

    int idx = 0;
    for(j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TAB_LENGTH_STOP != 0) {
                row->render[idx++] = ' ';
            }
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->renderSize = idx;

    editorSyntaxStyle(row);
}

void editorAppendRow(int at, char *s, size_t len) {
    if (at < 0 || at > E.numrows) {
        return;
    }

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].renderSize = 0;
    E.row[at].render = NULL;
    E.row[at].highlight = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.isDirty = 1;
}

void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->highlight);
}

void editorDelRow(int at) {
    if (at < 0 || at >= E.numrows) {
        return;
    }

    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.isDirty = 1;
}

void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size) {
        at = row->size;
    }

    row->chars = realloc(row->chars, row->size + 2);

    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.isDirty = 1;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.isDirty = 1;
}

void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at >= row-> size) {
        return;
    }
    // overwrite deleted character with what comes after
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.isDirty = 1;
}

void editorDeleteChar() {
    if (E.coordY == E.numrows) {
        return;
    }
    // do nothing if at start of first line
    if (E.coordX == 0 && E.coordY == 0) {
        return;
    }

    erow *row = &E.row[E.coordY];

    if (E.coordX > 0) {
        editorRowDelChar(row, E.coordX - 1);
        E.coordX--;
    } else {
        E.coordX = E.row[E.coordY - 1].size;
        editorRowAppendString(&E.row[E.coordY - 1], row->chars, row->size);
        editorDelRow(E.coordY);
        E.coordY--;
    }
}

// ops

void editorInsertChar(int c) {
    if (E.coordY == E.numrows) {
        editorAppendRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.coordY], E.coordX, c);
    E.coordX++;
}

void editorInsertNewline() {
    // if at start of line, just append a blank row
    if (E.coordX == 0) {
        editorAppendRow(E.coordY, "", 0);
    } else {
        // otherwise, split current line and pass rightward chars to the new row
        erow *row = &E.row[E.coordY]; // reassign the pointer to keep it from being invalidated
        editorAppendRow(E.coordY + 1, &row->chars[E.coordX], row->size - E.coordX);
        row = &E.row[E.coordY];
        row->size = E.coordX;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.coordY++;
    E.coordX = 0;
}

// file handling

char *editorRowsToString(int *buflen) {
    int totlen = 0;
    int j;
    for (j = 0; j < E.numrows; j++) {
        totlen += E.row[j].size + 1;
    }
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;

    for (j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editorOpen(char *filename) {
    free(E.filename);
    // strdup -> makes a copy of given string, alloc memory for it, assuming you'll free it
    E.filename = strdup(filename);

    editorSelectSyntaxHighlight();

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        end("fopen");
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    while((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
            linelen--;
        }
        editorAppendRow(E.numrows, line, linelen);
    }
    
    free(line);
    fclose(fp);
    E.isDirty = 0;
}

void saveFile() {
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save As: %s (Press ESC to cancel)", NULL);
        if (E.filename == NULL) {
            editorSetStatusMessage("Save Aborted");
            return;
        }
        editorSelectSyntaxHighlight();
    }

    int len;
    char *buf = editorRowsToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.isDirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
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
char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);
    
    size_t buflen = 0;
    buf[0] = '\0';

    while (1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();

        // allow special keys in prompt
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) {
                buf[--buflen] = '\0';
            }
        } else if (c == '\x1b') {
            editorSetStatusMessage("");
            if (callback) {
                callback(buf, c);
            }
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                if (callback) {
                    callback(buf, c);
                }
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
        if (callback) {
            callback(buf, c);
        }
    }
}


// convert renderx to a coordX
int editorRowRenderXToCoordX(erow *row, int rx) {
    int curr_rx = 0;
    int coordX;
    
    for (coordX = 0; coordX < row->size; coordX++) {
        if (row->chars[coordX] == '\t') {
            curr_rx += (TAB_LENGTH_STOP - 1) - (curr_rx % TAB_LENGTH_STOP);
        }
        curr_rx++;

        if (curr_rx > rx) {
            return coordX;
        }
    }
    return coordX;
}

void searchCB(char *term, int key) {
    static int prev_match = -1;
    static int dir = 1;

    static int saved_highlight_line;
    static char *saved_highlight = NULL;

    if (saved_highlight) {
        memcpy(E.row[saved_highlight_line].highlight, saved_highlight, E.row[saved_highlight_line].renderSize);
        free(saved_highlight);
        saved_highlight = NULL;
    }

    if (key == '\r' || key == '\x1b') {
        prev_match = -1;
        dir = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        dir = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        dir = -1;
    } else {
        prev_match = -1;
        dir = 1;
    }

    if (prev_match == -1) {
        dir = 1;
    }

    int current = prev_match;

    int i;
    for (i = 0; i < E.numrows; i++) {
        current += dir;
        if (current == -1) {
            current = E.numrows - 1;
        } else if (current == E.numrows) {
            current = 0;
        }

        erow *row = &E.row[current];
        char *match = strstr(row->render, term);
        if (match) {
            prev_match = current;
            E.coordY = current;
            E.coordX = editorRowRenderXToCoordX(row, match - row->render);
            E.rowOffset = E.numrows;

            saved_highlight_line = current;
            saved_highlight = malloc(row->renderSize);
            memcpy(saved_highlight, row->highlight, row->renderSize);

            memset(&row->highlight[match - row->render], HL_MATCH, strlen(term));
            break;
        }
    }
}

void search() {
    int stored_coordX = E.coordX;
    int stored_coordY = E.coordY;
    int stored_colOffset = E.colOffset;
    int stored_rowOffset = E.rowOffset;

    char *term = editorPrompt("Search: %s (Press ESC/Arrows/Enter)", searchCB);

    if (term) {
        free(term);
    } else {
        E.coordX = stored_coordX;
        E.coordY = stored_coordY;
        E.colOffset = stored_colOffset;
        E.rowOffset = stored_rowOffset;
    }
}

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
    static int quit_times = REMAINING_QUIT_ATTEMPTS;
    int c = editorReadKey();

    switch(c) {
        case '\r':
            editorInsertNewline();
            break;
        case CTRL_KEY('q'):
            if (E.isDirty && quit_times > 0) {
                editorSetStatusMessage("WARNING!!! FILE HAS UNSAVED CHANGES. QUIT %d more times to exit editor.", quit_times);
                quit_times--;
                return;
            }
            // clear screen then exist when ctrl-q
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case CTRL_KEY('s'):
            saveFile();
            break;
        case HOME_KEY:
            E.coordX = 0;
            break;
        case END_KEY:
            if (E.coordY < E.numrows) {
                E.coordX = E.row[E.coordY].size;
            }
            break;
        case CTRL_KEY('f'):
            search();
            break;
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) {
                editorMoveCursor(ARROW_RIGHT);
            }
            editorDeleteChar();    
            break;
        case PAGE_UP:
        case PAGE_DOWN: // scroll up/down an entire page
            if (c == PAGE_UP) {
                E.coordY = E.rowOffset;
            } else if (c == PAGE_DOWN) {
                E.coordY = E.rowOffset + E.screenrows - 1;
                if (E.coordY > E.numrows) {
                    E.coordY = E.numrows;
                }
            }
            int times = E.screenrows;
            while (times--) {
                editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
        case CTRL_KEY('l'):
        case '\x1b':
            break;
        // add character not mapped above
        default:
            editorInsertChar(c);
            break;
    }
    quit_times = REMAINING_QUIT_ATTEMPTS;
}

// OUTPUT

int editorRowCoordXtoRenderX(erow *row, int coordX) {
    int renderX = 0;
    int j;
    for (j = 0; j < coordX; j++) {
        if (row->chars[j] == '\t') {
            renderX += (TAB_LENGTH_STOP - 1) - (renderX % TAB_LENGTH_STOP);
        }
        renderX++;
    }
    return renderX;
}

void editorScroll() {
    E.renderX = 0;

    if (E.coordY < E.numrows) {
        E.renderX = editorRowCoordXtoRenderX(&E.row[E.coordY], E.coordX);
    }
    // check if cursor is above visible window. If so, move to where cursor is
    if (E.coordY < E.rowOffset) {
        E.rowOffset = E.coordY;
    }
    // check if cursor is below visible window. If so, move 
    if (E.coordY >= E.rowOffset + E.screenrows) {
        E.rowOffset = E.coordY - E.screenrows + 1;
    }
    if (E.renderX < E.colOffset) {
        E.colOffset = E.renderX;
    }
    if (E.renderX >= E.colOffset + E.screencols) {
        E.colOffset = E.renderX - E.screencols + 1;
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
            int len = E.row[filerow].renderSize - E.colOffset;
            // len = 0 prevents colOffset from making len a negative number/past the end of line
            if (len < 0) {
                len = 0;
            }
            if (len > E.screencols) {
                len = E.screencols;
            }

            char *c = &E.row[filerow].render[E.colOffset];
            unsigned char *highlight = &E.row[filerow].highlight[E.colOffset];
            int current_color = -1;

            int j;
            for (j = 0; j < len; j++) {
                if (highlight[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        abAppend(ab, "\x1b[39m", 5);
                        current_color = -1;
                    }

                    abAppend(ab, &c[j], 1);
                } else {
                    int color = editorSyntaxColoring(highlight[j]);

                    if (color != current_color) {
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, clen);
                    }

                    abAppend(ab, &c[j], 1);
                }
            }

            abAppend(ab, "\x1b[39m", 5);
        }
       
        // clear line -> esc[K
        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab) {
    // m -> makes text printed after it to be printed with attributes like bold (1), underscore (4), blink (5) and inverted colors(7)
    abAppend(ab, "\x1b[7m", 4);

    char status[80], rstatus[80];

    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename ? E.filename : "[No Name]", E.numrows, E.isDirty ? "(modified)" : "");
    
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d", E.syntax ? E.syntax->filetype : "no FT", E.coordY + 1, E.numrows);
    
    if (len > E.screencols) {
        len = E.screencols;
    }

    abAppend(ab, status, len);

    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) {
        msglen = E.screencols;
    }

    if (msglen && time(NULL) - E.statusmsg_time < 5) {
        abAppend(ab, E.statusmsg, msglen);
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
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.coordY - E.rowOffset) + 1, (E.renderX - E.colOffset) + 1);
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
    E.renderX = 0;
    E.numrows = 0;
    E.rowOffset = 0;
    E.colOffset = 0;
    E.row = NULL;
    E.filename = NULL;
    E.isDirty = 0;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.syntax = NULL; // Null -> no filetype/no syntax highlight

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
        end("getWindowSize");
    }
    E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-Q = quit | Ctrl-S = save | CTRL-F = find");

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}