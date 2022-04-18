#include <csignal>
#include <cstdlib>

static volatile int running = 1;

static void intr(int) { running = 0; }

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
