#include "capture.h"

int main()
{
    initscr();
    linenum();
    printf("\x1b[10;1H");         /* goto abs 10,1 */
    for (size_t i = 0; i < 120; i++) printf("S");
    printf("\n");
    printf("\x1b[1;1H");         /* goto abs 1,1 */
    printf("goto line 1\n");
    capture();
}