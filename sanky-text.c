#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

struct termios orig_termios;

// kill program on error
void die(const char *s) {
    // looks at global errno and prints a message for it
    perror(s);
    // exit with status of 1
    exit(1);
}

// turn off raw mode when user exits program
void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1) {
        die("tcsetattr");
    }
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        die("tcgetattr");
    };
    atexit(disableRawMode);

    struct termios raw = orig_termios;

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

int main() {
    enableRawMode();

    while (1) {
        char c = '\0';

        if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) {
            die("read");
        }
        if (iscntrl(c)) {
            // if its a ctrl input
            // \r\n makes sure to move cursor to left on enter, whereas \n will keep intendation without the r
            // \r\n is now needed for each newline
            printf("%d\r\n", c);
        } else {
            printf("%d ('%c')\r\n", c, c);
        }

        if (c == 'q') break;
    };

    return 0;
}