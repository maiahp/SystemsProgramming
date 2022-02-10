#include "queue.h"
#include <stdlib.h>

node* head = NULL; // ptr to head of queue
node* tail = NULL; // ptr to tail of queue

/* enqueue
 * puts data (the int connfd) into the back of the queue
 */
void enqueue(int data) {
	node *new_node = malloc(sizeof(node));
	new_node->data = data;
	new_node->next = NULL;
	if (head == NULL) { // this is first elem inserted into queue
            head = new_node;
	    tail = new_node;
	} else { // otherwise element becomes the tail
	    tail->next = new_node; // curr tail's next elem points to new node
	    tail = new_node; // new node becomes tail
	}
}

/*
 * dequeue
 * returns data (an int connfd) from the front of the queue
 */
int dequeue() {
	if (head != NULL) { // there is data in queue
		int d = head->data;
		node *old_head = head;
		node *new_head = head->next;
		head = new_head;
		if (head == NULL) { // no elems left in queue
			tail = NULL; // set tail to null
		}
		free(old_head);
		return d;
	} else { // head is null, meaning no more data in queue
		return -999; 
		// our data is the client file descriptor
		// which is given to us by fcn accept() 
		// which upon success, is a non-neg int value
		// and upon failure, returns -1
		// so -999 is a safe value to return if we have no items 
	}

}

