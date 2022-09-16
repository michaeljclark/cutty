#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <sys/ioctl.h>

static volatile int running = 1;

static void intr(int) { running = 0; }

static struct winsize ws;

static void initscr()
{
	ioctl(fileno(stdout), TIOCGWINSZ, &ws);
	printf("\x1b[2J");            /* clear screen */
}

static void linenum()
{
	for (size_t i = 1; i <= ws.ws_row; i++) {
		printf("%zu%s", i, i < ws.ws_row ? "\n" : "");
	}
}

static void capture()
{
	printf("\x1b[?25l");          /* hide cursor */
	printf("\x1b]555\x07");       /* screen capture */
	fflush(stdout);
	signal(SIGINT, intr);
	while(running);
	printf("\x1b[?25h");          /* show cursor */
	printf("\x1b[1;1H");          /* goto abs 1,1 */
	fflush(stdout);
	_Exit(0);
}
