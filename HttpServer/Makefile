#----------------------------------------------------------
# Makefile for CSE 130 Asgn 1
#
# make		makes httpserver
# make clean    removes all binaries and executables
# ---------------------------------------------------------

all : httpserver

httpserver : httpserver.o
	gcc -g -Wall -Wextra -Wpedantic -Wshadow -o httpserver httpserver.o

httpserver.o : httpserver.c
	gcc -g -Wall -Wextra -Wpedantic -Wshadow -c httpserver.c

clean :
	rm -f httpserver httpserver.o



