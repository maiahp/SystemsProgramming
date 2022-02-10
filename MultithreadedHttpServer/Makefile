#----------------------------------------------------------
# Makefile for CSE 130 Asgn 2
#
# make		makes httpserver
# make clean    removes all binaries and executables
# ---------------------------------------------------------
CFLAGS = -std=c99 -g -Wall -Wextra -Wpedantic -Wshadow -pthread

all : httpserver 

httpserver : httpserver.o queue.o
	gcc $(CFLAGS) -o httpserver httpserver.o queue.o

httpserver.o : httpserver.c 
	gcc $(CFLAGS) -c httpserver.c

queue.o : queue.c
	gcc $(CFLAGS) -c queue.c

clean :
	rm -f httpserver httpserver.o queue.o 



