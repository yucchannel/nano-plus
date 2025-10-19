#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define CTRL_KEY(k) ((k) & 0x1f)

struct termios orig_termios;
int screenrows, screencols;

void die(const char *s) {
    perror(s);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    exit(1);
}

void disableRawMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) return -1;
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

typedef struct {
    char **lines;
    size_t numlines;
} TextBuffer;

TextBuffer buffer = {NULL, 0};
char filename[256];
size_t cursor_x = 0, cursor_y = 0;

void editorOpen(const char *fname) {
    FILE *fp = fopen(fname, "r");
    if (!fp) {
        buffer.lines = malloc(sizeof(char*));
        buffer.lines[0] = calloc(1, 1);
        buffer.numlines = 1;
        return;
    }

    char *line = NULL;
    size_t len = 0;
    ssize_t read;
    buffer.lines = NULL;
    buffer.numlines = 0;

    while ((read = getline(&line, &len, fp)) != -1) {
        if (line[read-1] == '\n') line[read-1] = '\0';
        buffer.lines = realloc(buffer.lines, sizeof(char*) * (buffer.numlines +1));
        buffer.lines[buffer.numlines] = malloc(read);
        strcpy(buffer.lines[buffer.numlines], line);
        buffer.numlines++;
    }
    free(line);
    fclose(fp);
    strncpy(filename, fname, sizeof(filename)-1);
}

void editorSave() {
    FILE *fp = fopen(filename, "w");
    if (!fp) return;
    for (size_t i = 0; i < buffer.numlines; i++) {
        fwrite(buffer.lines[i], 1, strlen(buffer.lines[i]), fp);
        fputc('\n', fp);
    }
    fclose(fp);
}

void refreshScreen() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    for (size_t i = 0; i < buffer.numlines; i++) {
        write(STDOUT_FILENO, buffer.lines[i], strlen(buffer.lines[i]));
        write(STDOUT_FILENO, "\r\n", 2);
    }

    // status bar
    char status[128];
    snprintf(status, sizeof(status),
        "-- nano+ editor --  File: %s  | Ctrl+S Save | Ctrl+Q Quit --",
        filename);
    write(STDOUT_FILENO, status, strlen(status));

    char move[32];
    snprintf(move, sizeof(move), "\x1b[%zu;%zuH", cursor_y + 1, cursor_x + 1);
    write(STDOUT_FILENO, move, strlen(move));
}

void insertChar(char c) {
    char *line = buffer.lines[cursor_y];
    size_t len = strlen(line);
    line = realloc(line, len + 2);
    memmove(&line[cursor_x+1], &line[cursor_x], len - cursor_x + 1);
    line[cursor_x] = c;
    buffer.lines[cursor_y] = line;
    cursor_x++;
}

void insertNewline() {
    char *line = buffer.lines[cursor_y];
    size_t len = strlen(line);
    char *new_line = strdup(&line[cursor_x]);
    line[cursor_x] = '\0';
    buffer.lines[cursor_y] = line;

    buffer.lines = realloc(buffer.lines, sizeof(char*) * (buffer.numlines + 1));
    memmove(&buffer.lines[cursor_y+2], &buffer.lines[cursor_y+1], sizeof(char*) * (buffer.numlines - cursor_y - 1));
    buffer.lines[cursor_y+1] = new_line;
    buffer.numlines++;
    cursor_y++;
    cursor_x = 0;
}

void deleteChar() {
    if (cursor_x == 0 && cursor_y == 0) return;

    if (cursor_x > 0) {
        char *line = buffer.lines[cursor_y];
        memmove(&line[cursor_x-1], &line[cursor_x], strlen(line)-cursor_x+1);
        cursor_x--;
    } else { // merge with previous line
        size_t prev_len = strlen(buffer.lines[cursor_y-1]);
        size_t curr_len = strlen(buffer.lines[cursor_y]);
        buffer.lines[cursor_y-1] = realloc(buffer.lines[cursor_y-1], prev_len + curr_len +1);
        strcat(buffer.lines[cursor_y-1], buffer.lines[cursor_y]);
        free(buffer.lines[cursor_y]);
        memmove(&buffer.lines[cursor_y], &buffer.lines[cursor_y+1], sizeof(char*) * (buffer.numlines - cursor_y -1));
        buffer.numlines--;
        cursor_y--;
        cursor_x = prev_len;
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: nano+ <filename>\n");
        exit(1);
    }

    strncpy(filename, argv[1], sizeof(filename)-1);

    enableRawMode();
    getWindowSize(&screenrows, &screencols);
    editorOpen(argv[1]);
    if (!buffer.lines) buffer.lines = malloc(sizeof(char*));

    refreshScreen();

    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) die("read");

        if (iscntrl(c)) {
            if (c == CTRL_KEY('q')) break;
            else if (c == CTRL_KEY('s')) editorSave();
            else if (c == 127) deleteChar();
            else if (c == 10) insertNewline();
            else if (c == '\x1b') { // escape sequences
                char seq[3];
                if (read(STDIN_FILENO, &seq[0], 1) != 1) continue;
                if (read(STDIN_FILENO, &seq[1], 1) != 1) continue;
                if (seq[0] == '[') {
                    if (seq[1] == 'C' && cursor_x < strlen(buffer.lines[cursor_y])) cursor_x++;
                    else if (seq[1] == 'D' && cursor_x > 0) cursor_x--;
                    else if (seq[1] == 'A' && cursor_y > 0) {
                        cursor_y--;
                        cursor_x = cursor_x > strlen(buffer.lines[cursor_y]) ? strlen(buffer.lines[cursor_y]) : cursor_x;
                    }
                    else if (seq[1] == 'B' && cursor_y < buffer.numlines-1) {
                        cursor_y++;
                        cursor_x = cursor_x > strlen(buffer.lines[cursor_y]) ? strlen(buffer.lines[cursor_y]) : cursor_x;
                    }
                }
            }
        } else {
            insertChar(c);
        }
        refreshScreen();
    }

    disableRawMode();
    printf("\n[Exited nano+]\n");

    return 0;
}
