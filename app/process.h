#pragma once

struct cu_process
{
	cu_winsize zws;
	int pid;
	int fd;
	char device[PATH_MAX];

	int exec(cu_winsize zws, const char *path, const char *const argv[]);
	bool winsize(cu_winsize zws);
};

cu_process* cu_process_new();
