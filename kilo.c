/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>


/*** defines ****/

#define KILO_VERSION "0.0.1"

// Ctrl Key combinations
#define CTRL_KEY(k) ((k) & 0x1f)

// Arrow keys
enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};


/*** data ***/


// Define ONE row in the text editor
typedef struct erow {
    int size;
    char* chars;
}erow;

// Maintain out terminal state
struct editorConfig {
    int cx, cy; // Maintain cursor position
    int screenrows;
    int screencols;
    int numrows;
    erow* row;
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
int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    // If we get a special sequence (something like arrow keys)
    if (c == '\x1b') {
        char seq[3];

        /*
         * If we read an escape character, we immediately read 
         * two more bytes into the seq buffer. If either of these 
         * reads time out (after 0.1 seconds), then we assume the 
         * user just pressed the Escape key and return that.
         */
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            /* Handling PAGE UP and PAGE DOWN
             * Page UP : <esc>[5~
             * Page DOWN : <esc>[6~
             */
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5' : return PAGE_UP;
                        case '6' : return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            }
            else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP; //Up
                    case 'B': return ARROW_DOWN; //Down
                    case 'C': return ARROW_RIGHT; //Right
                    case 'D': return ARROW_LEFT; //Left
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        }
        else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    }
    else {
        // Return the character we read
        return c;
    }
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


/*** row operations  ***/

void editorAppendRow(char* s, size_t len) {
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len+1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.numrows++;
}


/*** file i/o  ***/

/*
 * Allow the user to open an actual file to edit :-)
 */
void editorOpen(char* filename) {
    FILE* fp = fopen(filename, "r");
    if(!fp) die("fopen");

    char* line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen>0 && (line[linelen-1] == '\n' || line[linelen-1] == '\r'))
            linelen--;
        editorAppendRow(line, linelen);
    }
    free(line);
    fclose(fp);
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
        /* 
         * Check if there is something in text buffer
         * If there is not then we draw the welcome page
         * else we draw the text buffer
         */
        if (y >= E.numrows) {  
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                        "Aniket's Editor -- version %s", KILO_VERSION);
                if (welcomelen > E.screencols) welcomelen = E.screencols;

                /* Center the welcome message 
                 * Divide the screen's width in half and subtract
                 * half of the string's length. This gives us the
                 * how far from left edge would we start printing
                 */
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            } else {
                abAppend(ab, "~", 1);
            } 
        } else {
            int len = E.row[y].size;
            if (len > E.screencols) len = E.screencols;
            abAppend(ab, E.row[y].chars, len);
        }

        // Clear lines one at a time rather than entire screen refresh
        abAppend(ab, "\x1b[K", 3);
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

    /*
     * We are clearing one line at a time
     * so do not need to clear the entire screen
     * abAppend(&ab, "\x1b[2J", 4);
     */

    // "[2J" would leave the cursor at the end of screen, need to move
    // it to the top left corner
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    // Mover cursor to the location pointed by co-ordinates
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));

    // Display the cursor again as we are ready
    abAppend(&ab, "\x1b[?25h", 6);

    // Finally write the append buffer at once
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}



/*** input ***/

// Cursor Movement
void editorMoveCursor(int key) {
    switch(key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            }
            break;
        case ARROW_RIGHT:
            if (E.cx != E.screencols -1) {
                E.cx++;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy != E.screenrows -1) {
                E.cy++;
            }
            break;
    }
}

// Read the key pressed
void editorProcessKeyPress() {
    int c = editorReadKey();

    switch(c) {
        case CTRL_KEY('q'):
            // Clear the screen and reposition the cursor on exit()
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            E.cx = E.screencols - 1;
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                int times = E.screenrows;
                while (times--) {
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
                }
                break;
            }

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
    }
}

/*** init ***/

void initEditor() {
    // Initialize the cursor position
    E.cx = 0;
    E.cy = 0;
    E.numrows = 0;
    E.row = NULL;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main(int argc, char* argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    while(1) {
        editorRefreshScreen();
        editorProcessKeyPress();
    }

    return 0;
}
