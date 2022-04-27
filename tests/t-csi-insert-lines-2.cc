#include <cstdio>
#include "capture.h"

int main()
{
	printf("\x1b[2J");            /* clear screen */
	for (size_t i = 1; i <= 24; i++) {
		printf("%zu%s", i, i < 24 ? "\n" : "");
	}
	printf("\x1b[1;22r");         /* set scroll region */
	printf("\x1b[1;1H");          /* goto abs 1,1 */
	printf("\x1b[11L");           /* insert 11 lines */
	printf("\x1b[12;1H");         /* goto abs 12,1 */
	printf("inserted 11 lines\n");
	capture();
}