#include "capture.h"

int main()
{
    initscr();
    linenum();
    printf("\x1b[12;1H");         /* goto abs 12,1 */
    printf("\x1b[1J");            /* erase lines to beginning */
    printf("erased to beginning\n");
    capture();
}