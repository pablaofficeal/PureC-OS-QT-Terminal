#include "ptysession.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static const char *user_shell(void)
{
    const char *shell = getenv("SHELL");
    if (shell && *shell && access(shell, X_OK) == 0)
        return shell;
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_shell && *pw->pw_shell && access(pw->pw_shell, X_OK) == 0)
        return pw->pw_shell;
    return "/bin/sh";
}

bool pty_start(struct pty *p, int cols, int rows)
{
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    int slave = -1;
    p->master = -1;
    p->pid = -1;
    if (openpty(&p->master, &slave, NULL, NULL, &ws) < 0)
        return false;
    const char *shell = user_shell();
    pid_t pid = fork();
    if (pid < 0) {
        close(p->master);
        close(slave);
        p->master = -1;
        return false;
    }
    if (pid == 0) {
        setsid();
#ifdef TIOCSCTTY
        ioctl(slave, TIOCSCTTY, 0);
#endif
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2)
            close(slave);
        close(p->master);
        for (int s = 1; s < 32; ++s)
            signal(s, SIG_DFL);
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        const char *home = getenv("HOME");
        if (home && *home && chdir(home) != 0) {
        }
        const char *base = strrchr(shell, '/');
        execl(shell, base ? base + 1 : shell, (char *)NULL);
        _exit(127);
    }
    close(slave);
    p->pid = pid;
    fcntl(p->master, F_SETFL, fcntl(p->master, F_GETFL) | O_NONBLOCK);
    return true;
}

int pty_read(struct pty *p, char *buf, int cap)
{
    if (p->master < 0)
        return -1;
    ssize_t n = read(p->master, buf, (size_t)cap);
    if (n > 0)
        return (int)n;
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
        return 0;
    return -1;
}

void pty_write(struct pty *p, const char *data, int size)
{
    if (p->master < 0)
        return;
    for (int tries = 0; size > 0 && tries < 100; tries++) {
        ssize_t n = write(p->master, data, (size_t)size);
        if (n > 0) {
            data += n;
            size -= (int)n;
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
            break;
        } else {
            usleep(1000);
        }
    }
}

void pty_resize(struct pty *p, int cols, int rows)
{
    if (p->master < 0)
        return;
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    ioctl(p->master, TIOCSWINSZ, &ws);
#ifdef SIGWINCH
    if (p->pid > 0)
        kill(-p->pid, SIGWINCH);
#endif
}

bool pty_reap(struct pty *p)
{
    if (p->pid <= 0)
        return true;
    int status = 0;
    pid_t r = waitpid(p->pid, &status, WNOHANG);
    if (r == p->pid || (r < 0 && errno == ECHILD)) {
        p->pid = -1;
        return true;
    }
    return false;
}

void pty_close(struct pty *p)
{
    if (p->pid > 0) {
        kill(-p->pid, SIGHUP);
        kill(p->pid, SIGHUP);
        p->pid = -1;
    }
    if (p->master >= 0) {
        close(p->master);
        p->master = -1;
    }
}
