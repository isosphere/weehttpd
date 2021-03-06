NAME=weehttpd

all:
	gcc -Werror -Wimplicit -o ${NAME} -l pcre -l config main.c
debug:
	gcc -ggdb -o ${NAME}-debug -l pcre -l config main.c
test:
	valgrind ./weehttpd-debug
