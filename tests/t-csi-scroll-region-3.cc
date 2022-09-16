#include "capture.h"

int main()
{
    initscr();
    linenum();
    printf("\x1b[15;16r");       /* set scroll region 15,16 */
    printf("\x1b[15;1H");        /* goto abs 15,1 */
    printf("\x1b[M");            /* delete 1 lines */
    capture();
}