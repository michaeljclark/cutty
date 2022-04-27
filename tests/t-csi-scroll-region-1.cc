#include <cstdio>
#include "capture.h"

int main()
{
	printf("\x1b[2J");            /* clear screen */
	for (size_t i = 1; i <= 24; i++) {
		printf("%zu%s", i, i < 24 ? "\n" : "");
	}
	printf("\x1b[1;23r");         /* set scroll region */
	printf("\x1b[23;1H");         /* goto abs 23,1 */
	printf("\r\n");               /* CR LF */
	capture();
}