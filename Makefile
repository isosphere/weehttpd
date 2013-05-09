NAME=weehttpd

all:
	gcc -o ${NAME} main.c
debug:
	gcc -ggdb -o ${NAME}-debug main.c
test:
	nc 127.0.0.1 80
	wbox http://localhost 10 wait 0
