#include <stdio.h>

int main()
{
	for (int i = 30; i < 37; i++) {
		printf("\x1b[7;47;%dm%03d\x1b[0m ", i, i);
	}
	for (int i = 30; i < 37; i++) {
		printf("\x1b[4;%dm%03d\x1b[0m ", i, i);
	}
	for (int i = 30; i < 37; i++) {
		printf("\x1b[2;47;%dm%03d\x1b[0m ", i, i);
	}
	for (int i = 30; i < 37; i++) {
		printf("\x1b[47;%dm%03d\x1b[0m ", i, i);
	}
	for (int i = 30; i < 37; i++) {
		printf("\x1b[1;47;%dm%03d\x1b[0m ", i, i);
	}

	for (int i = 90; i < 97; i++) {
		printf("\x1b[7;107;%dm%03d\x1b[0m ", i, i);
	}
	for (int i = 90; i < 97; i++) {
		printf("\x1b[4;%dm%03d\x1b[0m ", i, i);
	}
	for (int i = 90; i < 97; i++) {
		printf("\x1b[2;107;%dm%03d\x1b[0m ", i, i);
	}
	for (int i = 90; i < 97; i++) {
		printf("\x1b[107;%dm%03d\x1b[0m ", i, i);
	}
	for (int i = 90; i < 97; i++) {
		printf("\x1b[1;107;%dm%03d\x1b[0m ", i, i);
	}

	for (int i = 40; i < 47; i++) {
		printf("\x1b[7;37;%dm%03d\x1b[0m ", i, i);
	}
	for (int i = 40; i < 47; i++) {
		printf("\x1b[4;%dm%03d\x1b[0m ", i, i);
	}
	for (int i = 40; i < 47; i++) {
		printf("\x1b[2;37;%dm%03d\x1b[0m ", i, i);
	}
	for (int i = 40; i < 47; i++) {
		printf("\x1b[37;%dm%03d\x1b[0m ", i, i);
	}
	for (int i = 40; i < 47; i++) {
		printf("\x1b[1;37;%dm%03d\x1b[0m ", i, i);
	}

	for (int i = 100; i < 107; i++) {
		printf("\x1b[7;97;%dm%03d\x1b[0m ", i, i);
	}
	for (int i = 100; i < 107; i++) {
		printf("\x1b[4;%dm%03d\x1b[0m ", i, i);
	}
	for (int i = 100; i < 107; i++) {
		printf("\x1b[2;97;%dm%03d\x1b[0m ", i, i);
	}
	for (int i = 100; i < 107; i++) {
		printf("\x1b[97;%dm%03d\x1b[0m ", i, i);
	}
	for (int i = 100; i < 107; i++) {
		printf("\x1b[1;97;%dm%03d\x1b[0m ", i, i);
	}

	printf("\n");
}