/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** data ***/

// Ctrl Key combinations
#define CTRL_KEY(k) ((k) & 0x1f)

// Maintain out terminal state
struct editorConfig {
    int screenrows;
    int screencols;
    struct termios orig_termios; // Original terminal state    
};

struct editorConfig E;


/*** terminal stuff ***/

// Handle errors gracefully
void die(const char* s) {
    // Clear the screen and reposition the cursor on exit()
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

/*
 * Be good terminal citizen and
 * Disable the raw mode before we give back
 * control to orginal terminal session
 */
void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

/*
 * Turn off what is being written to stdin
 * to be displayed at stdout
 */
void enableRawMode() {
    /*
     * We get the terminal attributes
     * using tcgetattr() , modify the 
     * structure and pass the modified 
     * structure to tcsetattr()
     */
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");    

    /*
     * atexit() can be used to register functions
     * to be called when the program exits,
     * either by returning from main() or by an
     * explicit exit() call
     */
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;

    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_iflag &= ~(OPOST); // Turn off output processing
    raw.c_cflag |= (CS8);
    /*
     * Turn off ECHO, Cannoical mode
     * i.e read byte-by-byte instead of
     * line-by-line, SIGTERM
     */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    // Time-Out for waiting from the user for input
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

/*
 * Read a key entered into the editor
 */
char editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }
    
    // Return the character we read
    return c;
}


/* 
 * Get the cursor position
 *
 * We use the "n" command with argument "6"
 * to ask for cursor position
 */
int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}


/*
 * Get the current terminal size
 */
int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        /*
         * It might happen that ioctl doesn't work on all systems
         * We place the cursor to bottom right and then use escape 
         * sequences to locate the cursor and count the rows and cols
         *
         * "C" moves the cursor right and "B" down and use a large value
         * 999 to make sure we reach edges
         * 
         */
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return (getCursorPosition(rows, cols));
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}



/*** append buffer ***/

/*
 * We need a data structure to append strings to a buffer
 * and finally make a big write than byte size writes
 * as it can flicker
 */
struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);
    if (new == NULL) return;

    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}


/*** output ***/



/*
 * Draw ~ on left hand side of the screen at the end of the file
 */
void editorDrawRows(struct abuf *ab) {
    int y;
    for (y=0; y<E.screenrows; y++) {
        abAppend(ab, "~", 1);

        if (y < E.screenrows -1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

/*
 * This is to clear the screen
 * We write 4 bytes
 * Byte 1 -> "\x1b" is the escape char(27 in decimal)
 * Byte 2 -> "[" (this always follows escape sequence)
 * Byte 3 -> "2" is the argument to byte 4, it says to clear entire screen
 * Byte 4 -> "J" is to clear the screen (https://vt100.net/docs/vt100-ug/chapter3.html#ED"
 */
void editorRefreshScreen() {
    struct abuf ab = ABUF_INIT;

    /*
     * It might happen that cursor might show up for a second when
     * the terminal is drawing to the screen
     * So we hide the cursor and display again when ready
     */
    abAppend(&ab, "\x1b[?25l", 6);

    abAppend(&ab, "\x1b[2J", 4);

    // "[2J" would leave the cursor at the end of screen, need to move
    // it to the top left corner
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    abAppend(&ab, "\x1b[H", 3);

    // Display the cursor again as we are ready
    abAppend(&ab, "\x1b[?25h", 6);

    // Finally write the append buffer at once
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}



/*** input ***/



void editorProcessKeyPress() {
    char c = editorReadKey();

    switch(c) {
        case CTRL_KEY('q'):
            // Clear the screen and reposition the cursor on exit()
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
    }
}

/*** init ***/

void initEditor() {
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main() {
    enableRawMode();
    initEditor();

    while(1) {
        editorRefreshScreen();
        editorProcessKeyPress();
    }

    return 0;
}
