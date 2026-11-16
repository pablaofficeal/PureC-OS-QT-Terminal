#include "ptysession.h"

#include <errno.h>
#include <fcntl.h>
extern "C" {
#include <pty.h>
}
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

PtySession::PtySession(QObject *parent)
    : QObject(parent)
{
    m_reaper.setInterval(250);
    connect(&m_reaper, &QTimer::timeout, this, &PtySession::reap);
}

PtySession::~PtySession()
{
    if (m_pid > 0) {
        kill(-m_pid, SIGHUP);
        kill(m_pid, SIGHUP);
    }
    if (m_master >= 0)
        close(m_master);
}

static const char *userShell()
{
    const char *shell = getenv("SHELL");
    if (shell && *shell && access(shell, X_OK) == 0)
        return shell;
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_shell && *pw->pw_shell && access(pw->pw_shell, X_OK) == 0)
        return pw->pw_shell;
    return "/bin/sh";
}

bool PtySession::start(int cols, int rows)
{
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    int slave = -1;
    if (openpty(&m_master, &slave, nullptr, nullptr, &ws) < 0)
        return false;
    const char *shell = userShell();
    pid_t pid = fork();
    if (pid < 0) {
        close(m_master);
        close(slave);
        m_master = -1;
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
        close(m_master);
        for (int s = 1; s < 32; ++s)
            signal(s, SIG_DFL);
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        unsetenv("QT_QPA_PLATFORM");
        const char *home = getenv("HOME");
        if (home && *home)
            (void)chdir(home);
        const char *base = strrchr(shell, '/');
        execl(shell, base ? base + 1 : shell, (char *)nullptr);
        _exit(127);
    }
    close(slave);
    m_pid = pid;
    fcntl(m_master, F_SETFL, fcntl(m_master, F_GETFL) | O_NONBLOCK);
    m_notifier = new QSocketNotifier(m_master, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &PtySession::readable);
    m_reaper.start();
    return true;
}

void PtySession::readable()
{
    char buf[16384];
    for (int i = 0; i < 8; ++i) {
        ssize_t n = read(m_master, buf, sizeof(buf));
        if (n > 0) {
            emit received(QByteArray(buf, int(n)));
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
            m_notifier->setEnabled(false);
            reap();
        }
        break;
    }
}

void PtySession::reap()
{
    if (m_pid <= 0)
        return;
    int status = 0;
    pid_t r = waitpid(m_pid, &status, WNOHANG);
    if (r == m_pid || (r < 0 && errno == ECHILD)) {
        m_pid = -1;
        m_reaper.stop();
        if (m_notifier)
            m_notifier->setEnabled(false);
        emit finished(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
    }
}

void PtySession::write(const QByteArray &data)
{
    if (m_master < 0)
        return;
    const char *p = data.constData();
    qsizetype left = data.size();
    for (int tries = 0; left > 0 && tries < 100; ++tries) {
        ssize_t n = ::write(m_master, p, size_t(left));
        if (n > 0) {
            p += n;
            left -= n;
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
            break;
        } else {
            usleep(1000);
        }
    }
}

void PtySession::resize(int cols, int rows)
{
    if (m_master < 0)
        return;
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    ioctl(m_master, TIOCSWINSZ, &ws);
#ifdef SIGWINCH
    if (m_pid > 0)
        kill(-m_pid, SIGWINCH);
#endif
}
