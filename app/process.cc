#include <cstdlib>
#include <cerrno>
#include <cassert>
#include <climits>

#include <time.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/param.h>

#if defined(__linux__)
#include <pty.h>
#endif
#if defined(__APPLE__)
#include <util.h>
#endif
#if defined(__FreeBSD__)
#include <libutil.h>
#endif

#include "app.h"
#include "utf8.h"
#include "colors.h"

#include "timestamp.h"
#include "teletype.h"
#include "process.h"

tty_process* tty_process_new()
{
    return new tty_process{};
}

int tty_process::exec(tty_winsize zws, const char *path, const char *const argv[], bool go_home)
{
    struct winsize ws;
    struct termios tio;

    this->zws = zws;

    memset(&ws, 0, sizeof(ws));
    ws.ws_row = zws.vis_rows;
    ws.ws_col = zws.vis_cols;
    ws.ws_xpixel = zws.pix_width;
    ws.ws_ypixel = zws.pix_height;

    memset(&tio, 0, sizeof(tio));
    tio.c_lflag = ICANON | ISIG | IEXTEN | ECHO | ECHOE | ECHOKE | ECHOCTL;
    tio.c_iflag = ICRNL | IXON | IXANY | IMAXBEL | IUTF8 | BRKINT;
    tio.c_oflag = OPOST | ONLCR;
    tio.c_cflag = CREAD | CS8 | HUPCL;

    tio.c_cc[VINTR] = CTRL('c');
    tio.c_cc[VQUIT] = CTRL('\\');
    tio.c_cc[VERASE] = 0177; /* DEL */
    tio.c_cc[VKILL] = CTRL('u');
    tio.c_cc[VEOF] = CTRL('d');
    tio.c_cc[VEOL] = 255;
    tio.c_cc[VEOL2] = 255;
    tio.c_cc[VSTART] = CTRL('q');
    tio.c_cc[VSTOP] = CTRL('s');
    tio.c_cc[VSUSP] = CTRL('z');
    tio.c_cc[VREPRINT] = CTRL('r');
    tio.c_cc[VWERASE] = CTRL('w');
    tio.c_cc[VLNEXT] = CTRL('v');
    tio.c_cc[VDISCARD] = CTRL('o');
#if defined(__APPLE__) || defined(__FreeBSD__)
    tio.c_cc[VDSUSP] = CTRL('y');
    tio.c_cc[VSTATUS] = CTRL('t');
#endif
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;

    cfsetispeed(&tio, B9600);
    cfsetospeed(&tio, B9600);

    int cwd;
    char *home;

    /* save working directory and change to home directory */
    if (go_home) {
        if ((cwd = open(".", O_RDONLY)) < 0) {
            Error("tty_process::forkpty: open: %s\n", strerror(errno));
            return -1;
        }
        if ((home = getenv("HOME")) == NULL) {
            Error("tty_process::forkpty: getenv: HOME not found\n");
            return -1;
        }
        if (chdir(home) < 0) {
            Error("tty_process::forkpty: chdir: %s\n", strerror(errno));
            return -1;
        }
    }

    switch ((pid = ::forkpty((int*)&fd, device, &tio, &ws))) {
    case -1:
        Error("tty_process::forkpty: forkpty: %s\n", strerror(errno));
    case 0:
        setenv("TERM", "xterm-256color", 1);
        setenv("LC_CTYPE", "UTF-8", 0);
        execvp(path, (char *const *)argv);
        _exit(1);
    }

    /* restore previous working directory */
    if (go_home) {
        if (fchdir(cwd) < 0) {
            Error("tty_process::forkpty: fchdir: %s\n", strerror(errno));
            return -1;
        }
        if (close(cwd) < 0) {
            Error("tty_process::forkpty: close: %s\n", strerror(errno));
            return -1;
        }
    }

    Debug("tty_process::forkpty: pid=%d path=%s argv0=%s fd=%d rows=%d cols=%d device=%s\n",
        pid, path, argv[0], fd, ws.ws_row, ws.ws_col, device);

    return fd;
}

bool tty_process::winsize(tty_winsize zws)
{
    struct winsize ws;
    pid_t pgrp;

    if (this->zws == zws) return false;

    Debug("tty_process::winsize: size changed: %dx%d -> %dx%d\n",
        this->zws.vis_cols, this->zws.vis_rows, zws.vis_cols, zws.vis_rows);

    this->zws = zws;

    memset(&ws, 0, sizeof(ws));
    ws.ws_col = zws.vis_cols;
    ws.ws_row = zws.vis_rows;
    ws.ws_xpixel = zws.pix_width;
    ws.ws_ypixel = zws.pix_height;

    if (ioctl(fd, TIOCSWINSZ, (char *) &ws) < 0) {
        Error("tty_process::winsize: ioctl(TIOCSWINSZ) failed: %s\n",
            strerror(errno));
        return false;
    }
    if (ioctl(fd, TIOCGPGRP, &pgrp) < 0) {
        Error("tty_process::winsize: ioctl(TIOCGPGRP) failed: %s\n",
            strerror(errno));
        return false;
    }
    if (kill(-pgrp, SIGWINCH) < 0) {
        Error("tty_process::winsize: kill(%d,SIGWINCH) failed: %s\n",
            -pgrp, strerror(errno));
    }

    return true;
}
