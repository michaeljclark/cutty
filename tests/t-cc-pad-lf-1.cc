#include "capture.h"

int main()
{
    initscr();
    linenum();
    printf("\x1b[1;1H");          /* goto abs 1,1 */
    for (size_t i=0; i < ws.ws_col; i++) printf("L");
    printf("\nLF");
    capture();
}