#include <stdio.h>

int main()
{
	printf("inv  fg-regular-color bg-regular-white ");
	for (int i = 30; i < 37; i++) {
		printf("\x1b[7;47;%dm%03d\x1b[0m ", i, i);
	}
	printf("\nund  fg-regular-color bg-regular-white ");
	for (int i = 30; i < 37; i++) {
		printf("\x1b[4;%dm%03d\x1b[0m ", i, i);
	}
	printf("\ndim  fg-regular-color bg-regular-white ");
	for (int i = 30; i < 37; i++) {
		printf("\x1b[2;47;%dm%03d\x1b[0m ", i, i);
	}
	printf("\nnorm fg-regular-color bg-regular-white ");
	for (int i = 30; i < 37; i++) {
		printf("\x1b[47;%dm%03d\x1b[0m ", i, i);
	}
	printf("\nbold fg-regular-color bg-regular-white ");
	for (int i = 30; i < 37; i++) {
		printf("\x1b[1;47;%dm%03d\x1b[0m ", i, i);
	}
	printf("\ninv  fg-bright-color  bg-bright-white  ");
	for (int i = 90; i < 97; i++) {
		printf("\x1b[7;107;%dm%03d\x1b[0m ", i, i);
	}
	printf("\nund  fg-bright-color  bg-bright-white  ");
	for (int i = 90; i < 97; i++) {
		printf("\x1b[4;%dm%03d\x1b[0m ", i, i);
	}
	printf("\ndim  fg-bright-color  bg-bright-white  ");
	for (int i = 90; i < 97; i++) {
		printf("\x1b[2;107;%dm%03d\x1b[0m ", i, i);
	}
	printf("\nnorm fg-bright-color  bg-bright-white  ");
	for (int i = 90; i < 97; i++) {
		printf("\x1b[107;%dm%03d\x1b[0m ", i, i);
	}
	printf("\nbold fg-bright-color  bg-bright-white  ");
	for (int i = 90; i < 97; i++) {
		printf("\x1b[1;107;%dm%03d\x1b[0m ", i, i);
	}
	printf("\ninv  fg-regular-white bg-regular-color ");
	for (int i = 40; i < 47; i++) {
		printf("\x1b[7;37;%dm%03d\x1b[0m ", i, i);
	}
	printf("\nund  fg-regular-white bg-regular-color ");
	for (int i = 40; i < 47; i++) {
		printf("\x1b[4;%dm%03d\x1b[0m ", i, i);
	}
	printf("\ndim  fg-regular-white bg-regular-color ");
	for (int i = 40; i < 47; i++) {
		printf("\x1b[2;37;%dm%03d\x1b[0m ", i, i);
	}
	printf("\nnorm fg-regular-white bg-regular-color ");
	for (int i = 40; i < 47; i++) {
		printf("\x1b[37;%dm%03d\x1b[0m ", i, i);
	}
	printf("\nbold fg-regular-white bg-regular-color ");
	for (int i = 40; i < 47; i++) {
		printf("\x1b[1;37;%dm%03d\x1b[0m ", i, i);
	}
	printf("\ninv  fg-bright-white  bg-bright-color  ");
	for (int i = 100; i < 107; i++) {
		printf("\x1b[7;97;%dm%03d\x1b[0m ", i, i);
	}
	printf("\nund  fg-bright-white  bg-bright-color  ");
	for (int i = 100; i < 107; i++) {
		printf("\x1b[4;%dm%03d\x1b[0m ", i, i);
	}
	printf("\ndim  fg-bright-white  bg-bright-color  ");
	for (int i = 100; i < 107; i++) {
		printf("\x1b[2;97;%dm%03d\x1b[0m ", i, i);
	}
	printf("\nnorm fg-bright-white  bg-bright-color  ");
	for (int i = 100; i < 107; i++) {
		printf("\x1b[97;%dm%03d\x1b[0m ", i, i);
	}
	printf("\nbold fg-bright-white  bg-bright-color  ");
	for (int i = 100; i < 107; i++) {
		printf("\x1b[1;97;%dm%03d\x1b[0m ", i, i);
	}
	printf("\n");
}