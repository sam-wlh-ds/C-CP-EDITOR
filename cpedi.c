/* Includes */
// macros decide what features to expose
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include<ctype.h>
#include<errno.h>
#include<fcntl.h>
#include<math.h>
#include<stdio.h>
#include<stdarg.h>
#include<stdlib.h>
#include<string.h>
#include<sys/ioctl.h>
#include<sys/types.h>
#include<termio.h>
#include<time.h>
#include<unistd.h>

/* Defines */

#define CPEDI_VERSION "0.0.1"
#define CPEDI_TAB_STOP 4
#define CPEDI_QUIT_TIMES 2
#define CPEDI_READ_WAIT_TIME 1000

#define CTRL_KEY(k) ((k) & 0x1f) // sets the upper 3 bits of the character to 0

enum editorKey {
    BACKSPACE = 127,
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

/* Data */

typedef struct erow // editor row
{
    int size;
    int rsize; // size of contents of render
    char* chars;
    char* render;
} erow;


struct editorConfig
{
    int cx, cy; // position of cursor => zero based indexing => index into chars field of an erow
    int rx; // index into the render field
    int rowoff; // refers to what’s at the top of the screen => zero based
    int coloff; // refers to what's at the left of the screen=> zero based
    int screenrows; // rows in the terminal => 1 based indexing
    int screencols; // cols in the terminal => 1 based indexig
    int numrows; // number of rows in the file
    erow *row; // array of rows
    int dirty; // keep track if file is modified
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios; // config of orginal terminal   
};

struct editorConfig E;

/* prototypes */
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt);
char* editorRowsToString(int *buflen);

/* Math */
int imin(int a, int b){
    if (a>=b) return b;
    return a;
}

int imax(int a, int b){
    if (a<=b) return b;
    return a;
}

int countDigits(int a){
    static int maxDigit = 0;
    int digits = floor( log10( (double)a ) ) + 1;
    maxDigit = imax(maxDigit, digits);
    return maxDigit;
}

/* Basic */
int getRowLength(){
    erow *row = (E.cy >= E.numrows) ? NULL: &E.row[E.cy];
    int rowlen = row? row->size:0;
    return rowlen;
}

/* Terminal */

void die(const char *s){
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    //perror() looks at the global errno variable and prints a descriptive error message for it
    perror(s); //also prints the string given to it before it prints the error message, which is meant to provide context
    write(STDERR_FILENO, "\r\n", 1);
    exit(1); // non-zero value exit => failure
}

void disableRawMode(){
    // set back to default
    if (tcsetattr(STDERR_FILENO, TCIFLUSH, &E.orig_termios) == -1){
        die("tcsetattr failed while disabling raw mode");
    }
}

void enableRawMode(){
    // get terminal attributes
    if(tcgetattr(STDERR_FILENO, &E.orig_termios)==-1){
        die("tcgetattr failed while enabling raw mode");
    } 
    atexit(disableRawMode);

    struct termios raw = E.orig_termios; // make a copy to modify

    // c_lflag => local flags | c_iflags => input flags | c_oflag => output flags | c_cflag => control flag

    // Turn OFF: IXON: Turns of Ctrl-S && Ctrl-Q which controls the transmission of data to terminal
    // Turn OFF: ICRNL: Fixes Ctrl-M, now it and enter is read as 13
    // Turn OFF: BRKINT: BRKINT is turned on, a break condition will cause a SIGINT
    // Turn OFF: INPCK: INPCK enables parity checking, which doesn’t seem to apply to modern terminal emulators.
    // Turn OFF: ISTRIP: causes the 8th bit of each input byte to be stripped, meaning it will set it to 0
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    // Turn OFF: OPOST: Avoids "\n" to "\r\n" transaltion in output
    // "\r" brings the cursor back to the start of the line before "\n"
    raw.c_oflag &= ~(OPOST);

    /*CS8 is not a flag, it is a bit mask with multiple bits, which we set using the bitwise-OR (|) operator unlike all the flags we are turning off. It sets the character size (CS) to 8 bits per byte.*/
    raw.c_cflag |= (CS8);

    // Turn OFF: ECHO: Causes each key typed to be printed in terminal
    // Turn OFF: ICANON: So that we can input byte-by-byte instead of line by line
    // Turn OFF: ISIG: To Disable Ctrl-C (Send SIGINT signal for termination) && Ctrl-Z (sends SIGTSTP signal to suspend)
    // Turn OFF: IEXTEN: To Disable Ctrl-V
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN |ISIG); // Flip only the ECHO bit (4th) and others in or off in the flags 

    // VMIN value sets the minimum number of bytes of input needed before read() can return
    raw.c_cc[VMIN] = 0; // set to 0 as that read() returns as soon as there is any input
    // VTIME value sets the maximum amount of time to wait before read() returns.
    raw.c_cc[VTIME] = CPEDI_READ_WAIT_TIME; // here 500 millisecond;

    // set terminal attributes
    if (tcsetattr(STDERR_FILENO, TCSAFLUSH, &raw) == -1){
        die("tcsetattr failed while enabling raw mode");
    }
    
}

