#pragma once

struct tty_process
{
    tty_winsize zws;
    int pid;
    int fd;
    char device[PATH_MAX];

    int exec(tty_winsize zws, const char *path, const char *const argv[], bool go_home = false);
    bool winsize(tty_winsize zws);
};

tty_process* tty_process_new();
