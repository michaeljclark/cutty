#include <cstdio>

static const char* ctrl_code[32] = {
    "NUL",
    "SOH", // ^A - Start Of Heading
    "STX", // ^B - Start Of Text
    "ETX", // ^C - End Of Text
    "EOT", // ^D - End Of Transmission
    "ENQ", // ^E - Enquiry
    "ACK", // ^F - Acknowledge
    "BEL", // ^G - Bell
    "BS",  // ^H - Backspace
    "HT",  // ^I - Horizontal Tab
    "LF",  // ^J - Line Feed
    "VT",  // ^K - Vertical Tab
    "FF",  // ^L - Form Feed
    "CR",  // ^M - Carriage Return
    "SO",  // ^N - Shift Out
    "SI",  // ^O - Shift In
    "DLE", // ^P - Data Link Escape
    "DC1", // ^Q - Device Control 1
    "DC2", // ^R - Device Control 2
    "DC3", // ^S - Device Control 3
    "DC4", // ^T - Device Control 4
    "NAK", // ^U - Negative Acknowledge
    "SYN", // ^V - Synchronize Idle
    "ETB", // ^W - End Of Transmission Block
    "CAN", // ^X - Cancel
    "EM",  // ^Y - End of Medium
    "SUB", // ^Z - Substitute
    "ESC", // ^[ - Escape
    "FS",  // ^\ - File Separator
    "GS",  // ^] - Group Separator
    "RS",  // ^^ - Record Separator
    "US",  // ^_ - Unit Separator
};

int main()
{
    printf("\n     |");
    for (int x = 0; x < 8; x++) printf("  %02d |", x);
    printf("\n     |");
    for (int x = 0; x < 8; x++) printf(" --- |");
    printf("\n");
    for (int y = 0; y < 16; y++) {
        printf("  %02d |", y);
        for (int x = 0; x < 8; x++) {
            int c = x * 16 + y;
            if (c < 32) printf(" %3s |", ctrl_code[c]);
            else if (c == 127) printf(" DEL |");
            else printf("  %c  |", c);
        }
        printf("\n");
    }
    printf("\n");
}