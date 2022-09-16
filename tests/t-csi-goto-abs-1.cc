#include "capture.h"

int main()
{
    initscr();
    linenum();
    printf("\x1b[10;1H");         /* goto abs 10,1 */
    printf("goto line 10\n");
    capture();
}