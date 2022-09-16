#include "capture.h"

int main()
{
    initscr();
    for (size_t i = 0; i <  ws.ws_row *  ws.ws_col; i++) printf("L");
    printf("\x1b[12;1H");        /* goto abs 12,1 */
    printf("\x1b[K");            /* erase line */
    printf("\x1b[13;1H");        /* goto abs 13,1 */
    printf("\x1b[K");            /* erase line */
    capture();
}