// wait for one keypress, and return it
int editorReadKey(){
    int nread;
    char c;
    while((nread = read(STDIN_FILENO, &c, 1)) != 1){
        if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN){
            die("Error while reading from terminal");
        }
    }

    if (c == '\x1b'){
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        // ecs[5~
        if (seq[0] == '['){
            if (seq[1] >= '0' && seq[1] <= '9'){
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~'){
                    switch (seq[1])
                    {   
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;

                    }
                }
            } else {
                switch (seq[1])
                {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
                case 'F': return END_KEY;
                case 'H': return HOME_KEY;
                }
            }
        } else if (seq[0] == 'O'){
            switch (seq[1])
            {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return '\x1b';
    }

    return c;
}

int getCursorPosition(int *rows, int *cols){
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
    // printf("\r\n");

    while (i < sizeof(buf) - 1)
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    // read formatted input from a string, sscanf returns the number of input items successfully matched and assigned
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols){
    struct winsize ws;

    // TIOCGWINSZ => Terminal Input/Output Control Get WINdow SiZe.
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)==-1 || ws.ws_col == 0){
        // Brute force measure size of terminal
        // C => moves cursor forward, B => moves cursor down } x999 times [these commands cannot exceed boundary of terminal]
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/* row operations */

int editorRowCxtoRx(erow *row, int cx){
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++){
        if (row->chars[j] == '\t'){
            rx += (CPEDI_TAB_STOP - 1) - (rx%CPEDI_TAB_STOP);
        }
        rx++;
    }
    return rx;
}

