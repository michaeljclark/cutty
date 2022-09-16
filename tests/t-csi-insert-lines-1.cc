#include "capture.h"

int main()
{
    initscr();
    linenum();
    printf("\x1b[10;1H");         /* goto abs 10,1 */
    printf("\x1b[4L");            /* insert 4 lines */
    printf("inserted 4 lines\n");
    capture();
}