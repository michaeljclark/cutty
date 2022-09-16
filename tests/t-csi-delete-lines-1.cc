#include "capture.h"

int main()
{
    initscr();
    linenum();
    printf("\x1b[10;1H");         /* goto abs 10,1 */
    printf("\x1b[4M");            /* delete 4 lines */
    printf("deleted 4 lines\n");
    capture();
}