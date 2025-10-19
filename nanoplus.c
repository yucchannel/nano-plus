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
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
        die("tcsetattr");
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
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1)
        return -1;
    else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

char *buffer = NULL;
size_t bufsize = 0;
size_t cursor_x = 0, cursor_y = 0;
char filename[256];

void editorOpen(const char *fname) {
    FILE *fp = fopen(fname, "r");
    if (!fp) {
        buffer = calloc(1, 1);
        bufsize = 0;
        return;
    }
    fseek(fp, 0, SEEK_END);
    bufsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buffer = malloc(bufsize + 1);
    fread(buffer, 1, bufsize, fp);
    buffer[bufsize] = '\0';
    fclose(fp);
    strncpy(filename, fname, sizeof(filename)-1);
}

void editorSave() {
    FILE *fp = fopen(filename, "w");
    if (!fp) return;
    fwrite(buffer, 1, strlen(buffer), fp);
    fclose(fp);
}

void refreshScreen() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    if (buffer) write(STDOUT_FILENO, buffer, strlen(buffer));

    // Draw status bar
    char status[128];
    snprintf(status, sizeof(status),
        "\r\n-- nano+ editor --  File: %s  | Ctrl+S Save | Ctrl+Q Quit --",
        filename);
    write(STDOUT_FILENO, status, strlen(status));

    // Move cursor
    char move[32];
    snprintf(move, sizeof(move), "\x1b[%zu;%zuH", cursor_y + 1, cursor_x + 1);
    write(STDOUT_FILENO, move, strlen(move));
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
    if (!buffer) buffer = calloc(1, 1);

    refreshScreen();

    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) die("read");

        if (iscntrl(c)) {
            if (c == CTRL_KEY('q')) break;
            else if (c == CTRL_KEY('s')) editorSave();
            else if (c == 127) {  // Backspace
                if (cursor_x > 0 && strlen(buffer) > 0) {
                    memmove(&buffer[cursor_x - 1],
                            &buffer[cursor_x],
                            strlen(buffer) - cursor_x + 1);
                    cursor_x--;
                }
            } else if (c == '\x1b') {  // Arrow keys
                char seq[3];
                if (read(STDIN_FILENO, &seq[0], 1) != 1) continue;
                if (read(STDIN_FILENO, &seq[1], 1) != 1) continue;

                if (seq[0] == '[') {
                    if (seq[1] == 'D' && cursor_x > 0) cursor_x--;          // Left
                    else if (seq[1] == 'C' && cursor_x < strlen(buffer)) cursor_x++; // Right
                }
            }
        } else {
            size_t len = strlen(buffer);
            buffer = realloc(buffer, len + 2);
            memmove(&buffer[cursor_x + 1], &buffer[cursor_x], len - cursor_x + 1);
            buffer[cursor_x] = c;
            cursor_x++;
        }
        refreshScreen();
    }

    disableRawMode();
    printf("\n[Exited nano+]\n");
    return 0;
}
