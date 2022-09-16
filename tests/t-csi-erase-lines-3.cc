#include "capture.h"

int main()
{
    initscr();
    for (size_t i = 0; i <  ws.ws_row *  ws.ws_col; i++) printf("L");
    printf("\x1b[12;31H");       /* goto abs 12,31 */
    printf("\x1b[K");            /* erase line */
    capture();
}