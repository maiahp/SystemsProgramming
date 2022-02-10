#ifndef QUEUE_H
#define QUEUE_H

typedef struct queue_node {
	struct queue_node* next;
	int data;
} node;


// functions: enqueue, dequeue
// enqueue an int, dequeue an int (the int is the client connfd)
void enqueue(int data);
int dequeue();



#endif

