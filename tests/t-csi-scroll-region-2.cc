#include "capture.h"

int main()
{
    initscr();
    linenum();
    printf("\x1b[15;23r");        /* set scroll region */
    printf("\x1b[23;1H");         /* goto abs 23,1 */
    printf("\r\n");               /* CR LF */

    capture();
}