void editorUpdateRow(erow *row){
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++){
        if (row->chars[j] == '\t') tabs++;
    }

    free(row->render);
    row->render = malloc(row->size + tabs*(CPEDI_TAB_STOP-1) +1);

    int idx = 0;
    for (j = 0; j < row->size; j++){
        if (row->chars[j] == '\t'){
            row->render[idx++] = ' ';
            while(idx%CPEDI_TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorInsertRow(int at, char *s, size_t len){
    if (at < 0 || at > E.numrows) return;

    E.row = realloc(E.row, sizeof(erow)*(E.numrows+1));
    memmove(&E.row[at+1], &E.row[at], sizeof(erow)*(E.numrows-at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len+1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row){
    free(row->render);
    free(row->chars);
}

void editorDelRow(int at){
    if (at < 0 || at >= E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows-at-1));
    E.numrows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c){
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size+2); // 1 extra byte for NULL Character
    // Copy N bytes of SRC to DEST, guaranteeing correct behavior for overlapping strings.
    memmove(&row->chars[at+1], &row->chars[at], row->size-at+1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len){
    row->chars = realloc(row->chars, row->size+len+1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at){
    if (at < 0 || at >= row->size) return;
    // Null byte gets included in memmove
    if ((row->chars[at] == '(' && at+1<row->size && row->chars[at+1] == ')')
        || (row->chars[at] == '{' && at+1<row->size && row->chars[at+1] == '}')
        || (row->chars[at] == '[' && at+1<row->size && row->chars[at+1] == ']')
        || (row->chars[at] == '\'' && at+1<row->size && row->chars[at+1] == '\'')
        || (row->chars[at] == '"' && at+1<row->size && row->chars[at+1] == '"')){
        editorRowDelChar(row, at+1);
    } 
    memmove(&row->chars[at], &row->chars[at+1], row->size-at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/* Editor Operations */

void editorInsertChar(int c){
    if (E.cy == E.numrows){
        editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], (E.cx-countDigits(E.numrows)), c);
    E.cx++;
}

void editorInsertNewLine(){
    erow *row = &E.row[E.cy];
    int tabs = 0, j = 0;
    while (row->chars[j] != '\0' && row->chars[j] == '\t')
    {
        tabs++;
        j++;
    }

    if ((E.cx-countDigits(E.numrows)) == 0){
        editorInsertRow(E.cy, "", 0);
    } else {
        editorInsertRow(E.cy+1, &row->chars[(E.cx-countDigits(E.numrows))], row->size-(E.cx-countDigits(E.numrows)));
        row = &E.row[E.cy];
        row->size = (E.cx-countDigits(E.numrows));
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = countDigits(E.numrows);
    while(tabs--) editorInsertChar('\t');
}

void editorDelChar(){
    if (E.cy == E.numrows) return;
    if ((E.cx-countDigits(E.numrows)) == 0 && E.cy == 0) return;

    erow *row = &E.row[E.cy];
    if ((E.cx-countDigits(E.numrows)) > 0){
        editorRowDelChar(row, (E.cx-countDigits(E.numrows)) - 1);
        E.cx--;
    } else {
        E.cx = countDigits(E.numrows) + E.row[E.cy-1].size;
        editorRowAppendString(&E.row[E.cy-1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

void editorTeleport(){
    char *line = editorPrompt("Line Number : %s");
    int l = atoi(line);
    if (l < E.numrows){
        E.cy = l-1;
        E.cx = countDigits(E.numrows)+getRowLength();
    }
}

// This function is written by Claude AI as the earlier version of this function was not suitable for copying
// characters involved in the code, not sure what sorcery it did here, something realted to xclip - linux
void editorCopyToClipboard(const char *text, int len) {
    // Create a pipe for communication with xclip
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        return;
    }
    
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }
    
    if (pid == 0) {  // Child process
        close(pipefd[1]);  // Close write end
        
        // Redirect stdin to read from pipe
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        
        // Execute xclip
        execlp("xclip", "xclip", "-selection", "clipboard", NULL);
        
        // If we get here, exec failed
        perror("execlp");
        exit(EXIT_FAILURE);
    } else {  // Parent process
        close(pipefd[0]);  // Close read end
        
        // Write the text to the pipe
        write(pipefd[1], text, len);
        close(pipefd[1]);
        
        // Wait for child to finish
        waitpid(pid, NULL, 0);
    }
}

void editorCopyAll(){
    int len;
    char *buf = editorRowsToString(&len);
    buf[len] = '\0';
    editorCopyToClipboard(buf, len);
    editorSetStatusMessage("Copied to Clipboard Successfully: %d", len);
}
/* File i/o */

char* editorRowsToString(int *buflen){
    int totlen = 0;
    int j;
    for (j = 0; j < E.numrows; j++){
        totlen += E.row[j].size + 1;
    }
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf; // pointer to navigate buf string
    for (j = 0; j < E.numrows; j++){
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    return buf;
}

void editorOpen(char *filename){
    free(E.filename);
    E.filename = strdup(filename); // duplicate the string

    FILE *fp = fopen(filename, "r");
    if (!fp) die("editorOpen: Error while opening file");
   
    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen = 0;
    while ((linelen = getline(&line, &linecap, fp))!=-1){
        while (linelen > 0 && (line[linelen-1] == '\r' || line[linelen-1] == '\n'))
        {
            linelen--;
        }
        editorInsertRow(E.numrows, line, linelen);
    }   
    free(line);
    fclose(fp);
    E.dirty = 0;
    if (strcmp(E.filename,"cp/template.cpp")==0){
        E.cy = 45;
        editorScroll();
        E.cy = 30;
        E.cx = countDigits(E.numrows);   
        editorInsertChar('\t');
    }
}

void editorSave(int newFile){
    if (E.filename == NULL || newFile) {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)");
        if (E.filename == NULL){
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char *buf = editorRowsToString(&len);

    // Open or create the file with specified filename
    // 0644 => Standard permission for textfiles (Owner gets rw other get r)
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1){
        // sets the file's size to the specified length cut off extra data or add '0' bytes at the end 
        if (ftruncate(fd, len) != -1){
            if (write(fd, buf, len) == len){
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d File Saved Successfully!", len);
                return;
            }
        } 
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/* Append Buffer */

struct abuf
{
    char *b;
    int len;
};

#define ABUF_INIT {NULL,0}

void abAppend(struct abuf *ab, const char * s, int len){
    // allocate enough memory to hold the new string
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;
    // copy the string s after the end of the current data
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab){
    free(ab->b);
}

/* Output */

void editorScroll(){
    E.rx = countDigits(E.numrows);
    if (E.cy < E.numrows){
        E.rx = countDigits(E.numrows) + editorRowCxtoRx(&E.row[E.cy], (E.cx-countDigits(E.numrows)));
    }

    if (E.cy < E.rowoff){
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows){
        // As screenrows is 1 based indexing
        E.rowoff = E.cy - E.screenrows+1;
    }
    if (E.rx-countDigits(E.numrows) < E.coloff){
        E.coloff = E.rx-countDigits(E.numrows);
    } 
    if (E.rx-countDigits(E.numrows) >= E.coloff + E.screencols){
        E.coloff = E.rx-countDigits(E.numrows) - E.screencols+1;
    }
}

void editorDrawLineNumber(struct abuf *ab, int line){
    int dig = countDigits(E.numrows);
    char num[dig+10];
    int len = snprintf(num, sizeof(num), "\x1b[7m%d\x1b[m", line); // 4 + x + 3
    while (len<dig+7)
    {
        // 0123456789(10)
        memmove(&num[5], &num[4], len-4);
        num[4] = ' ';
        len++;
    }
    num[len] = '\0';
    abAppend(ab, num, len);
}

void editorDrawRows(struct abuf *ab){
    int y;
    for (y = 0; y < E.screenrows; y++){
        int filerow = y + E.rowoff;
        if (y >= E.numrows){
            if (E.numrows == 0 && y == 0){
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), 
                    "CP Editor -- version %s", CPEDI_VERSION);
                
                welcomelen = imin(welcomelen, E.screencols);
                int padding = (E.screencols-welcomelen)/2;
                if (padding){
                    editorDrawLineNumber(ab, y+1);
                    padding--;
                    while (padding--)
                    {
                        abAppend(ab, " ", 1);
                    }
                }
                abAppend(ab, welcome, welcomelen);
            }
            else{
                // abAppend(ab, "~", 1);
                editorDrawLineNumber(ab, E.rowoff+y+1);
            }
        } else {
            int len = E.row[filerow].rsize - E.coloff;
            len = imax(0, len);
            len = imin(len, E.screencols);
            editorDrawLineNumber(ab, E.rowoff+y+1);
            abAppend(ab, &E.row[filerow].render[E.coloff], len);
        }

        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab){
    // <esc>[7m switches to inverted colours and <esc>[m switches back to normal
    abAppend(ab, "\x1b[7m", 4); 
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), " %.20s - %d lines %s",
        E.filename ? E.filename : "[No Name]", E.numrows, 
        E.dirty ? "(modified)":"");
    int rlen = snprintf(rstatus, sizeof(rstatus), "R: %d C: %d",
        E.cy+1, E.rx-countDigits(E.numrows)+1);
    len = imin(len, E.screencols);
    abAppend(ab, status, len);
    while (len < E.screencols)
    {
        if (E.screencols - len - 1 == rlen){
            abAppend(ab, rstatus, rlen);
            len += rlen;
        }
        abAppend(ab, " " , 1);
        len++;
    }
    abAppend(ab, "\x1b[m", 3);
}

void editorDrawMessageBar(struct abuf *ab, const int duration){
    abAppend(ab, "\x1b[7m", 4); 
    abAppend(ab, "\x1b[K", 3); // Clear the message bar
    int msglen = strlen(E.statusmsg);
    msglen = imin(msglen, E.screencols);
    int len = 0;
    while (len < E.screencols)
    {
        // draw message which is less than 5 second old
        if (len == (E.screencols-msglen)/2 && time(NULL) - E.statusmsg_time < duration){
            abAppend(ab, E.statusmsg, msglen);
            len += msglen;
        }
        abAppend(ab, " " , 1);
        len++;
    }
   
    abAppend(ab, "\r\n", 2);
}

void editorRefreshScreen(){
    editorScroll();

    struct abuf ab = ABUF_INIT;
    // Write 4 bytes out to the terminal. 
    // \x1b is escape character (27 in decimal) => escape sequence => <esc>[
    // <esc>[2J => clear entire screen , <esc>[1J => clear the screen up to where the cursor is, <esc>[0J or <esc>[J => would clear the screen from the cursor up to the end of the screen 
    abAppend(&ab, "\x1b[?25l", 6); // to hide the cursor which printing
    // abAppend(&ab, "\x1b[2J", 4);
    // H command (Cursor Position) to position the cursor.
    // <esc>[row;colH => default: <esc>[1;1H
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawMessageBar(&ab, 5);
    editorDrawStatusBar(&ab);

    char buf[32];
    // Terminal uses 1 based indexing, poisition the cursor
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy-E.rowoff)+1, (E.rx-E.coloff)+1);
    abAppend(&ab, buf, strlen(buf));
    
    abAppend(&ab, "\x1b[?25h",6); // to unhide the cursor after the printing is done

    // Write the buffer to the terminal
    write(STDOUT_FILENO, ab.b, ab.len);
    // free the buffer
    abFree(&ab);
}

// variadic function, meaning it can take any number of arguments
void editorSetStatusMessage(const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/* Input */

char *editorPrompt(char *prompt){
    size_t bufsize = 128;
    char* buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (1)
    {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE){
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b'){
            editorSetStatusMessage("");
            free(buf);
            return NULL;
        } else if (c == '\r'){
            if (buflen != 0){
                editorSetStatusMessage("");
                return buf;
            }
        } else if(!iscntrl(c) && c < 128){
            if (buflen == bufsize-1){
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
    }
    
}

void editorMoveCursor(int key){
    erow *row = (E.cy >= E.numrows) ? NULL: &E.row[E.cy];

    switch (key)
    {
    case ARROW_LEFT:
        if ((E.cx-countDigits(E.numrows)) != 0) E.cx--;
        else if (E.cy > 0){
            E.cy--;
            E.cx = countDigits(E.numrows) + E.row[E.cy].size;
        }
        break;
    case ARROW_RIGHT:
        if (row && (E.cx-countDigits(E.numrows)) < row->size){
            E.cx++;
        } else if (E.cy+1 < E.numrows) {
            E.cx = countDigits(E.numrows);
            editorMoveCursor(ARROW_DOWN);
        }
        break;
    case ARROW_UP:
        if (E.cy != 0) E.cy--;
        break;
    case ARROW_DOWN:
        if (E.cy < E.numrows-1) E.cy++;
        break;
    }

    E.cx = countDigits(E.numrows) + imin((E.cx-countDigits(E.numrows)), getRowLength());
}

void editorProcessKeypress(){
    static int quit_times = CPEDI_QUIT_TIMES;

    int c = editorReadKey();

    switch (c)  
    {
        case '\r':
            editorInsertNewLine();
            break;
        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0){
                editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                     "Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('s'):
            editorSave(0);
            break;
        
        case CTRL_KEY('w'):
            editorSave(1);
            break;
        
        case CTRL_KEY('d'):
            editorTeleport();
            break;
        
        case CTRL_KEY('a'):
            editorCopyAll();
            break;
            
        case ARROW_UP:
        case ARROW_LEFT:
        case ARROW_DOWN:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP){
                    E.cy = E.rowoff;
                } else if (c == PAGE_DOWN){
                    E.cy = E.rowoff + E.screenrows - 1;
                    E.cy = imin(E.cy, E.numrows);
                }
                int times = E.screenrows;
                while (times--)
                {
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP: ARROW_DOWN);
                }
                break;
            }
        
        case HOME_KEY:
            E.cx = countDigits(E.numrows);
            break;
        case END_KEY:
            E.cx = countDigits(E.numrows) + getRowLength();
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) {
                int temp = E.cx;
                editorMoveCursor(ARROW_RIGHT);
                if (temp == E.cx) break;
            }
            editorDelChar();
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        case '"':
        case '\'':
            editorInsertChar(c);
            c=='"'? editorInsertChar('"'): editorInsertChar('\'');
            E.cx--;
            break;
        
        case '[':
        case '(':
            editorInsertChar(c);
            c=='('? editorInsertChar(')'): editorInsertChar(']');
            E.cx--;
            break;
        
        case '{':
            editorInsertChar(c);
            editorInsertChar('}');
            E.cx--;
            editorInsertNewLine();editorInsertNewLine();
            E.cy--; editorInsertChar('\t');
            break;
            
        
        default:
            editorInsertChar(c);
            break;
    }

    quit_times = CPEDI_QUIT_TIMES;

}

/* Init */

void initEditor(){
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.dirty = 0;
    E.row = NULL;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if (getWindowSize(&E.screenrows, &E.screencols)==-1){
        die("initEditor: getWindowSize failed to get size of terminal");
    }
    E.screenrows -= 2; // Room for status bar at the bottom
    E.cx = E.rx = countDigits(E.screenrows);
}

int main(int argc, char* argv[]){
    system("clear");
    enableRawMode();
    initEditor();
    if (argc >= 2){
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("COMMANDS: ^Q = quit | ^S = save | ^W = Save As | ^D: Teleport | ^A: Copy All");

    while (1)
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    
    return 0;
}
