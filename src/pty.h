#pragma once
#include <stdbool.h>

struct pty {
    int master;
    int pid;
};

bool pty_start(struct pty *p, int cols, int rows);
int pty_read(struct pty *p, char *buf, int cap);
void pty_write(struct pty *p, const char *data, int size);
void pty_resize(struct pty *p, int cols, int rows);
bool pty_reap(struct pty *p);
void pty_close(struct pty *p);
