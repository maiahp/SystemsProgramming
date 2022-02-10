#define _XOPEN_SOURCE 500
#include <err.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/file.h> // for flock
#include <stdio.h>
#include <ctype.h>
#include <stdbool.h>
#include <pthread.h>
#include "queue.h"


// must be declared globally
// locks queue operations so that enqueue/dequeue don't happen together
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
// condition for dispatch loop
pthread_cond_t dispatch_condition = PTHREAD_COND_INITIALIZER;
// num of requests received from client
int NUM_REQUESTS = 0;
// num of requests which ended in error
int NUM_REQUEST_ERRORS = 0;
// locks the logfile offset var so that only one thread can access it at one time
static pthread_mutex_t logfile_offset_mutex = PTHREAD_MUTEX_INITIALIZER;

// protect the call to open, close, read, write using a mutex
// the flock() lock can be grabbed by multiple threads if they
// reach for it at the same time, so we need a mutex to protect it
static pthread_mutex_t flock_lock = PTHREAD_MUTEX_INITIALIZER;

// log file offset
off_t LOG_OFFSET = 0;
//int LOG_FD = -1;
char LOG_FILE_NAME[500];
int isLogging = 0;
int NUMTHREADS;




/**
 * workerThread struct
 * stores data for a thread
 */
typedef struct workerThreadAttributes {
  pthread_t thread_id;		        // need this for pthread_create()
  int id;
  int log_fd;                           // log file descriptor
  size_t logfile_offset;                // logfile offset
  int status;                           // status code for the message
  char httprequest_msg[4096];           // the message to put into log file
  char requestType[50];                 // holds PUT, GET, or HEAD
  char filename[50];                    // filename for operations: PUT, GET, HEAD
  long contentlen;                      // body of the request (only for PUT, GET)
  int portnum;                          // holds port num
  char first_thou_file_bytes[2001];     // holds hex data of first thousand bytes we send/recv
  uint8_t putBuffer[8192];             // holds extra file contents from first recv() if we have them
  char host[50];                        // host name
  char req_line_from_client[1001];      // need this when we have a FAIL case
  int logoffset;
  int* free;
  int putBufferSize;
  int num_request_errors;
  int num_requests;
} workerThread;



