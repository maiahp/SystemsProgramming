#------------------------------------------------------------------------------
# Maiah Pardo, mapardo
# Asgn3 Makefile
# 
# make		makes httpproxy
# make clean	removes all binaries and executables
#------------------------------------------------------------------------------
CFLAGS = -std=c99 -g -Wall -Wextra -Wpedantic -Wshadow -pthread

all : httpproxy

httpproxy : httpproxy.o queue.o List.o
	gcc $(CFLAGS) -o httpproxy httpproxy.o queue.o List.o

httpproxy.o : httpproxy.c 
	gcc $(CFLAGS) -c httpproxy.c 

queue.o : queue.c
	gcc $(CFLAGS) -c queue.c

List.o : List.c
	gcc $(CFLAGS) -c List.c

clean :
	rm -f httproxy httpproxy.o queue.o List.o


