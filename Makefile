NAME=weehttpd

all:
	gcc -o ${NAME} -l pcre main.c
debug:
	gcc -ggdb -o ${NAME}-debug -l pcre main.c
test:
	nc localhost 8080
