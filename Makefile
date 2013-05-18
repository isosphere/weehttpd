NAME=weehttpd

all:
	gcc -o ${NAME} -l pcre -l config main.c
debug:
	gcc -ggdb -o ${NAME}-debug -l pcre -l config main.c
test:
	nc localhost 8080