// request  responded to successfully (if no other msg is more appropriate)
const char OK[] = "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\nOK\n";
int OKSIZE = 3;
// file was created (not overwritten) and the request responded to successfully
const char CREATED[] = "HTTP/1.1 201 Created\r\nContent-Length: 8\r\n\r\nCreated\n"; 
// request cannot be parsed successfully or has problems that result in failure to respond (if no other message is appropriate)
const char BADREQUEST[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 12\r\n\r\nBad Request\n";
// request is for a valid resource name but server doesn't have permission to access it
const char FORBIDDEN[] = "HTTP/1.1 403 Forbidden\r\nContent-Length: 10\r\n\r\nForbidden\n";
// request is for valid resource name but server can't find it (file doesn't exist)
const char FILENOTFOUND[] = "HTTP/1.1 404 File Not Found\r\nContent-Length: 15\r\n\r\nFile Not Found\n";
// request is valid but server can't comply because of internal problems (error when allocating memory for a data structure that would be necessary to process the request (and no other code is more appropriate)
const char INTERNALSERVERERROR[] = "HTTP/1.1 500 Interal Server Error\r\nContent-Length: 22\r\n\r\nInternal Server Error\n";
// if request is properly formatted but uses a request type that the server has not implemented (not GET, PUT, or HEAD)
const char NOTIMPLEMENTED[] = "HTTP/1.1 501 Not Implemented\r\nContent-Length: 16\r\n\r\nNot Implemented\n";
// status codes
int OK_STATUS = 200;
int CREATED_STATUS = 201;
int BAD_STATUS = 400;
int FORBIDDEN_STATUS = 403;
int FILENOTFOUND_STATUS = 404;
int INTERNALSERVERERROR_STATUS = 500;
int NOTIMPLEMENTED_STATUS = 501;


/**
   Converts a string to an 16 bits unsigned integer.
   Returns 0 if the string is malformed or out of the range.
 */
uint16_t strtouint16(char number[]) {
  char *last;
  long num = strtol(number, &last, 10);
  if (num <= 0 || num > UINT16_MAX || *last != '\0') {
    return 0;
  }
  return num;
}


/**
 * Checks that a string may be successfully converted to an integer
 * Returns 1 upon success, 0 upon failure
 */
int canConvertToInt(char str[]) {
   char *ptr = NULL;
   strtol(str, &ptr, 0);
   if (ptr == str) {
      // all of input str is made up of chars
      // then could not convert to num
      return 0;
   } else if (*ptr == '\0') { 
      // entire string converted to num
      return 1;
   } else {          
      // part of str was unconverted
      // invalid num
      return 0;
   }
}


/**
   Creates a socket for listening for connections.
   Closes the program and prints an error message on error.
 */
int create_listen_socket(uint16_t port) {
  struct sockaddr_in addr;
  int listenfd = socket(AF_INET, SOCK_STREAM, 0L);
  if (listenfd < 0) {
    err(EXIT_FAILURE, "socket error");
  }

  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htons(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(listenfd, (struct sockaddr*)&addr, sizeof addr) < 0L) {
    err(EXIT_FAILURE, "bind error");
  }

  if (listen(listenfd, 500) < 0) {
    err(EXIT_FAILURE, "listen error");
  }

  return listenfd;
}

// GET sends file contents
// GET FAILS for large text files
void handleGet(int connfd, char* file, workerThread* wkr) {
    memset(wkr->first_thou_file_bytes,'\0',2001);
    ssize_t size_1000 = 0; // size of extra file contents from first recv()
    
    //fprintf(stderr, "check if filename is healthcheck! wkr->filename:%s, file:%s\n", wkr->filename, file);
    if ((strcmp(file, "healthcheck") == 0) || (strcmp(wkr->filename, "healthcheck") == 0)) {
	   return;
    } 
    //fprintf(stderr, "SHOULD NOT SEE THIS if filename is healthcheck!\n");

    int file_exists;
    int file_readpermission;
    char msg[150];
    memset(msg,'\0',150);
    //msg[150-1] = '\0';
    // check if file exists
    file_exists = access(file, F_OK); // 0 if access permitted, -1 otherwise
    if (file_exists == -1) {
        // send a file not found message
        wkr->status = FILENOTFOUND_STATUS;
        send(connfd, FILENOTFOUND, strlen(FILENOTFOUND), 0);
        close(connfd);
        return;
    }
    // check read permission
    file_readpermission = access(file, R_OK); // 0 if access permitted, -1 otherwise
    if (file_readpermission == -1) {
        // send forbidden message
	wkr->status = FORBIDDEN_STATUS;
        send(connfd, FORBIDDEN, strlen(FORBIDDEN), 0);
        close(connfd);
        return;
    }

    int fd = open(file, O_RDONLY); // if error, -1 is returned and errno is set with why failure

    // rdonly, no flag

    if (fd == -1) {
        int error_num = errno;
        // check if file does not exist
        if (error_num == ENOENT) {
            wkr->status = FILENOTFOUND_STATUS;
            send(connfd, FILENOTFOUND, strlen(FILENOTFOUND), 0);
            close(connfd);
	    close(fd);
            return;
        // check if file cannot be accessed (forbidden)
        } else if (error_num == EACCES) {
	    wkr->status = FORBIDDEN_STATUS;
            send(connfd, FORBIDDEN, strlen(FORBIDDEN), 0);
            close(connfd);
	    close(fd);
            return;
        // any other error should be an internal error      
        } else {
            wkr->status = INTERNALSERVERERROR_STATUS;
            send(connfd, INTERNALSERVERERROR, strlen(INTERNALSERVERERROR), 0);             
            close(connfd);
	    close(fd);
            return;
        }
    }

    // Using flock:
    pthread_mutex_lock(&flock_lock);
    //fprintf(stderr, "In a get, locking flock_lock\n");
    int r = flock(fd, LOCK_SH);
    pthread_mutex_unlock(&flock_lock);

    if (r == -1) {
       //fprintf(stderr, "ERROR: FLOCK() FAILED IN GET\n");
       wkr->status = INTERNALSERVERERROR_STATUS;
       close(connfd);
       close(fd);
       return;
    }
    
    // find the size of the file
    struct stat st;
    stat(file, &st);
    off_t size = st.st_size; // file size
    wkr->contentlen = size; 
    ssize_t readbytes = 0;

    // reached here, the file exists and can be opened
    int bufferSize = 1000;
    uint8_t Buffer[bufferSize + 1];
    
    // send the OK message
    // then after, send the read bytes from the file
    sprintf(msg, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", size);
    send(connfd, msg, strlen(msg), 0);
    wkr->status = OK_STATUS;

    // send bytes from file to client
    while(1) {
        readbytes = read(fd, Buffer, bufferSize);
	if (readbytes <= 0) { // no more bytes, reached eof
            break;
        }
             // if readbytes >= 1000-size of xtra file contents from first recv()
             // (1000 - size of xtra file contents) is the remaining amount of bytes we need to put into first_thou_file_bytes
             // so if readbytes has more bytes than the remaining amount of bytes we need to put into first_thou_file_bytes
        else if(readbytes >= 1000-size_1000){
            // if we do not have 1000 bytes yet in first_tho_bytes, continue
            if(size_1000 < 1000){
               // add the remaining bytes that we need into first_thou_file_bytes

	       // convert to hex
                int j = size_1000;
                for (int i=0; i<(1000-size_1000); i++) {
                      // for each character in r, conver to hex
                      sprintf((char*)&wkr->first_thou_file_bytes + j, "%02hhx", Buffer[i]);
                      j+=2;
                }
               //fprintf(stderr, "In Get #1, first_thouO_file_bytes:%s\n", wkr->first_thou_file_bytes);
               size_1000 = 1000;
            }
        }
        // if readbytes < the remaining amount of bytes we need to put into first_thou_file_bytes
        // then we add all the rest of the bytes in readbytes, as long as readbytes is greater than 0
        else if((size_1000 < 1000) && (readbytes > 0)){
            // convert to hex
            int j = size_1000;
            for (int i=0; i<(readbytes); i++) {
                 // for each character in r, conver to hex
                 sprintf((char*)&wkr->first_thou_file_bytes + j, "%02hhx", Buffer[i]);
                 j+=2;
            }
            size_1000 += readbytes;
        } 
       	
	// we have read in bytes from file, send to client
	send(connfd, Buffer, readbytes, 0); 
   }


   close(connfd);
   // only close connfd when error 
   // if you try to recv again 

   //fprintf(stderr, "About to finish GET, wkr->first_thou_file_bytes: %s\n", wkr->first_thou_file_bytes);
   fsync(fd);
   close(fd); //close file descriptor, also releases flock() lock
   //fprintf(stderr, "Finished Get, exiting with status code: wkr->status: %d\n", wkr->status);
   return;
}

// put receives file contents
void handlePut(int connfd, char* file, long contentlen, int isMoreContents, workerThread* wkr) {
    memset(wkr->first_thou_file_bytes,'\0', 2001);
    //ssize_t size_1000 = strlen(wkr->first_thou_file_bytes);
    ssize_t size_1000 = 0;
    wkr->contentlen = contentlen; // must store for logging
    
    if (strcmp(wkr->filename, "healthcheck") == 0) {
        return;
    }

    //fprintf(stderr, "entered PUT\n");
    
    int wasCreated = 1;
    int file_exists;
    int file_writepermission;
    char msg[150];
    memset(msg,'\0',150);
    //msg[150-1] = '\0';
    // check if file exists
    file_exists = access(file, F_OK); // 0 if access permitted, -1 otherwise
    if (file_exists == 0) { // if file does exist
        // check if file has write permission
        file_writepermission = access(file, W_OK);
        if (file_writepermission == -1) {
            // send forbidden message
	    wkr->status = FORBIDDEN_STATUS;
            send(connfd, FORBIDDEN, strlen(FORBIDDEN), 0);
            close(connfd);
            return;
        }
	
	wasCreated = 0; // file was not created, already existed
    }
    
    int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0666); // 00700 

    if (fd == -1) {
        //fprintf(stderr, "in put: fd is -1\n");
        int error_num = errno;
        // check if file does not exist
        if (error_num == ENOENT) {
	    wkr->status = FILENOTFOUND_STATUS;
            send(connfd, FILENOTFOUND, strlen(FILENOTFOUND), 0);
            close(connfd);
	    close(fd);
            return;
        // check if file cannot be accessed (forbidden)
        } else if (error_num == EACCES) {
	    wkr->status = FORBIDDEN_STATUS;
            send(connfd, FORBIDDEN, strlen(FORBIDDEN), 0);
            close(connfd);
	    close(fd);
            return;
        // any other error should be an internal error      
        } else {
            wkr->status = INTERNALSERVERERROR_STATUS;
            send(connfd, INTERNALSERVERERROR, strlen(INTERNALSERVERERROR), 0);
            close(connfd);
	    close(fd);
            return;
        }
    }

    pthread_mutex_lock(&flock_lock);
    int r = flock(fd, LOCK_EX);
    pthread_mutex_unlock(&flock_lock);
    if (r == -1) {
       //fprintf(stderr, "ERROR: FLOCK() FAILED IN PUT\n");
       wkr->status = INTERNALSERVERERROR_STATUS;
       close(connfd);
       close(fd);
       return;
    }
   
    // file was opened successfully for writing
    int BufferSize = 1000;
    uint8_t Buffer[BufferSize + 1]; // create buffer of size contentlen to hold bytes from client
    ssize_t num_bytes_received = 0; // bytes received from client 
    ssize_t recv_data = 0; 

    // only grab rest of bytes we did not receive if we initially recv'd some
    if (isMoreContents == 1) {
	//fprintf(stderr, "FOUND EXTRA FILE CONTENTS FOR FILE: %s. They are:\n%s\nThey are of size: %d\n", file, extra_file_contents, strlen(extra_file_contents));
        // grab len of extra contents
        int len = wkr->putBufferSize;
	if (len > 0) { // if we have extra contents
	   // we must write it to the file
	   write(fd, wkr->putBuffer, len);
	}

	//fprintf(stderr, "just wrote extra bytes to the file. They are of size:%ld, wkr->putBufferSize is:%d\n", strlen((char*)wkr->putBuffer), wkr->putBufferSize);

        // add this to num bytes recvd from file
        num_bytes_received += len;
        if(len < 1000) {
           // if len is less than 1000, add all bytes to first thou file bytes
           // hex conversion
	   int j = 0;
           for (int i=0; i<len; i++) {
                // for each character in r, conver to hex
                sprintf((char*)&wkr->first_thou_file_bytes + j, "%02hhx", wkr->putBuffer[i]);
                j+=2;
           }
           size_1000 += len;
        } else{
           // if len is not less than 1000, add just 1000 bytes to first thou file bytes
           // hex conversion
	   int j = 0;
           for (int i=0; i<1000; i++) {
                // for each character in r, conver to hex
                sprintf((char*)&wkr->first_thou_file_bytes + j, "%02hhx", wkr->putBuffer[i]);
                j+=2;
           }
           size_1000 += 1000;
        }
    }
    
    //fprintf(stderr, "in put: now looping through while, and writing data\n");
    //fprintf(stderr, "num bytes recvd:%ld, contentlen:%ld\n", num_bytes_received, contentlen);

    while(num_bytes_received < contentlen) {
        // receive the bytes from the client
	// only receive the exact num of bytes the client is sending
	if ((contentlen-num_bytes_received) >= BufferSize) { // total bytes left to recv > buff size	    
            recv_data = recv(connfd, Buffer, BufferSize, 0);
            // if data recvd is bigger than the remaining amount of bytes we need in first thou file bytes
            
	    // storing first thou file bytes as hex
	    if(recv_data >= 1000-size_1000){
               // if size of first thou file bytes < 1000, add to it the correct num of bytes
                if(size_1000 < 1000){
                    // convert to hex for what is missing IF we had extra file data
		    int j = size_1000;
                    for (int i=0; i<(1000-size_1000); i++) {
                          // for each character in r, conver to hex
                          sprintf((char*)&wkr->first_thou_file_bytes + j, "%02hhx", Buffer[i]);
                          j+=2;
                     }
                   size_1000 = 1000;
                 }
            // if readbytes is less than the remaining amount of bytes and
            // if size of extra bytes is less than 1000 and recv data is bigger than 0
            // add all the rest of recv data
	    } else if((size_1000 < 1000) && (recv_data > 0)){
                // convert to hex
		int j = size_1000;
                for (int i=0; i<recv_data; i++) {
                      // for each character in r, conver to hex
                      sprintf((char*)&wkr->first_thou_file_bytes + j, "%02hhx", Buffer[i]);
                      j+=2;
                 }
                size_1000 += recv_data;
            }
	    // end storing first thou file bytes as hex
             
	     num_bytes_received += recv_data;
	     write(fd, Buffer, recv_data);
	   
	} else if ((contentlen-num_bytes_received) < BufferSize) { // we need to receive a smaller amount of bytes
	         ssize_t num_bytes_left = contentlen-num_bytes_received;
	         recv_data = recv(connfd, Buffer, num_bytes_left, 0);
             
             // convert first thou file bytes to hex
	     if(recv_data >= 1000-size_1000){
                if(size_1000 < 1000){
                     // convert to hex
		     int j = size_1000;
                     for (int i=0; i<(1000-size_1000); i++) {
                           // for each character in r, conver to hex
                           sprintf((char*)&wkr->first_thou_file_bytes + j, "%02hhx", Buffer[i]);
                           j+=2;
                     }
                   size_1000 = 1000;
                }
             }
             else if(size_1000 < 1000 && recv_data > 0){
                // convert to hex
		int j = size_1000;
                for (int i=0; i<(recv_data); i++) {
                      // for each character in r, conver to hex
                      sprintf((char*)&wkr->first_thou_file_bytes + j, "%02hhx", Buffer[i]);
                      j+=2;
                }
                size_1000 += recv_data;
             }
	     // end converting first thou bytes to hex

	     // don't write non-received data:
	     if (recv_data < 0) {
		     wkr->status = INTERNALSERVERERROR_STATUS;
		     send(connfd, INTERNALSERVERERROR, strlen(INTERNALSERVERERROR), 0);
		     close(connfd);
		     close(fd);
		     return;
	     }
	     num_bytes_received += recv_data;
	     write(fd, Buffer, recv_data);
       }
    }

    //fprintf(stderr, "About to finish PUT, first_thou_file_bytes:%s\n", wkr->first_thou_file_bytes);
    // try flushing file
    fsync(fd);
    close(fd); // close file descriptor, also releases flock() lock
    
    // send the created message if it was created
    if (wasCreated == 1) {
	wkr->status = CREATED_STATUS;
        send(connfd, CREATED, strlen(CREATED), 0);
        close(connfd);
    } else { // file was not created, send OK message
        long int val = 3;
        wkr->status = OK_STATUS;
	sprintf(msg, OK, val);
	send(connfd, msg, strlen(msg), 0);
	close(connfd);
    }
    //fprintf(stderr, "Finished Put\n");
    //fprintf(stderr, "2: About to finish PUT, first_thou_file_bytes:%s\n", wkr->first_thou_file_bytes);
    //fprintf(stderr, "Finished Put, exiting with status code: wkr->status: %d\n", wkr->status);
    return;
}


void handleHead(int connfd, char* file, workerThread* wkr) {
    if (strcmp(wkr->filename, "healthcheck") == 0) {
        return;
    }
    
    int file_exists;
    int file_readpermission;
    char msg[150];
    msg[150-1] = '\0';

    // check if file exists
    file_exists = access(file, F_OK); // 0 if access permitted, -1 otherwise
    if (file_exists == -1) {
        // send a file not found message
        wkr->status = FILENOTFOUND_STATUS;
        send(connfd, FILENOTFOUND, strlen(FILENOTFOUND), 0);
        close(connfd);
	return;
    }
    // check read permission
    file_readpermission = access(file, R_OK); // 0 if access permitted, -1 otherwise
    if (file_readpermission == -1) { 
        // send forbidden message
        wkr->status = FORBIDDEN_STATUS;
	send(connfd, FORBIDDEN, strlen(FORBIDDEN), 0);
        close(connfd);
        return;	
    }

    int fd = open(file, O_RDONLY); // if error, -1 is returned and errno is set with why failure
    
    if (fd == -1) {
	    int error_num = errno;
	    // check if file does not exist
            if (error_num == ENOENT) {
		wkr->status = FILENOTFOUND_STATUS;
                send(connfd, FILENOTFOUND, strlen(FILENOTFOUND), 0);
                close(connfd);
		close(fd);
                return;
            // check if file cannot be accessed (forbidden)
	    } else if (error_num == EACCES) {
	        wkr->status = FILENOTFOUND_STATUS;
                send(connfd, FORBIDDEN, strlen(FORBIDDEN), 0);
                close(connfd);
		close(fd);
                return;
	    // any other error should be an internal error	
	    } else { 
		    wkr->status = INTERNALSERVERERROR_STATUS;
		    send(connfd, INTERNALSERVERERROR, strlen(INTERNALSERVERERROR), 0);
		    close(connfd);
		    close(fd);
		    return;
	    }
    }

    pthread_mutex_lock(&flock_lock);
    int r = flock(fd, LOCK_SH);
    pthread_mutex_unlock(&flock_lock);
    if (r == -1) { 
	    //fprintf(stderr, "ERROR LOCKING FILE WITH FLOCK IN HEAD\n");
	    wkr->status = INTERNALSERVERERROR_STATUS;
            send(connfd, INTERNALSERVERERROR, strlen(INTERNALSERVERERROR), 0);
	    close(connfd);
	    close(fd);
    }
    

    struct stat st;
    stat(file, &st);
    long int size = st.st_size;
    wkr->contentlen = size;
    // try flushing file
    fsync(fd);
    close(fd); // releases flock() lock
    wkr->status = OK_STATUS;
    sprintf(msg, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", size);
    send(connfd, msg, strlen(msg), 0);
    close(connfd);
    //fprintf(stderr, "Finished Head\n");
    //fprintf(stderr, "Finished Head, exiting with status code: wkr->status: %d\n", wkr->status);
    return;
}


void handle_connection(int connfd, workerThread* wkr) {
  int bufferSize = 8191; // changed from 4096
  char Buffer[bufferSize + 1];
  memset(Buffer, '\0', bufferSize+1); 
  // read in bytes which is the request msg
  ssize_t readbytes =  recv(connfd, Buffer, bufferSize, 0);
  if (readbytes < 0) {
     //fprintf(stderr, "handle conn, recv bytes error! wkr->status: %d\n", wkr->status);
     wkr->status = INTERNALSERVERERROR_STATUS;
     send(connfd, INTERNALSERVERERROR, strlen(INTERNALSERVERERROR), 0);
     close(connfd);
     // if we have a bad request, also must put it in httprequest_msg
     // but don't do this here, since we had error getting request data
     // don't need to update status, status = 0
     return; // if we dont have readbytes, can't process request
  }

  //fprintf(stderr, "\n\n\nAll bytes from buffer:\n%s\n\n\n", Buffer);

  int isMoreFileBytes = 0;
  int l = 0;
  int end = 0;
  while(l < readbytes){
       if(Buffer[l] == '\r' && Buffer[l+1] == '\n' && Buffer[l+2] == '\r' && Buffer[l+3] == '\n'){
       //printf("%d\n", l+4);
           end = l+4;
           break; // we found our index
       }
       //if((l+3) >= readbytes){
           //printf("oh oh\n");
       //    break;
       //}
       l++;
   } 
  int j = 0;

  memset(wkr->putBuffer, '\0', bufferSize+1);
  if (end < readbytes) { // if the index after \r\n\r\n is smaller than readbytes then we have more file data
      
     // for debug!!!
     //fprintf(stderr, "We have found extra file contents.\n");
     //fprintf(stderr, "The entire read bytes is:%s\n", Buffer);
     //fprintf(stderr, "The index where the extra file contents starts is:%d\n", end);
     //fprintf(stderr, "Buffer[%d] = %c\n", end, Buffer[end]);

     for(int k = end; k < readbytes; k++) {
           wkr->putBuffer[j] = Buffer[k];
	   Buffer[k] = '\0'; // remove extra file byte from Buffer so that we can parse headers correctly
           j++;
     }
    // fprintf(stderr, "We stored remaining bytes into putBuffer, they are: %s\n", wkr->putBuffer);
  
     if (j > 0) {
        // then we have extra file content
        //fprintf(stderr, "FOUND FILE CONTENTS IN FIRST RECV()\n");
        isMoreFileBytes = 1;
        //fprintf(stderr, "We stored remaining bytes into putBuffer, they are: %s\n", wkr->putBuffer);
     }
  }

  wkr->putBufferSize = j;
  //if (wkr->putBufferSize > 0) {
     //fprintf(stderr, "num of recv'd file bytes is wkr->putBufferSize:%d\n", wkr->putBufferSize);
     //fprintf(stderr, "num of recv'd file bytes could be: (readbytes-1)-(readbytes-end):%ld\n", (readbytes-1)-(readbytes-end));
  //}
  
  
  //fprintf(stderr, "Doing string tokenizing:\n");
  char *splitmsg = strtok(Buffer, "\r\n"); // split is the message split by \r\n, the clrf
  //fprintf(stderr, "\n\nSTART:\n\nsplitmsg:   %s\n", splitmsg);
  char requestType[50]; // max request type length is 5 chars (HEAD is 5 letters)
  memset(requestType, '\0', 50);
  char filename_in[100]; // filename taken in from request
  memset(filename_in, '\0', 100);
  char filename[20]; // filenames can only be up to 19 chars long
  memset(filename, '\0', 20);
  char httpversion[50]; // must always be equal to HTTP/1.1
  memset(httpversion, '\0', 50);
  sscanf(splitmsg, "%s %s %s", requestType, filename_in, httpversion); 
  
  //fprintf(stderr, "saving req_line_from_client\n");
  memset(wkr->req_line_from_client, '\0', 1001);
  if (strlen(splitmsg) > 1000) {
      strncat(wkr->req_line_from_client, splitmsg, 1000);
      //fprintf(stderr, "Saved req line from client (for FAIL):%s\n", wkr->req_line_from_client);
  } else {
      sprintf(wkr->req_line_from_client, "%s", splitmsg);
      //fprintf(stderr, "Saved req line from client (for FAIL):%s\n", wkr->req_line_from_client);
  }

//TEST
/*
  if (wkr->req_line_from_client == NULL) {
      fprintf("REQ LINE IS NULL. It is: %s\n exiting!", wkr->req_line_from_client);
      exit(0);
  }
  // END TEST
*/

  // must collect headers here because we need Content-Length value for all error messages 
  // Now loop through headers (header: body) 
  // and search for Content-Length
  char* token = strtok(NULL, "\r\n");
  long contentlen = 0;
  char contentlenstr[500];
  memset(contentlenstr, '\0', 500);
  long token2;
  long token1;
  long tokenHost; // make sure Host has no white space
  char hostname[50];
  memset(hostname, '\0', 50);
  char header[50];
  memset(header, '\0', 50);
  char body[50];
  memset(body, '\0', 50);
  int isNumber = 0;
  char *ptr = NULL;
  long num = 0L;
  char buff[20];
  
  // loop through headers and collect (header: body)
  // it must also be in this same format
  while(token != NULL) { // while token is not null
      //fprintf(stderr, "The TOKEN is:  %s\n", token);
      // check for content-length
      token1 = sscanf(token, "Content-Length:%s", contentlenstr);
      // if content-length was returned and we are in a PUT, check that it is a valid number
      if (strcmp(requestType, "PUT") == 0) {
           if (token1 == 1) { // if token1 saved 1 thing into a variable
               // check it is a valid number
               // strtol will set ptr to the first part of string that was unable to convert
               // to a number, or \0 if entire string converted to a num
               num = strtol (contentlenstr, &ptr, 0);
               if (ptr == contentlenstr) {
                   // if entire contentlenstr is made up of chars
                   // then could not convert to num
                   isNumber = 0;
               } else if (*ptr == '\0') { // entire string converted to num
                   isNumber = 1;
               } else {          // part of the string was unconverted, so we do not have a number
                   // consider as a file name
                   isNumber = 0;
               }
	       if (isNumber) {
                    contentlen = num;
		    wkr->contentlen = contentlen;
                    //fprintf(stderr, "In a PUT, Content-Length is: %ld\n", contentlen);
	       } else {
	           // not a number, send bad request
		   //fprintf(stderr, "handle_conn: reached bad-req: content len not a num\n");
                   wkr->status = BAD_STATUS;
		   send(connfd, BADREQUEST, strlen(BADREQUEST), 0);
                   close(connfd);
                   //return;
	       }
           }
      }
      
      memset(buff, '\0', 20); 
      tokenHost = sscanf(token, "Host: %s %s", hostname, buff);
      if(tokenHost == 2){ // if sscanf saved 2 items, then header is bad header
         //fprintf(stderr, "handle conn, token host failure! wkr->status: %d\n", wkr->status);
         wkr->status = BAD_STATUS;
	 send(connfd, BADREQUEST, strlen(BADREQUEST), 0);
	 close(connfd);
	 //return;
      }

      // if there is no hostname
      //if (strlen(hostname) == 0) {
         //fprintf(stderr, "handle conn again, token host failure! wkr->status: %d\n", wkr->status);
         //wkr->status = BAD_STATUS;
         //send(connfd, BADREQUEST, strlen(BADREQUEST), 0);
         //close(connfd);
      //}

      sprintf((char*)wkr->host, "%s", hostname);
      //fprintf(stderr, "Saved wkr->host:%s\n", wkr->host);
      // sscanf returns the number of fields that were saved into variables
      // fprintf(stderr, "token1 is: %ld\n", token1);
      // if no content len and no host tokens:
      if (token1 != 1 && tokenHost != 1) {
	      // then we have not grabbed content-len or Host  
	      // and have reached a different header
	      // must check that all other headers are valid header:body format
	      token2 = sscanf(token, "%s %s", header, body);
              if (token2 == 2) { // correct header:body format, check for ':'
		     //fprintf(stderr, "header: %s body: %s\n", header, body);
		     if (header[strlen(header)-1] != ':') { // if invalid request
			     //fprintf(stderr, "handle_conn:reached bad-req: Invalid header; header: %s, body: %s\n", header, body);
			     wkr->status = BAD_STATUS;
			     send(connfd, BADREQUEST, strlen(BADREQUEST), 0);
			     close(connfd);
			     //return;
		     } else {
			     // do nothing, valid header: body
	             }
	      
              } else { // token1 != 1 (not contentlen) and token2 != 2 (invalid header: body pair)
		      // we probably have extra file bytes in the first recv() if reached here, which we caught before
	      }
        } 
      // update token to move to next one
      token = strtok(NULL, "\r\n");
    }
  
  // check httpversion is correct 
  if(strcmp(httpversion, "HTTP/1.1") != 0) {
	  //sprintf(msg, BADREQUEST, 0L); // contentlen = 0 here
	  //fprintf(stderr, "handle_conn: Entered badreq for http/1.1 wrong version\n");
	  wkr->status = BAD_STATUS;
	  send(connfd, BADREQUEST, strlen(BADREQUEST), 0);
          close(connfd);
          //return;
  }

  // save filename = filename_in+1 to filename_in (exclude the first '/')
  memmove(filename, filename_in+1, strlen(filename_in));

  // check filename is ok
  int len = (int)strlen(filename);

  //fprintf(stderr, "checking if len > 19\n");
  if (len > 19) { // if length of incoming file name is > 19 chars
	//fprintf(stderr, "handle_conn:Entered badreq len>19\n");
	sprintf((char*)&wkr->filename, "%s", filename);
	wkr->status = BAD_STATUS;
	send(connfd, BADREQUEST, strlen(BADREQUEST), 0); 
        close(connfd); // close connection
  }	

  sprintf((char*)&wkr->filename, "%s", filename);
  //fprintf(stderr, "saved wkr->filename:%s\n", wkr->filename);

  // filename must only contain alphanumeric, . and _ only (exclude the first '/')
  len = (int)strlen(filename);
  for(int k=1; k < len; k++) {
          if (!(isalpha(filename[k]) || isdigit(filename[k]) || filename[k] == '_'|| filename[k] == '.')) {
		//fprintf(stderr, "handle conn:Entered badreq for validfilename\n");
		wkr->status = BAD_STATUS;
		send(connfd, BADREQUEST, strlen(BADREQUEST), 0);
                close(connfd);
                //return;
          }
  }


    
    // if wkr->status is already set with an error (is not 0) then
    // we must write this to the log file immediately
    if (wkr->status > 201) {
	//fprintf(stderr, "We have an error status in handle_conn, not performing operation, instead returning with error code: wkr->status: %d\n", wkr->status);
        return; // return to doWorkFcn() to write to log file
    }

  // for debugging purposes
  // fprintf(stderr, "The file name is now: %s\n", filename);

  // request is properly formatted but the type is incorrect
  // so we must check here that request type is valid
  // check request_type is either GET, PUT, or HEAD, otherwise close connection
  int isGet = strcmp(requestType, "GET"); // 0 if true
  int isPut = strcmp(requestType, "PUT");
  int isHead = strcmp(requestType, "HEAD");

  //fprintf(stderr, "performing operation: no error status code\n");
  if (isGet == 0) {
	  // do something for get
	  sprintf((char*)&wkr->requestType, "GET");
	  handleGet(connfd, filename, wkr);
  } else if (isPut == 0) {
	  // add readbytes and the place where we start reading
	  sprintf((char*)&wkr->requestType, "PUT");
	  handlePut(connfd, filename, contentlen, isMoreFileBytes, wkr);
  } else if (isHead == 0) {
	 // do something for head
	 sprintf((char*)&wkr->requestType, "HEAD");
	 handleHead(connfd, filename, wkr);
  } else { // is not get, put or head
	//sprintf(msg, NOTIMPLEMENTED, 0L); // content len = 0
	wkr->status = NOTIMPLEMENTED_STATUS; 
	send(connfd, NOTIMPLEMENTED, strlen(NOTIMPLEMENTED), 0);
  }  
  //fprintf(stderr, "returning from handle_conn\n");
  return;
}

/**
 * If the log file was created, this function is called to check
 * that the log file contains the correct format.
 * Correct format: 3,4,5 tab separated entries upon each line of the log file
 *                 each line is separated by a \n
 */
int log_format_check() {
    int logfd = open(LOG_FILE_NAME, O_RDONLY | O_CREAT | O_APPEND, 0666); // 00700

    if (logfd == -1) {
       errx(EXIT_FAILURE, "The log file: %s cannot be accessed", LOG_FILE_NAME);
    }

    pthread_mutex_lock(&flock_lock);
    flock(logfd, LOCK_EX); // when accessing the log file for reading and checking if contents is valid, we must have an exclusive lock    
    pthread_mutex_unlock(&flock_lock);

    struct stat st;
    stat(LOG_FILE_NAME, &st);
    long int size = st.st_size;
    if (size == 0) {
	    close(logfd);
	    return 1; // valid log file if empty
    }

    int bufferSize = 4096;
    char Buffer[bufferSize + 1];
    char buff[1];
    memset(buff, '\0', 1); 
    lseek(logfd, 0, 0); // args: fd, 0=offset, 0=pointer is set to offset bytes
    ssize_t readbytes = read(logfd, Buffer, bufferSize);
    int i=0;
    int newline = 1; // initially at new line
    int num_elems = 0;
  
    while (readbytes != 0) { 
       i=0; // reset i if we grabbed a new readbytes
       //fprintf(stderr, "Inside log_format_check()\n");
       //fprintf(stderr, "readbytes is: %d\n", (int)readbytes);

       if (readbytes < 0) {
          errx(EXIT_FAILURE, "error when reading log file");
       }
       // loop through each char of grabbed data
       // check that we have 3 or 4 tabs before each newline 
       //for (int i=0; i<(int)readbytes; i++) {
       while (i < (int)readbytes) { 
           buff[0] = Buffer[i];
	   //fprintf(stderr, "buff[0] is: %c\n", buff[0]);
	   if (buff[0] == '\n') {
	       //fprintf(stderr, "buff[0] is a newline ");
	       if (newline == 1) { // if we are at start of new line
	  	  // and immediately we have a \n
		  // invalid
		  //fprintf(stderr, " and we are at a brand new line, invalid\n");
		  // try if at start of new line and it is a \n, ignore invalid
		  close(logfd);
		  return 0;
	     }
	    //fprintf(stderr, "Final num elems for the line is: %d\n", num_elems);
	     if (!((num_elems == 3) || (num_elems == 4) || (num_elems == 5))){
	        close(logfd);
		return 0; //invalid file
	     }
	     num_elems = 0; // reset
	     //fprintf(stderr, "num_elems=0\n");
             newline = 1; // at start of a new line
	     i++;
	 
	   } else { // buff[0] is anything but '\n'
               if (newline == 1) { // if at start of a new line
	           // we don't care what this first elem is
	       	   // if first elem is a tab, doesn't matter
		   // considered first tab spaced elem
		   num_elems++;
		   //fprintf(stderr, "num_elems=%d\n", num_elems);
		   //i++; // move to check next elem
		   newline = 0; // not at a new line now
		   i++;
	        } else { // not at a new line
		   if (buff[0] == '\t') {
		       // we are not at the start of a new line
		       // count the elem after it as an elem
		       //i++; // move to next elem
		       // as long as next element is a tab
		       // we don't care, can be any num of tabs
		       //fprintf(stderr, "buff[0] is a tab\n");
		       while(Buffer[i] == '\t') {
		          i++; 
		       }
		       // reached here, elem is not a tab
		       num_elems++; 
		       //fprintf(stderr, "num_elems=%d\n", num_elems);
		       i--; // go to the very last tab, will update +1 at end
		   }
		   i++;
	       }
	   }
	   memset(buff, '\0', 1);
       }
       readbytes = read(logfd, Buffer, bufferSize);
  }  

  close(logfd);
  return 1; // return 1 for valid file
}


/**
 * Helper function that will write to the log file
 *
 */ 
void logWriter(workerThread* wkr, const char* request_line, int isHealthCheck) {
  // max amount of hex bytes that we write is 2000 bytes
  int logfd = open(LOG_FILE_NAME, O_RDWR | O_CREAT | O_APPEND, 0666); // 00700
  //fprintf(stderr, "opened log file\n");

  if (logfd == -1) {
      errx(EXIT_FAILURE, "The log file: %s cannot be accessed", LOG_FILE_NAME);
  }

  //fprintf(stderr, "About to lock flock_lock mutex\n");
  pthread_mutex_lock(&flock_lock);
  //fprintf(stderr, "About to lock file lock\n");
  flock(logfd, LOCK_EX); // shared lock for all threads that want to write to the log file
  pthread_mutex_unlock(&flock_lock);
  //fprintf(stderr, "unlock flock lock\n");

  // only have contents if:
  // in a healthcheck and type is "GET"
  // if we had a successful request 
  if (wkr->status > 201 || wkr->status == 0) {
      //fprintf(stderr, "in logWriter in error case, wkr->status:%d\n", wkr->status);
  }

  if ((isHealthCheck == 1) && (strcmp(wkr->requestType, "GET") == 0)) { // in a health check
     //fprintf(stderr, "Entered healthcheck in log writer with wkr->status:%d\n", wkr->status);
     char r[1000];
     memset(r, '\0', 1000);
     sprintf(r, "%d\n%d\n", wkr->num_request_errors, wkr->num_requests);
     memset(wkr->first_thou_file_bytes, '\0', 2001); // reset just incase
     // using first thou file bytes to store the content 
     int j = 0;
     for (int i=0; i<(int)strlen(r); i++) {
        sprintf((char*)&wkr->first_thou_file_bytes + j, "%02hhx", r[i]);
        j+=2;
     }
     //fprintf(stderr, "hexContents:\n%s\n", wkr->first_thou_file_bytes);
  } 

  pthread_mutex_lock(&logfile_offset_mutex);
  off_t offset = LOG_OFFSET;
  LOG_OFFSET += wkr->logfile_offset; // logfile offset updated in log_offset
  LOG_OFFSET += (int)strlen(wkr->first_thou_file_bytes) + 1; // plus content in first_thou_file_bytes + 1 for final newline in logfile
  pthread_mutex_unlock(&logfile_offset_mutex);
  
  // write the request line first
  char writedata[5000];
  memset(writedata, '\0', 5000);
 
  ssize_t error = pwrite(logfd, request_line, strlen(request_line), offset); 
  if (error < 0) {
    fsync(logfd);
    close(logfd); 
    return; // nothing I can do
  }
  
  // write the hex data, will write nothing if first_thou_file_bytes is size 0
  int second_offset = offset + wkr->logfile_offset;
  if (strlen(wkr->first_thou_file_bytes) != 0) {
    //fprintf(stderr, "ommitting the content print\n");
    error = pwrite(logfd, &wkr->first_thou_file_bytes, strlen(wkr->first_thou_file_bytes), second_offset);
    if (error < 0) {
       fsync(logfd);
       close(logfd);
       return; // nothing I can do
    }
  }

  int third_offset = second_offset+1;
  error = pwrite(logfd, "\n", 1, third_offset);
  if (error < 0) {
     fsync(logfd);
     close(logfd);
     return;
  }

  //fprintf(stderr, "Done with log writing\n"); 
  fsync(logfd);
  close(logfd);
  return;
}


/* Given a current log file,
 * Reads it in, updates num_requests and num_request_errors global vars
 */
void doHealthCheck(workerThread* wkr) {
    // open the file or create it
    int logfd = open(LOG_FILE_NAME, O_RDONLY | O_CREAT | O_APPEND, 0666); // 00700

    if (logfd == -1) {
       errx(EXIT_FAILURE, "The log file: %s cannot be accessed", LOG_FILE_NAME);
    }
   
    pthread_mutex_lock(&flock_lock);
    flock(logfd, LOCK_EX); // exclusive lock when reading the log file
    pthread_mutex_unlock(&flock_lock);
    int bufferSize = 4096;
    char Buffer[bufferSize + 1];
    char buff[1];
    memset(buff, '\0', 1); 
    int num_requests = 0;
    int num_error = 0;
    ssize_t readbytes = read(logfd, Buffer, bufferSize);
    int i=0;
    int newline = 1; // initially at new line

    while (readbytes != 0) { // while we have more data in the file
       i=0; // reset i if we grabbed a new readbytes
       if (readbytes < 0) {
          errx(EXIT_FAILURE, "error when reading log file");
       }
       // loop through each char of grabbed data
       // check that we have 3 or 4 tabs before each newline 
       //for (int i=0; i<(int)readbytes; i++) {

       while (i < (int)readbytes) { 
           buff[0] = Buffer[i];
	   if (buff[0] == '\n') {
	       //fprintf(stderr, "buff[0] is a newline ");
	       //fprintf(stderr, "Final num elems for the line is: %d\n", num_elems);
               newline = 1; // at start of a new line
	   } else { // buff[0] is anything but '\n'
               if (newline == 1) { // if at start of a new line
	            // check first character of line
	            if (buff[0] == 'F') {
		         num_error++;
	            } 
	            num_requests++;
		    newline = 0; // not at a new line now
	       }
	   }
	   memset(buff, '\0', 1);
	   i++;
       }
       readbytes = read(logfd, Buffer, bufferSize);
  }  

  fsync(logfd);
  close(logfd);
  
  wkr->num_request_errors = num_error;
  wkr->num_requests = num_requests;
  //fprintf(stderr, "Leaving healthcheck, numerrs:%d, numreqs:%d\n", num_error, num_requests); 
}


/** update_offset
 * Function which returns the num of bytes of the first portion of the 
 * message that we will write to the log file.
 * The first portion is the "FAIL\t..." or "PUT\t.."
 * The second portion is the content that we dump into hex
 * We do not deal with the second portion here so we do not 
 * count the bytes yet of the second portion
 */
void update_offset(workerThread* wkr, char* request_line) {
    //fprintf(stderr, "update_offset: entered\n");
    int size = 0;
    char Buffer[500];
    memset(Buffer, '\0', 500);
    if (wkr->status > 201 || wkr->status == 0) { // we have error
         //fprintf(stderr, "in error case, wkr->status: %d\n",wkr->status);
         // ERROR:wkr->req line has nothing in it here
         if (wkr->status == 0) {
            sprintf(request_line, "%s\t%s\t%s", "FAIL", wkr->req_line_from_client, "000");
            size = strlen(request_line);
        } else { // status is a normal error code
            sprintf(request_line, "%s\t%s\t%d", "FAIL", wkr->req_line_from_client, wkr->status);
            size = strlen(request_line);
        }
     } else { // we do not have an error
	// don't handle size of content here, only size of request_line
	// Here we don't use req_line_from_client, we build it ourselves
        
	// only if in case of PUT or GET
	 if ((strcmp(wkr->requestType, "PUT") == 0) || (strcmp(wkr->requestType, "GET") == 0)) {
	    sprintf(request_line, "%s\t/%s\t%s\t%ld\t", wkr->requestType, wkr->filename, wkr->host, wkr->contentlen);
	 } else if (strcmp(wkr->requestType, "HEAD") == 0) {
	    // only if in case of HEAD ends with no \t
	    sprintf(request_line, "%s\t/%s\t%s\t%ld", wkr->requestType, wkr->filename, wkr->host, wkr->contentlen);
	 } else {
	    // if for any reason, we did not catch the error status and we do not have put, get or head
	    sprintf(request_line, "%s\t%s\t%s", "FAIL", wkr->req_line_from_client, "000");
	 }
        size = strlen(request_line);
     }
    // update the log offset of the worker (the first part's num bytes, first part that we write to log file which is the FAIL... or PUT..., not the content)
    wkr->logoffset = size;
    //fprintf(stderr, "update_offset, about to exit back to doWorkFcn with status: wkr->status:%d\n", wkr->status);
    return;
}

/** 
 * Given a worker thread, this function causes the thread
 * to do the work to process the request for the client
 *
 */ 
void *doWorkFcn (void *worker) {
   // so I don't have void pointer warnings
   // cast void ptr type worker to a workerThread ptr type
   workerThread* wkr = (workerThread*) worker;   
   char request_line[500];
   while(1) {
       memset(request_line,'\0',500);
       pthread_mutex_lock(&queue_mutex);
       // dequeue from queue
       int connfd = dequeue(); // take connfd from queue

       while (connfd == -999) { // if no item was in queue then no work to do
	   // make threads wait until there is work
           pthread_cond_wait(&dispatch_condition, &queue_mutex);
           // try to get an connfd off queue again
	   connfd = dequeue();
       }
       // unlock queue operations
       pthread_mutex_unlock(&queue_mutex);
       *wkr->free = -1;
       handle_connection(connfd, wkr);
       
       //fprintf(stderr, "doWorkFcn: Beginning logging: \n");
         
       if ((strcmp(wkr->filename, "healthcheck") == 0)) {
          // if there is no logging to do
          // (no log file)
	  //fprintf(stderr, "doWorkFcn: We are in a healthcheck\n");
	  if ((strcmp(wkr->requestType, "HEAD")) == 0 || (strcmp(wkr->requestType, "PUT") == 0)) {
                // send a 403 error
                //fprintf(stderr, "doWorkFcn: HEAD, PUT healthcheck is error\n");
                wkr->status = FORBIDDEN_STATUS;
                send(connfd, FORBIDDEN, strlen(FORBIDDEN), 0);
                close(connfd);
                update_offset(wkr, request_line);
		//fprintf(stderr, "calling logWriter with reqline:%s\n", request_line);
	        logWriter(wkr, request_line, 1);
	   } else if (isLogging == 0) {
                // if in a health check but there is no logging to do (no logfd)
                // throw a 404 error
	        //fprintf(stderr, "doWorkFcn: Log file dne, in a healthcheck\n");
		wkr->status = FILENOTFOUND_STATUS;
                send(connfd, FILENOTFOUND, strlen(FILENOTFOUND), 0);
                close(connfd);
	        // no logging to do here 
	   } else if ((strcmp(wkr->requestType, "GET") == 0)) {
                // check if log file has valid format, in healthcheck
	        //fprintf(stderr, "doWorkFcn: GET healthcheck: calling log_format_check\n");
                int result = log_format_check();
       	        if (result == 0) {
                   // the log file is corrupt
	           // send 500 response
		   //fprintf(stderr, "doWorkFcn: log format came back as bad format\n");
                   wkr->status = INTERNALSERVERERROR_STATUS;
                   send(connfd, INTERNALSERVERERROR, strlen(INTERNALSERVERERROR), 0); 
	           close(connfd);
                   update_offset(wkr, request_line);
	           logWriter(wkr, request_line, 1);
	        } else {
	           doHealthCheck(wkr);
		   // update content len of wkr
		   char c[500];
                   memset(c, '\0', 500);
	           //pthread_mutex_lock(&numreq_lock); // when grabbing num req vals
                   sprintf(c, "%d\n%d\n", wkr->num_request_errors, wkr->num_requests);
                   wkr->contentlen = strlen(c);
	           //pthread_mutex_unlock(&numreq_lock); // when grabbing num req vals
	           // reached here, has valid format
	           // perform the health check with 200 OK msg
	           char msg[1000];
	           memset(msg, '\0', 1000);
        	   sprintf(msg, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n%s", (long)strlen(c), c);
	      
                   wkr->status = OK_STATUS;
	           send(connfd, msg, strlen(msg), 0);
	           close(connfd);
	           //fprintf(stderr, "doWorkFcn: in GET healthcheck: calling update_offset\n");
                   update_offset(wkr, request_line);
	           //fprintf(stderr, "doWorkFcn: returned from update_offset, now calling logWriter\n");
	           logWriter(wkr, request_line, 1);
		}
	      }
       } else { // else, file is not a healthcheck
           // not in a healthcheck, do the logging if there is a log file
	   //fprintf(stderr, "value of isLogging: %d\n", isLogging);
           if (isLogging == 1) {
	      //fprintf(stderr, "inside, if isLogging = 1\n");
              // do the logging
              //fprintf(stderr, "doWorkFcn: calling update_offset on finished operation\n");
              update_offset(wkr, request_line);
	      //fprintf(stderr, "doWorkFcn: calling logWriter on finished operation\n");
              logWriter(wkr, request_line, 0);
              }
        }
        //fprintf(stderr, "reached here, erasing variables\n");

        // when done with everything, RESET worker's char variables
        wkr->status = 0;
	wkr->putBufferSize = 0;
	wkr->num_request_errors = 0;
	wkr->num_requests = 0;
        memset(wkr->httprequest_msg, '\0', 4096);
        memset(wkr->requestType, '\0', 50);
        memset(wkr->filename, '\0', 50);
        wkr->contentlen = 0;
        memset(wkr->first_thou_file_bytes, '\0', 2001); // may cause bin data issue
        memset(wkr->putBuffer, '\0', 8192); // may cause bin data issue
	memset(wkr->req_line_from_client, '\0', 1001);
   }
}


/**
 * Main: Checks for valid arguments, creates and dispatches the worker
 *       threads 
 */ 
int main(int argc, char *argv[]) {
  int listenfd;
  uint16_t port;

  // http 8080
  // http 8080 -l logfile, http -l logfile 8080
  // http 8080 -N 5, http -N 5
  // http 8080 -N 5 -l log, http -N 5 -l log 8080, http -l log -N 8080, http -l log 8080 -N 5


  // we can have at most 6 arguments: httpserver port -N n -l l
  // min 2 args, max 6 args
  // can have 2, 4, or 6 args
  // cannot have 3 or 5 args
  if ((argc < 2) || (argc > 6) || (argc == 3) || (argc == 5)) {
    errx(EXIT_FAILURE, "wrong arguments for: %s", argv[0]);
  } 
  int DEFAULT_THREADS = 5;

  // collect args 
  int numThreads = DEFAULT_THREADS; // set to default
  char *logfilename = NULL;    // stores the log file, if we have one
  //int port = 0;              // port number
  int getNumThreads = 0; // set if curr arg is num threads
  int getLogFile = 0;    // set if curr arg is log file
  int havePortNum = 0;   // set if we have a port number
  int haveLog = 0;       // set if we have logging to do
  //int logfd = -1;

  for(int i=1; i<argc; i++) {
    // curr arg is num threads
    if (getNumThreads == 1) {
       if (canConvertToInt(argv[i]) == 1) { // successfully converted to int
           numThreads = atoi(argv[i]);
	   getNumThreads = 0; // reset bool var
	   if (numThreads < 1) {
              errx(EXIT_FAILURE, "invalid number of threads: %s", argv[i]);    
	   }
       } else { // unsuccessful conversion to int
           errx(EXIT_FAILURE, "invalid number of threads: %s", argv[i]);    
       }	       
    }
    // curr arg is log file
    else if (getLogFile == 1) {
       logfilename = argv[i];   // save curr arg as log file name
       getLogFile = 0;      // reset bool var
    }
    // curr arg is -N
    else if (strcmp(argv[i], "-N") == 0) {
       getNumThreads = 1; // set bool to grab num threads for next arg
    }
    // curr arg is -l
    else if (strcmp(argv[i], "-l") == 0) {
       haveLog = 1; // set, we have logging to do
       isLogging = 1;
       getLogFile = 1;    // set bool to grab log file for next arg
    }
    // not a -N, -l, or their values
    // then have a port number
    else if ((getNumThreads == 0) && (getLogFile == 0)) {
	// check if we have already caught a port num
	// then we have something like: httpserver 1234 somearg
	// where somearg is an arg with no flag (had no -N or -l flag)
	if (havePortNum == 1) {
           errx(EXIT_FAILURE, "invalid argument: %s", argv[i]);
	}	
        // initialize server socket
        havePortNum = 1;
	port = strtouint16(argv[i]);
        if (port <= 1024) {
            errx(EXIT_FAILURE, "invalid port number: %d", port);
        }
    }
  }

  // check that we were given a port number
  // if not, exit 
  if (havePortNum == 0) {
     errx(EXIT_FAILURE, "no port number"); 
  }
  
  listenfd = create_listen_socket(port); // creates listening socket to listen for connections
  
  
  // checking for errors
  //fprintf(stderr, "prog: %s\nport: %d\nnumThreads: %d\nlogfile: %s\n", argv[0], port, numThreads, logfilename); 
  
  // if we have a log file name, then we have logging to do
  int logCreated = 1; // bool to check if log file created or already existed
  
  if (haveLog == 1) { // if we have logging to do
    if (logfilename == NULL) { // if we do not have a logfilename
      errx(EXIT_FAILURE, "no log file name");
    }
    // open log file for reading and writing
    int file_exists;
    int file_readpermission;
    int file_writepermission;
    // check if file exists
    file_exists = access(logfilename, F_OK); // 0 if access permitted, -1 otherwise
    memset(LOG_FILE_NAME, '\0', 500);
    sprintf(LOG_FILE_NAME, "%s", logfilename);
    if (file_exists == 0) { // if file does exist
        // check if file has write and read permission
        file_writepermission = access(logfilename, W_OK);
	file_readpermission = access(logfilename, R_OK);
        if (file_writepermission == -1) {
            errx(EXIT_FAILURE, "log file cannot be opened for writing: %s", logfilename);
        }
	if (file_readpermission == -1) {
	   errx(EXIT_FAILURE, "log file cannot be opened for reading: %s", logfilename);
	}
        logCreated = 0; // log file already existed
    }
    
    // if log file was not created and already existed
    // we must check it follows the specified format
    if (logCreated == 0) { 
       // before first log format check, update log offset
       // we must know how many lines are in the file
       // this is the offset
              
       struct stat st;
       stat(LOG_FILE_NAME, &st);
       off_t size = st.st_size;
       LOG_OFFSET = size;
        
       int result = log_format_check();
       if (result == 0) {
	  //fprintf(stderr, "Log is of incorrect format! exiting\n");
          errx(EXIT_FAILURE, "the log file has incorrect format: %s", logfilename);
       }
    } else { // if log does not already exist (logCreated = 1, must be created)
       // open the log file
       int logfd = open(LOG_FILE_NAME, O_RDONLY | O_CREAT | O_APPEND, 0666); // 00700

       if (logfd == -1) {
          errx(EXIT_FAILURE, "The log file: %s cannot be accessed", LOG_FILE_NAME);
       }
       // close the log file
       close(logfd);

    }
  }
 
  // creating pool of worker threads
  workerThread workers[numThreads];

  NUMTHREADS = numThreads;
  int allFree = -1; // if no threads working, val = -1

  // set all initial worker thread attributes
  for (int i=0; i<numThreads; i++) {
     // id = i
     workers[i].id = i;
     // status code for the message
     workers[i].status = 0;
     // set null httprequest msg
     memset(workers[i].httprequest_msg, '\0', 4096);
     // requestType: PUT, GET, or HEAD
     memset(workers[i].requestType, '\0', 50);
     // filename
     memset(workers[i].filename, '\0', 50);
     // contentlen
     workers[i].contentlen = 0;
     // port num
     workers[i].portnum = (int)port;
     // log offset
     workers[i].logoffset = 0;
     // checks for if we have no running threads
     workers[i].free = &allFree;
     // put buffer size
     workers[i].putBufferSize = 0;
     // num requests and request errors
     workers[i].num_request_errors = 0;
     workers[i].num_requests = 0;

     // create pthread workers 

     int e = pthread_create(&workers[i].thread_id, NULL, &doWorkFcn, &workers[i]);
     if (e) {
        errx(EXIT_FAILURE, "Error creating worker thread number %d", i);
     }
  }
  // self checking
  //fprintf(stderr, "\nCreated %d workers!\n", numThreads);

  // Dispatch:
  // The while loop below is our dispatch
  // where we give a free worker thread a connfd and work to do

  // create a condition variable for our dispatcher
  // note: condition variable: let's threads wait until something 
  //       happens and it can then have work to do
  //       - thread1 can wait() on a cond var meaning it will wait
  //         and not do anything until thread2 calls signal()
  //       - thread2 calls signal() 
  
  while(1) {
    int connfd = accept(listenfd, NULL, NULL); 

    if (connfd < 0) {
      warn("accept error");
      continue;
    }
    
    pthread_mutex_lock(&queue_mutex); 

    // work to do, thread will now run, not all threads free now
    //pthread_cond_signal(&dispatch_condition); 
    allFree = 0; // work to be done
    enqueue(connfd);
    
    pthread_mutex_unlock(&queue_mutex);

    pthread_cond_signal(&dispatch_condition); 
    
    // testing
    if (allFree == -1) {
        pthread_mutex_lock(&logfile_offset_mutex);
        struct stat st;
        stat(LOG_FILE_NAME, &st);
        long int s = st.st_size;
	   
        if (LOG_OFFSET != s) {
           //fprintf(stderr, "Someone switched a logfile on us!!\n");
           LOG_OFFSET = s; // update the new log offset
        }
        pthread_mutex_unlock(&logfile_offset_mutex);
     }
     //end testing

  }
  return EXIT_SUCCESS;
}


