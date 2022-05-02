#include <cstdio>
#include "capture.h"

int main()
{
	printf("\x1b[2J");            /* clear screen */
	for (size_t i = 1; i <= 24; i++) {
		printf("%zu%s", i, i < 24 ? "\n" : "");
	}
	printf("\x1b[10;1H");         /* goto abs 10,1 */
	printf("\x1b[4L");            /* insert 4 lines */
	printf("inserted 4 lines\n");
	capture();
}