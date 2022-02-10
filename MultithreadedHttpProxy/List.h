/*
* Maiah Pardo, mapardo
* 2020 Fall CSE 101 PA1
* List.h
* Header file for List ADT
*/

#ifndef List_h
#define List_h
#define index cursorIndex
#define clear clearList

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>

// Struct for file_t
typedef struct fileData {
    char filename[20]; // filename can only be up to 19 characters
    uint8_t *contents; // contents can be of chosen cache size
    char date[500];
    size_t filesize;  // the total num of bytes in the file
} file_t;

typedef struct NodeObj{
    file_t* data;
    struct NodeObj* next;
    struct NodeObj* prev;
} NodeObj;

typedef NodeObj* Node;

typedef struct ListObj{
    Node front;     // references node at the front of list
    Node back;      // references back of list
    Node cursor;    // the node which is pointed to by cursor
    int index;      // the cursor index; starts at 0
    int length;     // number of elements; starts at 1
} ListObj;


// Public Exported Type ---------------------------------------------------
typedef struct ListObj* List;


// Constructors-Destructors ---------------------------------------------------

// newList()
// Creates and returns a new empty List.
List newList(void);

// freeList()
// Frees all heap memory associated with *pL, and sets
// *pL to NULL.
void freeList(List* pL);


// Access functions -----------------------------------------------------------

// addToContents()
void addToContents(List L, uint8_t* Buffer, size_t BufferSize, int idx);


// length()
// Returns the number of elements in L.
int length(List L);

// index()
// Returns index of cursor element if defined, -1 otherwise.
int indexFn(List L);

// front()
// Returns front element of L. Pre: length()>0
Node front(List L);

// back()
// Returns back element of L. Pre: length()>0
Node back(List L);

// get()
// Returns cursor element of L. Pre: length()>0, index()>=0
Node get(List L);

/* Added this */
// Find()
// Given a filename, returns the node containing the filename
Node Find(List L, char* filename);

/* End of Addition */


// Manipulation procedures ----------------------------------------------------

// moveNodetoFront()
// If L is non-empty, moves the Node which the cursor is set to,
// to the front of the list
// otherwise does nothing.
void moveNodetoFront(List L, int max_file_size); // deletes and re-adds a node (loops thru file data twice)

void moveNodetoFront2(List L); // uses pointers to hook the node to the front of list

void deleteContentsUpdateDate(List L, char *date, int max_file_size);

/* End Addition */

// clear()
// Resets L to its original empty state.
void clear(List L);

// moveFront()
// If L is non-empty, sets cursor under the front element,
// otherwise does nothing.
void moveFront(List L);

// movePrev()
// If cursor is defined and not at front, move cursor one
// step toward the front of L; if cursor is defined and at
// front, cursor becomes undefined; if cursor is undefined
// do nothing
void movePrev(List L);

// moveNext()
// If cursor is defined and not at back, move cursor one
// step toward the back of L; if cursor is defined and at
// back, cursor becomes undefined; if cursor is undefined
// do nothing
void moveNext(List L);

// prepend()
// Insert new element into L. If L is non-empty,
// insertion takes place before front element.
void prepend(List L, long max_file_size, char *filename, char *date, size_t filesize);

// append()
// Insert new element into L. If L is non-empty,
 // insertion takes place after back element.
void append(List L, long max_file_size, char *filename, char *date, size_t filesize);

// insertBefore
// Insert new element before cursor.
// Pre: length()>0, index()>=0
void insertBefore(List L, long max_file_size, char *filename, char *date, size_t filesize);

// insertAfter()
// Insert new element after cursor.
// Pre: length()>0, index()>=0
void insertAfter(List L, long max_file_size, char *filename, char *date, size_t filesize);

// deleteFront()
// Delete the front element. Pre: length()>0
void deleteFront(List L);

// deleteBack()
// Delete the back element. Pre: length()>0
void deleteBack(List L);

// delete()
// Delete cursor element, making cursor undefined.
// Pre: length()>0, index()>=0
void delete(List L);


// Other operations -----------------------------------------------------------

// printList()
// Prints to the file pointed to by out, a
// string representation of L consisting
// of a space separated sequence of integers,
// with front on left.
void printList(List L);

// copyList()
// Returns a new List representing the same integer
// sequence as L. The cursor in the new list is undefined,
// regardless of the state of the cursor in L. The state
// of L is unchanged.
//List copyList(List L);

// concatList()
// Returns a new List which is the concatenation of
// A and B. The cursor in the new List is undefined,
// regardless of the states of the cursors in A and B.
// The states of A and B are unchanged.
//List concatList(List A, List B);

#endif /* List_h */
