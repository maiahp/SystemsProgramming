#----------------------------------------------------------
# Makefile for CSE 130 Asgn 0
#
# make		makes shoulders
# make clean    removes all binaries
# ---------------------------------------------------------

all : shoulders

shoulders : shoulders.o
	gcc -o shoulders shoulders.o

shoulders.o : shoulders.c
	gcc -g -Wall -c shoulders.c

clean : 
	rm shoulders shoulders.o
