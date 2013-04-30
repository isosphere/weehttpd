NAME=main

all:
	gcc -o ${NAME} main.c
debug:
	gcc -ggdb -o ${NAME}-debug main.c
