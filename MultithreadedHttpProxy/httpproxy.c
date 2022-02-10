#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <err.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/select.h>
#include <ctype.h>
#include <pthread.h>
#include <getopt.h>
#include "queue.h"
#include "List.h"
#include <fcntl.h>
#include <time.h>

/* Globals */
List CACHE;
int CACHING = 0; // 1 if we are caching, 0 if we are not 
int MODIFY_CACHE_BEHAVIOR = 0; // 1 if we are modifying cache behavior, 0 if we are not
int MAX_FILE_SIZE = 0;
int CACHE_CAPACITY = 0;
int REQ_HEALTH = 0; // how often we request healthcheck
int NUM_SERVERS = 0; // num servers
int TOTAL_REQS = 0;

static pthread_cond_t cond_var = PTHREAD_COND_INITIALIZER; // for worker threads
static pthread_cond_t health_thread_cond = PTHREAD_COND_INITIALIZER; // for healthcheck thread
static pthread_mutex_t queue = PTHREAD_MUTEX_INITIALIZER; // locks queue
static pthread_mutex_t server_to_use = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t total_reqs = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cache_access = PTHREAD_MUTEX_INITIALIZER; // locks linked list (cache) accesses

/* Structs */

/* individual server data */
typedef struct serverData {
    uint16_t port; // port num of the server
    int status; // 1 if server is up, 0 if server is down
    int num_errors; // num errors of the server
    int num_requests; // num requests of the server
} server_t;

/* all servers data */
typedef struct allServers {
    server_t* servers_array; // array of servers 
    server_t *server_to_use;
} servers_t;

/* worker thread */
typedef struct workerThread {
    //servers_t* servers;
    server_t *server_to_use; // we want this to point to the server chosen in servers_t struct
    server_t *servers_array; // we want this to point to the array of all servers    
    int noServerUsed;        // we did not use a server (used cached file or error in headers)
    int storeFileContents;
    char filename[20];       // filename of file we are working with for the request
    int idx;
    int contentlen;          // content length (size of) the file
    int storeFromGet;
} worker_t;

// file type is saved in List.h

/** isValid
 * checks that the argument is a number
 * if it is a number and greater or equal to 0, it is valid
 * If valid, return 1, else return 0
 */
int isValidNonZero(char str[]) {
   char *ptr = NULL;
   strtol(str, &ptr, 0);
   if (ptr == str) {
      // all of input str is made up of chars
      // then could not convert to num
      return 0;
   } else if (*ptr == '\0') { 
      // entire string converted to num
      int n = atoi(str);
      if (n >= 0) { // valid
         return 1;
      } else {      // invalid
	 return 0;
      }
   } else {          
      // part of str was unconverted
      // invalid num
      return 0;
   }
}

/* isValid
 * checks if number can be successfully converted to an integer
 * returns 1 if valid, 0 if not a number
 */
int isValid(char str[]) {
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
   Creates a socket for listening for connections.
   Closes the program and prints an error message on error.
 */
int create_listen_socket(uint16_t port) {
  struct sockaddr_in addr;
  int listenfd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenfd < 0) {
    err(EXIT_FAILURE, "socket error");
  }

  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htons(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(listenfd, (struct sockaddr*)&addr, sizeof addr) < 0) {
    err(EXIT_FAILURE, "bind error");
  }

  if (listen(listenfd, 500) < 0) {
    err(EXIT_FAILURE, "listen error");
  }

  return listenfd;
}

/**
   Creates a socket for connecting to a server running on the same
   computer, listening on the specified port number.  Returns the
   socket file descriptor on success.  On failure, returns -1 and sets
   errno appropriately.
 */
int create_client_socket(uint16_t port) {
  int clientfd = socket(AF_INET, SOCK_STREAM, 0);
  if (clientfd < 0) {
    return -1;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (connect(clientfd, (struct sockaddr*) &addr, sizeof addr)) {
    return -1;
  }
  return clientfd;
}

/** handleHeaders
 * parses through the headers given by a client to a server
 * 
 * If we have a GET request, and file is stored in cache and file is < max file size
 *       return 0, meaning we do not send request to a server, we handle it ourselves and send back correct status along with file data
 * If we have a GET request, and the file is not stored in cache
 *      If file size is > max file size, do not store file in cache
 *         return 1 meaning give request to a server to process
 *      If file size is < max file size, 
 *         return 1 meaning let server handle request but also store file bytes in cache
 * If we have a head request, send 501, return 0 meaning we do not send request to a server
 * If we have a put request, send 501, return 0 meaning we do not send request to a server
 *
 *
 */
int handleHeadersandCache(char *Buffer, int numBytesRead, worker_t* wkr, int server_fd, int server_port) {
   // remove the extra file bytes (if they are there) from the Buffer
   // want just the headers
   int l = 0;
   int end = 0;
   while (l < numBytesRead) {
       if (Buffer[l]=='\r' && Buffer[l+1]=='\n' && Buffer[l+2]=='\r' && Buffer[l+3]=='\n') {
          end = l+4;
	  break;
       }
       l++;
   }
   if (end < numBytesRead) {
       for (int k=end; k<numBytesRead; k++) {
           Buffer[k] = '\0'; // zero out the extra file bytes in the buffer
       }
   } 
	
   // the string is: Buffer
   char *splitmsg = strtok(Buffer, "\r\n");
   char requestType[50];
   char filename_in[100];
   char filename[20];
   char httpversion[50];
   memset(requestType, '\0', 50);
   memset(filename_in, '\0', 100);
   memset(filename, '\0', 20);
   memset(httpversion, '\0', 50);
   sscanf(splitmsg, "%s %s %s", requestType, filename_in, httpversion);

   if (strcmp(requestType, "GET") != 0) {
      return 501; // Not implemented
   }
   if (strcmp(httpversion, "HTTP/1.1") != 0) {
      return 400;
   }
   memmove(filename, filename_in+1, strlen(filename_in)); // exclude first /
   int len = (int)strlen(filename);
   if (len > 19) {
      return 400;
   }
   
   for (int k=1; k<len; k++) { 
      if (!(isalpha(filename[k]) || isdigit(filename[k]) || filename[k] == '_' || filename[k] == '.')) {
          return 400;
      }
   }

   /* collect headers */
   char *token = strtok(NULL, "\r\n");
   long token2;
   long tokenHost;
   char hostname[50];
   char header[50];
   char body[50];
   char buff[20];
   memset(hostname, '\0', 50);
   memset(header, '\0', 50);
   memset(body, '\0', 50);

   while(token != NULL) {
      memset(buff, '\0', 20);
      tokenHost = sscanf(token, "Host: %s %s", hostname, buff);
      if (tokenHost == 2) {
          // if sscanf saved 2 items, header is bad
	  return 400;
      }      
      if (tokenHost != 1) { // if we did not collect Host header
	 // check that header is valid, has a ":" 
	 token2 = sscanf(token, "%s %s", header, body);
	 if (token2 == 2) { // correct 'header: body' format
	     if (header[strlen(header)-1] != ':') {
	         return 400;
	     }
	 }
      }
      token = strtok(NULL, "\r\n");
   }

   if (CACHING == 1) { // if we are caching at all
       strcpy(wkr->filename, filename);
       //fprintf(stderr, "we are caching\n");
       int size = 1000;
       int size2 = 300;
       char Buffer2[size+1];
       char status_num[size2+1];
       char status_msg[size2+1];
       char contentlen_num[size+1];
       char date[size+1];

       pthread_mutex_lock(&cache_access);
       Node Data = Find(CACHE, filename);
       //fprintf(stderr, "Looking for filename:%s in the cache\n", filename);
       if (Data == NULL) { // NULL when not in cache
          //fprintf(stderr, "Did not find filename:%s in the cache\n", filename);
          // HEAD data: if file size <= max file size, grab size and date
	  wkr->storeFromGet = 1;
          pthread_mutex_unlock(&cache_access);
          return 200; // indicate we must recv 
       } else { // file is in the cache
           //fprintf(stderr, "Found filename:%s in the cache, sending a HEAD to server:%d\n", filename, server_port);
           // Use select() to make sure the server is ready to send bytes back to us
      
           fd_set set;
           FD_ZERO(&set);
           FD_SET(server_fd, &set);	    
           //fprintf(stderr, "File exists in cache, sending request to server:HEAD /%s HTTP/1.1\r\nHost: localhost:%d\r\n\r\n", filename, server_port);
           dprintf(server_fd, "HEAD /%s HTTP/1.1\r\nHost: localhost:%d\r\n\r\n", filename, server_port);
       
           if (select(FD_SETSIZE, &set, NULL, NULL, NULL) <= 0) { // use select() to see if connection open
              pthread_mutex_unlock(&cache_access); 
	      return 500;  // server is down, send internal server error
           } else { 
              // reached here, can connect
              if (FD_ISSET(server_fd, &set)) {
                 memset(Buffer2, '\0', size+1);
                 memset(status_num, '\0', size2+1);
                 memset(status_msg, '\0', size2+1);
                 memset(contentlen_num, '\0', size+1);
	         memset(date, '\0', size+1);
		     
	         int readbytes = recv(server_fd, Buffer2, size, 0); // recving HEAD response
		 if (readbytes < 0) {
		    return 500;
		 }
	         //fprintf(stderr, "caching: read the bytes Buffer2: %s\n", Buffer2);
                 int num_items_scanned = sscanf(Buffer2, "HTTP/1.1 %s %s\r\nContent-Length: %s\r\nLast-Modified: %[^\r\n]", status_num, status_msg, contentlen_num, date);

		 if (num_items_scanned != 4) {
			 pthread_mutex_unlock(&cache_access);
			 //fprintf(stderr, "returning early from handleHeaders, num items scanned is not 4\n");
			 return 500;
		 }
			 
	         date[strlen(date)-4] = '\0'; // remove GMT
	         //fprintf(stderr, "SAVING DATE!!!!!!!!! DATE:%s\n", date);
	         int stat = 0;
	         if (isValid(status_num) == 1) {
	            stat = atoi(status_num);
	         }
	         if (stat != 200) {
	            //fprintf(stderr, "Caching: Error in handleHeadersandCache: status code not 200 from HEAD request toserver\n");
                    pthread_mutex_unlock(&cache_access); 
		    return 500;
	         }  

                 // look at data we got from the HEAD request
	         //fprintf(stderr, "Caching: From Head request: Status:%s, Status Msg: %s, Contentlength:%s, Date:%s\n", status_num, status_msg, contentlen_num, date);
            
	         int cl;
                 if (isValid(contentlen_num) == 1) {
                     cl = atoi(contentlen_num);
		     //fprintf(stderr, "Contenlen is valid\n");
	         } else {
		     //fprintf(stderr, "Contenlen is invalid\n");
                     pthread_mutex_unlock(&cache_access); 
		     return 500;
	         }
	         wkr->contentlen = cl;

		 // HEAD data: if date of HEAD is newer than date in cache
		 // must check if the newest file size <= max file rsize
	         // delete the cache entry of the file (or update the date) and delete contents
	         // set wkr->storeFileContents=1; to store contents later
	         // ELSE: (if file date from head is newer or the same as date in cache)
	         // If -u: move the file entry to the front of the list.
	         // Then we return -999 so we know to use cached file contents
                
                 // check date of cached file vs the one in date buffer 
                 struct tm tm1 = {0}; // date from CACHE
                 struct tm tm2 = {0}; // date from HEAD
		 //fprintf(stderr, "Caching: Data->data->date:%s\n", Data->data->date);
		 //fprintf(stderr, "Caching: date:%s\n", date);
                 strptime(Data->data->date, "%a, %d %b %Y %H:%M:%S", &tm1); 
                 strptime(date, "%a, %d %b %Y %H:%M:%S", &tm2); 
                 //fprintf(stderr, "Caching: Cached file: day:%d, date:%d, month:%d, year:%d, hour:%d, min:%d, sec: %d\n", tm1.tm_wday, tm1.tm_mday, tm1.tm_mon+1, tm1.tm_year+1900, tm1.tm_hour, tm1.tm_min, tm1.tm_sec);
                 //fprintf(stderr, "Caching: file from HEAD: day:%d, date:%d, month:%d, year:%d, hour:%d, min:%d, sec: %d\n", tm2.tm_wday, tm2.tm_mday, tm2.tm_mon+1, tm2.tm_year+1900, tm2.tm_hour, tm2.tm_min, tm2.tm_sec);

                 tm1.tm_isdst = -1; // tells mktime() to determine if daylight savings time 
                 tm2.tm_isdst = -1;
                 time_t t = mktime(&tm1);
                 time_t t2 = mktime(&tm2);
                 if (t == -1 || t2 == -1) {
                    pthread_mutex_unlock(&cache_access);
                    return 500; // internal error with mktime
                 }
                 // check date of t (date from cache) vs t2 (date from head)
                 if (difftime(t, t2) >= 0) { // returns time in seconds of d1-d2
	            // d1 is newer
	            //fprintf(stderr, "date from cache is newer or same\n");
		    if (MODIFY_CACHE_BEHAVIOR == 1) {
		       // if we are to modify cache behavior, file accessed in cache, move to front of cache
                       moveNodetoFront2(CACHE); // move node to front of the list
		       moveFront(CACHE); // keep cursor at current node
		    }
		    wkr->storeFileContents = 0; // do not store file contents
                    pthread_mutex_unlock(&cache_access);
		    return -999; // use cached file

                 } else {
 	             // d2 is newer
	             //fprintf(stderr, "date from cache is older\n");
		     if (cl >= MAX_FILE_SIZE) { // new contents of file > max file size
	                  delete(CACHE); // delete the cache entry
		     } else {
		         if (MODIFY_CACHE_BEHAVIOR == 1) {
		             moveNodetoFront2(CACHE); // move node to front of list
		             moveFront(CACHE); // put cursor at front of list
		         }
		         wkr->storeFileContents = 1;
			 wkr->idx = 0;
                         deleteContentsUpdateDate(CACHE, date, MAX_FILE_SIZE);
		     }
                     pthread_mutex_unlock(&cache_access);
		     return 200; // return and store file contents from the server 
		 } 
              }
           }
        }
     }
    pthread_mutex_unlock(&cache_access);
    return 200; // OK
}


/** handleResponse
 * checks the status code of the response from the server
 * Just returns the status code
 * If it is an error code, we inc num errors of the server later
 */
int handleResponse(char *Buffer, int numBytesRead, worker_t* wkr) {
   // Note: The response is always going to have no errors.
   // There will be times where the response is just file bytes
   // just ignore this case.
   
    char temp[4096+1];
    memset(temp, '\0', 4096+1);
    memcpy(temp, Buffer, numBytesRead);

    int buffSize = 50;
    char status_num2[buffSize+1];
    memset(status_num2, '\0', buffSize+1);
    
    temp[12] = '\0'; // HTTP/1.1 200'\0'
    int scanned_item = sscanf(temp, "HTTP/1.1 %s", status_num2);
    // headers will never come back with bad data from httpservers
    //fprintf(stderr, "handleResponse: Buffer is: %s, status_num:%s\n", Buffer, status_num);
    int stat = 0;
    if (scanned_item == 1) {
       if (isValid(status_num2) == 1) {
          stat = atoi(status_num2);
	  if (stat > 200)return stat; // return the status if bad status
       }
    }

    // reached here, we got back a 200

    char httptype[buffSize+1], status_num[buffSize+1], status_msg[buffSize+1], contentlen_msg[buffSize+1], contentlen_num[buffSize+1], date[buffSize+1];
   
    memset(httptype, '\0', buffSize+1);
    memset(status_num, '\0', buffSize+1);
    memset(status_msg, '\0', buffSize+1);
    memset(contentlen_msg, '\0', buffSize+1);
    memset(contentlen_num, '\0', buffSize+1);
    memset(date, '\0', buffSize+1);
    
    // delete extra bytes of file if they are here  
    int cl;
    int l = 0;
    int end = 0;
    while (l < numBytesRead) {
        if (Buffer[l]=='\r' && Buffer[l+1]=='\n' && Buffer[l+2]=='\r' && Buffer[l+3]=='\n') {
           end = l+4;
	   break;
        }
        l++;
    }

    if (end == 0)return 200; // never found \r\n\r\n then we are dealing with file bytes only, return

    if (end < numBytesRead) {
        for (int k=end; k<numBytesRead; k++) {
            Buffer[k] = '\0'; // zero out the extra file bytes in the buffer
        }
    }    

    // scan items in headers 
    int num_items_scanned = sscanf(Buffer, "%s %s %s\r\n%s %s\nLast-Modified: %[^\r\n]", httptype, status_num, status_msg, contentlen_msg, contentlen_num, date);

    //fprintf(stderr, "Making sure I have the right date info, date:%s", date);
    //fprintf(stderr, "inside handleResponse: Buffer: %s\n", Buffer);
    //fprintf(stderr, "number of items scanned is: %d\n", num_items_scanned);
    
    if (num_items_scanned == 6) {
         //fprintf(stderr, "httptype:%s, status_num:%s, status_msg:%s, contentlen_msg:%s, contentlen_num:%s, date:%s\n", httptype, status_num, status_msg, contentlen_msg, contentlen_num, date);
         if (isValid(status_num) == 1) {
              stat = atoi(status_num); // status of response
	 } else {
	      //fprintf(stderr, "status_num could not be converted to an int, status_num: %s\n", status_num);
	      return 500;
	 }
	 if (isValid(contentlen_num) == 1) {
	     cl = atoi(contentlen_num);
	 } else {
	     //fprintf(stderr, "contentlen_num is invalid, could not be converted to int: %s\n", contentlen_num);
	     return 500;
	 }
	 date[strlen(date)-4] = '\0'; // remove the GMT
    } else {
       //fprintf(stderr, "handleResponse: num of items scanned is not 8, sending 500\n");
       return 500;
    }

    pthread_mutex_lock(&cache_access);
    if (CACHING == 1 && wkr->storeFromGet == 1) { // we are caching and must store file info from GET headers
        // add file data  	
        //fprintf(stderr, "Adding a new cache entry, file:%s\n", wkr->filename);
	// if file size <= max file size, add new enty to front of cache
	if (cl <= MAX_FILE_SIZE) {
	    
	   if (length(CACHE) >= CACHE_CAPACITY) { // if cache full, delete tail)
	       //fprintf(stderr, "Deleting the back of the cache if it is full.\n");
	       deleteBack(CACHE);
	       //fprintf(stderr, "CACHE IS NOW:\n");
	       //printList(CACHE);
               //fprintf(stderr, "End printing cache\n");
	    }

	    //fprintf(stderr, "file size < MAX, adding new cache node\n");
	    prepend(CACHE, MAX_FILE_SIZE, wkr->filename, date, cl); // start with idx=0
            wkr->idx = 0; // contents index starts as 0
	    moveFront(CACHE); // move cursor to front of list
	    wkr->contentlen = cl;
	    wkr->storeFileContents = 1; // indicate to store file contents later
            //fprintf(stderr, "maxfilesize: %d, file size: %d\n", MAX_FILE_SIZE, cl);
	  }
    }
    pthread_mutex_unlock(&cache_access);
    return 200; // indicate that we do not have an error
}



/** forward_request_or_response
 * when given isRequest = 1: (we have a request)
 *    receives the request from the client
 *    parses the request
 *    sends the request to a best chosen server
 * when given isRequest = 0: (we have a response)
 *    receives the response from the server
 *    sends the response back to the client
 *
 */
int forward_request_or_response(int client_fd, int server_fd, int isRequest, server_t** server, worker_t* wkr, int server_port) {
    //fprintf(stderr, "Entered forward_request-or_response\n");
    int size = 4096;
    int result = 1; // 1 if no error, 0 if error
    uint8_t recv_data_cpy[size+1];
    uint8_t recv_data[size + 1];
    memset(recv_data, 0, size+1);

    /* receive data from the client */
    int recv_bytes = recv(client_fd, recv_data, size, 0);
    memset(recv_data_cpy, 0, size+1);
    memcpy(recv_data_cpy, recv_data, recv_bytes);
    
    if (recv_bytes < 0) {
       //fprintf(stderr, "error in recv()\n");
       return -1;
    }
    if (recv_bytes == 0) {
       //fprintf(stderr, "received all bytes\n");
       return 0;
    }

    if (isRequest == 1) { // must handle request by parsing headers from client
         //fprintf(stderr, "We received bytes from client. They are:\n%s\n", recv_data);
	 // result returns error code or -999 if we are going to pull file from the cache 
	 // DO NOT use a server
	 result = handleHeadersandCache((char*)recv_data_cpy, recv_bytes, wkr, server_fd, server_port);
	 if (result == -999) { // send file bytes from cache
            // wkr->noServerUsed = 1; // NEED to remove this, we did a head on server
	    // send the cached file contents to the client
	    // and do not use a server (return 0 here to not use server)
	    // after sending all the contents, return 0 (we sent all bytes)
            //fprintf(stderr, "SENDING BYTES IN CACHE\n");
	    // send only the exact num of bytes of the file, not all at once? in 4096 bytes at time
	    // send the file contents pointed to by the cursor
	    pthread_mutex_lock(&cache_access);
            Node F = Find(CACHE, wkr->filename);
	    if (F != NULL) {
	       // send headers
	       dprintf(client_fd, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nLast-Modified: %s GMT\r\n\r\n", wkr->contentlen, F->data->date);
	       // send file bytes from cache 
               int sendbytes = send(client_fd, F->data->contents, F->data->filesize, 0);
	       if (sendbytes == -1) {
	          // fprintf(stderr, "Error sending cache bytes to client\n");
	       }
	    } else {
	       // fprintf(stderr, "file to send was NOT FOUND IN CACHE\n");
	    } 
	    pthread_mutex_unlock(&cache_access);
	    //fprintf(stderr, "sent all cached file bytes\n");
	    return 0; 

	 } else if (result != 200) { // had a bad header
             // send the right error message back to the client_fd based on the error code
	     if (result == 500) {
	        dprintf(client_fd, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 22\r\n\r\nInternal Server Error\n");
	     } else if (result == 501) {
	        dprintf(client_fd, "HTTP/1.1 501 Not Implemented\r\nContent-Length: 16\r\n\r\nNot Implemented\n");
	     } else if (result == 400) {
	        dprintf(client_fd, "HTTP/1.1 400 Bad Request\r\nContent-Length: 12\r\n\r\nBad Request\n");
	     }
	     
	     wkr->noServerUsed = 1; // bad headers that we caught, didn't use a server
	     /* HERE: if we never give the request to the server (we return)
	      * then we should not update errors or requests for the server */
	     
	     return 0;
	 }


    } else { // we have a response back to the client 
	 //fprintf(stderr, "We are sending response to the client\n");
	 result = handleResponse((char*)recv_data_cpy, recv_bytes, wkr); // parse the response headers only
         //fprintf(stderr, "The status of the response is: %d\n", result);
	 // no such thing as a "bad" header, headers come from httpserver binary
	 if (result != 200) { // the header had an error code as result
             // do not send err message to client, this is contained in what we recv'd from httpserver
	     
	     /* increment num errors of the server */  
	     pthread_mutex_lock(&server_to_use);
             (**server).num_errors++; // this does not update errors for servers in servers_array
	     pthread_mutex_unlock(&server_to_use);
	 } 
    }
    
     /* send the data from client to server */
     //fprintf(stderr, "send'ing to server (or client)\n");
     int send_bytes = send(server_fd, recv_data, recv_bytes, 0);
     
     //fprintf(stderr, "reached here, then send() is not hanging\n"); 
     if (send_bytes == -1 ) {
         //fprintf(stderr, "error in send()\n");
	 return -1; // internal server error
     }
     if (send_bytes == 0) {
         //fprintf(stderr, "sending bytes finished\n");
	 return 0;
     }

     pthread_mutex_lock(&cache_access);
     if (wkr->storeFileContents == 1) { 
	  //fprintf(stderr, "STORING FILE CONTENTS\n");
          // if we are to store the file contents
	  // add file contents one character at a time (from recv_data of size recv_btyes) into file contents buffer
	  // keep an index of the file contents place that we stopped
	  // and just add to the last place in contents buffer (must do this because of bin data)
	  // then add recv_data to file->size variable
          Node F = Find(CACHE, wkr->filename);
	  if (F != NULL) {
              // only send file bytes, not the header data
	      /* remove headers from recv_data IF they exist
	       * (separated by \r\n\r\n)
	       * and subtract the amount of chars in headers
	       * from recv_bytes */
               //fprintf(stderr, "Caching: new file should now be in cache %s in cache\n", wkr->filename);
               int l = 0;
               int end = 0;
	       int bytes_read_in = recv_bytes;
	       int file_bytes_read = recv_bytes;
	       uint8_t FileData[4096+1];
	       memset(FileData, 0, 4096+1);
               while (l < bytes_read_in) {
                   if (recv_data[l]=='\r' && recv_data[l+1]=='\n' && recv_data[l+2]=='\r' && recv_data[l+3]=='\n') {
                      end = l+4;
	              break;
                   }
                    l++;
               } 
	       //fprintf(stderr, "found last 'n' of rnrn at index: %d in recv_data:%s\n", end, recv_data);
	       //int file_bytes_to_add = recv_bytes;
	       int p = 0;
               if (end < bytes_read_in) { // then we have file data to grab
		  //fprintf(stderr, "there are more file bytes after rnrn\n");
                  for (int k=end; k<bytes_read_in; k++) {
		      //fprintf(stderr, "Adding FileData[%d] = recv_data[%d] = %c\n", p, k, recv_data[k]);
                      FileData[p] = recv_data[k]; // add only the file bytes
		      p++;
                  }
                  file_bytes_read -= end;
		  addToContents(CACHE, FileData, file_bytes_read, wkr->idx);
		  wkr->idx = wkr->idx + file_bytes_read;

               }
	  } else {
	      //fprintf(stderr, "adding to contents: file is NOT IN CACHE\n");
	  }
     }
    pthread_mutex_unlock(&cache_access);
    //fprintf(stderr, "Returning from forward_connection\n");
    return recv_bytes; // return success or error status
}


/** create_duplex_connection
 * returns an int: 0 if error, 1 if we completed the connection
 * given client fd and server fd, we need to be able to forward from client to server
 * then from server to client, so we need two concurrent streams of data 
 * To do this we use select()
 */ 
void create_duplex_connection(int client_fd, int server_fd, server_t** server, worker_t* wkr, int server_port) {
    fd_set set;
    int isRequest = 0;
    int val;

    while(1) {
        FD_ZERO(&set);
	FD_SET(client_fd, &set);
	FD_SET(server_fd, &set);

	val = select(FD_SETSIZE, &set, NULL, NULL, NULL); // val = num descriptors select deems ready
	if (val != -1) {
	    if (val != 0) {
	        
	        if (FD_ISSET(client_fd, &set)) {
		    //fprintf(stderr, "we have a request: want to send from client to server\n");
                    isRequest = 1;
	        } else if (FD_ISSET(server_fd, &set)) {
		    //fprintf(stderr, "we have a response: want to send from server to client\n");
		    isRequest = 0;
	        } else { // internal server error
		    //fprintf(stderr, "client, server error: unable to create duplex connection\n");
                    dprintf(client_fd, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 22\r\n\r\nInternal Server Error\n");
		    return; // internal server error
	        }
	    } else { // if val = 0
	       // do nothing, want to loop again
	    }
       } else { // if val = -1
	   //fprintf(stderr, "Error with select(), returning\n");
           dprintf(client_fd, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 22\r\n\r\nInternal Server Error\n");
	   return;
       }
       
       if (isRequest == 1) { // forward request from client_fd to server_fd
          //fprintf(stderr, "calling forward_data() function\n");	
          int r = forward_request_or_response(client_fd, server_fd, isRequest, server, wkr, server_port);
          //fprintf(stderr, "returning from duplex_connection() in the case of a request\n");
          if (r <= 0) { 
	     return; 
	  }
       } else { // forward response from server_fd to client_fd
           //fprintf(stderr, "calling forward_data() function\n");	
           int r = forward_request_or_response(server_fd, client_fd, isRequest, server, wkr, server_port);
           //fprintf(stderr, "returning from duplex_connection() in the case of a response\n");
           if (r <= 0) { 
	       return; 
	   }
       }
    }
}


/** pick_server_to_use()
 *  Given array of servers, check if server is down and if it is, can't be the best server
 *  if server is up, choose one with least amount of requests
 *  tie break: success rate = errors/requests
 */
void update_server_to_use(worker_t* wkr) {
    int numServers = NUM_SERVERS;
    int bestIndex = 0;
    for (int i=0; i<numServers; i++) { 
	// compare with best server
	//fprintf(stderr, "server[%d]: %d: num reqs: %d, num errs: %d, status: %d\n", i, wkr->servers_array[i].port, wkr->servers_array[i].num_requests, wkr->servers_array[i].num_errors, wkr->servers_array[i].status);

	if (wkr->servers_array[i].status == 1) { // curr server is up (1)
 	    if (wkr->servers_array[bestIndex].status == 0) { // best servers status is down (0)
                bestIndex = i; // replace best server with i
	    } else if (wkr->servers_array[bestIndex].num_requests > wkr->servers_array[i].num_requests) { 
		bestIndex = i; // replace best server with i
	    } else if (wkr->servers_array[bestIndex].num_requests == wkr->servers_array[i].num_requests) {
		// same num requests
		if (wkr->servers_array[bestIndex].num_errors > wkr->servers_array[i].num_errors) {
                    // best server has more errors, replace with i
                    bestIndex = i;
	        }
            }
	}
    }
   
    // now update wkr.server_to_use
    //fprintf(stderr, "About to update wkr.server_to_use with best server: %d\n", wkr->servers_array[bestIndex].port);
    wkr->server_to_use = &(wkr->servers_array[bestIndex]); // update the best server w addr of one in arr
    //fprintf(stderr, "Updated wkr->server_to_use->port: %d\n", wkr->server_to_use->port);
}

/** doWork
 * function in which all threads begin to run
 */
void *doWork(void *worker) {
   worker_t *wkr = (worker_t*)worker;
   int client_fd;
   int server_fd;
   //fprintf(stderr, "Entered doWork fcn\n");

   while(1) {
      pthread_mutex_lock(&queue);
      client_fd = dequeue();
      while (client_fd == -999) { // check this, not supposed to have "if" statement, this is WRONG here w/ dequeue first
         // queue is empty
	 pthread_cond_wait(&cond_var, &queue);
	 client_fd = dequeue();
      }
      // grab client_connfd from queue
      pthread_mutex_unlock(&queue);
      memset(wkr->filename, '\0', 20);
      /* Lock so that we can access server_to_use */
      pthread_mutex_lock(&server_to_use);
      
      /* TESTING 
      fprintf(stderr, "the servers_array for the worker should be exactly same as last updated values, if not worker threads do not share the same instance of servers_array\n");
      for (int i=0; i<NUM_SERVERS; i++) {
          fprintf(stderr, "servers_arr[%d]:%d, num_reqs:%d, num_errs:%d, status:%d\n", i, wkr->servers_array[i].port, wkr->servers_array[i].num_requests, wkr->servers_array[i].num_errors, wkr->servers_array[i].status);
      }
      END TESTING 
      */

      //fprintf(stderr, "Entered update_server_to_use(wkr)\n");
      update_server_to_use(wkr); // before fulfilling request, update the best server
      //fprintf(stderr, "Exited update_server_to_use(wkr)\n");
      int server_port = (int)wkr->server_to_use->port; 
      //fprintf(stderr, "Picked server %d as best\n", server_port);
      int server_connfd;
      int m=1;
      
      //fprintf(stderr, "Checking if the picked best server is up\n");
      while (m < NUM_SERVERS) { // m=1 bc we already picked 1 server
         //fprintf(stderr, "m=%d\n", m);
         server_connfd = create_client_socket(server_port); // If you create server connfd, you must close
	 // if server is down, choose another
	 // But if server is UP
	 // in the case where all servers are down, a down server will end up here
	 // But if it is now "up" then we are not supposed to know this. Check if it is up and its status
	 //fprintf(stderr, "wkr->server_to_use->status == %d\n", wkr->server_to_use->status);
         if (server_connfd != -1 && wkr->server_to_use->status == 1) {
	    //fprintf(stderr, "server connfd is not -1, break while and continue operation\n");
	    close(server_connfd); // close it we will open again later
	    break; // we found running server
	 } else { 
	    //fprintf(stderr, "server: %d connfd is -1, set server as down and pick another\n", wkr->server_to_use->port);
	    wkr->server_to_use->status = 0; // server is not up, set as down
	    //fprintf(stderr, "update server: %d status as %d\n", wkr->server_to_use->port, wkr->server_to_use->status);
            update_server_to_use(wkr); // check again for a new server if not running
	    server_port = (int)wkr->server_to_use->port;
            //fprintf(stderr, "Picked server %d as best\n", server_port);
	    close(server_connfd); // close it (just in case)
	 }
         m++;
      }

      server_fd = (int)wkr->server_to_use->port; 
      //fprintf(stderr, "Found and saving a bestServer: %d\n", wkr->server_to_use->port);
      server_t *bestServer; // save server to use as "bestServer" same addr as server
      bestServer = wkr->server_to_use;
      //fprintf(stderr, "bestServer is: %d\n", bestServer->port);
      int bestStatus = wkr->server_to_use->status; // must come before the lock, status can change
      pthread_mutex_unlock(&server_to_use);


      if (bestStatus == 0) {
         // if all servers unresponsive, send internal error
         // otherwise process the request by creating duplex connection
	 //fprintf(stderr, "All servers are down\n");
	 wkr->noServerUsed = 1; // because all are down
         dprintf(client_fd, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 22\r\n\r\nInternal Server Error\n");
	 //fprintf(stderr, "doWork: failed to connect to server file descriptor");
      } else {
	 // server is valid, create socket
	 //fprintf(stderr, "connected to server file descriptor, creating client socket\n");
	 server_fd = create_client_socket(server_fd);
	 //fprintf(stderr, "creating duplex connection\n");
	 create_duplex_connection(client_fd, server_fd, &bestServer, wkr, bestServer->port);
	 close(server_fd);
      }

      // any errors that occured, must send error to client here.
      // note: result == 0 is used below, make sure to update that.
      close(client_fd);
     
      if (wkr->noServerUsed == 0) { // if we used a server
          pthread_mutex_lock(&server_to_use); //!!!!!!!!!!!!!!!!!!!!!!!!!!! check this 
          bestServer->num_requests += 1;
          pthread_mutex_unlock(&server_to_use);
      } else {
	  //fprintf(stderr, "bad headers or all servers down, not updating num reqs of server\n");
      }

      /* TESTING 
      fprintf(stderr, "may not be good values if ran healthcheck at same time: the servers_array for the worker should now be updated with current req and err values\n");
      for (int i=0; i<NUM_SERVERS; i++) {
          fprintf(stderr, "\nservers_arr[%d]:%d, num_reqs:%d, num_errs:%d\n", i, wkr->servers_array[i].port, wkr->servers_array[i].num_requests, wkr->servers_array[i].num_errors);
      }
       pthread_mutex_lock(&cache_access);
       if (CACHING == 1) {
           //fprintf(stderr, "\n\nTHE CACHE IS NOW:\n");
           printList(CACHE);
           //fprintf(stderr, "\n\n");
       }
       pthread_mutex_unlock(&cache_access);
      */
      /* END TESTING */
      
      // reset thread variable for next run
      wkr->noServerUsed = 0;
      wkr->storeFileContents = 0;
      memset(wkr->filename, '\0', 20);
      wkr->idx = 0;
      wkr->contentlen = 0;
      wkr->storeFromGet = 0;
   }
}


/**
 * This function is where healthcheck worker threads starts working
 * //signals to the healthcheck worker thread
 * //that we need to do a healthcheck on all servers 
 */
void *doHealthcheck(void *servers_d) {
    servers_t servers_data = *(servers_t *)servers_d; // must redefine as servers_t so we can access struct obj
    server_t *servers_arr = servers_data.servers_array; // grab servers array
    //fprintf(stderr, "\n\nEntered healthcheck\n");
    int numServers = NUM_SERVERS;
    fd_set set;
    char get_healthcheck_msg[] = "GET /healthcheck HTTP/1.1\r\nHost: localhost:%d\r\n\r\n";
    int bufferSize = 500;
    int buffSize = 200;
    char Buffer[bufferSize + 1];
    // response from healthcheck: HTTP/1.1 200 OK\r\nContent-Length: 4\nLast-Modified: Sat, 27 Nov 2021 19:47:07 GMT\r\n\r\nErrors\nRequests\n 
     
    char httptype[buffSize+1], status_num[buffSize+1], status_msg[buffSize+1], contentlen_msg[buffSize+1], contentlen_num[buffSize+1], day[buffSize+1], date[buffSize+1], month[buffSize+1], year[buffSize+1], time[buffSize+1], errors[buffSize+1], requests[buffSize+1], gmt[buffSize+1]; 
    int errs = 0;
    int reqs = 0;
    int result1, result2;
    int bestIndex = 0;
    char extraBuff[bufferSize + 1];
    int curr_connfd;

    while(1) {
        //fprintf(stderr, "in healthcheck: attempt server_to_use lock\n");
        pthread_mutex_lock(&server_to_use);
        //fprintf(stderr, "in healthcheck: got server_to_use lock\n");

        // loop through servers and check if active
	for (int i=0; i<numServers; i++) {
	    //fprintf(stderr, "i=0 to numServers=%d, i=%d, checking port: %d\n", numServers, i, servers_arr[i].port);
            servers_arr[i].status = 1; // set status as active
            // call create_client_socket() with each port in the servers arr
	    // to see if we can connect, if port == -1, could not connect
	    curr_connfd = create_client_socket(servers_arr[i].port); 
	    //fprintf(stderr, "doHealthcheck: created client socket with port num: %d, curr_connfd: %d\n", servers_arr[i].port, curr_connfd);
            if (curr_connfd == -1) {
	        servers_arr[i].status = 0;
	    } else {
	       // Now send and receive the healthcheck and update reqs and errors for each server
               FD_ZERO(&set);
               FD_SET(curr_connfd, &set);	    
	       dprintf(curr_connfd, get_healthcheck_msg, servers_arr[i].port);
	       //fprintf(stderr, "doHealthcheck: sent healthcheck to curr_connfd: %d\n", curr_connfd);

	       if (select(FD_SETSIZE, &set, NULL, NULL, NULL) <= 0) { // use select() to see if connection open
	           servers_arr[i].status = 0; // server is down
	       } else { 
	          // reached here, can connect
	          if (FD_ISSET(curr_connfd, &set)) {
	             memset(Buffer, '\0', bufferSize+1);
	             memset(extraBuff, '\0', bufferSize+1);
	             memset(httptype, '\0', buffSize+1);
	             memset(status_num, '\0', buffSize+1);
	             memset(status_msg, '\0', buffSize+1);
	             memset(contentlen_msg, '\0', buffSize+1);
	             memset(contentlen_num, '\0', buffSize+1);
		     memset(day, '\0', buffSize+1);
		     memset(date, '\0', buffSize+1);
		     memset(month, '\0', buffSize+1);
		     memset(year, '\0', buffSize+1);
		     memset(time, '\0', buffSize+1);
	             memset(gmt, '\0', buffSize+1);
		     memset(errors, '\0', buffSize+1);
	             memset(requests, '\0', buffSize+1);
		     
		     //fprintf(stderr, "doHealthcheck: Healthcheck response: %s\n", Buffer);
	             
		     int readbytes = recv(curr_connfd, extraBuff, bufferSize-1, 0);
		     //fprintf(stderr, "readbytes is: %d\n", readbytes);
		     strcat(extraBuff, "\0");
		     strcat(Buffer, extraBuff);

		     /* check that there is stuff after \r\n\r\n */
                     //int isMoreFileBytes = 0;
                     int l = 0;
                     int end = 0;
                     while(l < readbytes){
                          if(Buffer[l] == '\r' && Buffer[l+1] == '\n' && Buffer[l+2] == '\r' && Buffer[l+3] == '\n'){
                              end = l+4;
                              break; // we found our index
                          }
                          l++;
                     } 
                     if (end >= readbytes) { // if end index >= readbytes, we need another recv
		         memset(extraBuff, 0, sizeof(extraBuff));
			 readbytes = recv(curr_connfd, extraBuff, bufferSize-1, 0);
		         //fprintf(stderr, "readbytes is: %d\n", readbytes);
		         strcat(extraBuff, "\0");
		         strcat(Buffer, extraBuff);
                         memset(extraBuff, 0, sizeof(extraBuff));

		     }
		     //fprintf(stderr, "Buffer should contain content: %s\n", Buffer);
	    
	             // from spec:
                     //a health check probe can fail either with the server not connecting, if the response code is not 200, or if the body does not fit the <errors>\n<entries>\n format.
                     // response from healthcheck: HTTP/1.1 200 OK\r\nContent-Length: 4\nLast-Modified: Sat, 27 Nov 2021 19:47:07 GMT\r\n\r\nErrors\nRequests\n 
	             int num_items_scanned = sscanf(Buffer, "%s %s %s\r\n%s %s\nLast-Modified: %s %s %s %s %s %s\r\n\r\n%s\n%s\n", httptype, status_num, status_msg, contentlen_msg, contentlen_num, day, date, month, year, time, gmt, errors, requests);

		     //fprintf(stderr, "httptype:%s, status_num:%s, status_msg:%s, contentlen_msg:%s, contentlen_num:%s, day:%s, date:%s, year:%s, time:%s, gmt:%s, errors:%s, requests:%s\n", httptype, status_num, status_msg, contentlen_msg, contentlen_num, day, date, year, time, gmt, errors, requests);
                     int stat = 0;
		     if (isValid(status_num) == 1) {
		         stat = atoi(status_num); // status of response
		     }
		     //fprintf(stderr, "in doHealthcheck: The status of the 'get healthcheck' response is: %d, if not 200, we consider server as invalid.\n", stat);
		     //fprintf(stderr, "doHealthcheck: num_items_sanned: %d, stat: %d\n", num_items_scanned, stat);
	             if (num_items_scanned == 13 && stat == 200) {
	                 // we have correct format of headers and body
		         // check if response is 200 (if not, server down)
			 // update num reqs and errors for server
			 result1 = isValid(errors);
			 result2 = isValid(requests);
			 if (result1 == 1 && result2 == 1) {
			     //fprintf(stderr, "calling atoi() on errors\n");    
			     errs = atoi(errors);
			     //fprintf(stderr, "calling atoi() on requests\n");    
			     reqs = atoi(requests);
			  
			     //fprintf(stderr, "num_requests:%d, num_errors:%d\n", reqs, errs);
			     //fprintf(stderr, "updating servers_arr[i].num_requests and num_errors\n");    
	                     servers_arr[i].num_requests = reqs+1; // +1 including healthcheck we did
                             servers_arr[i].num_errors = errs;
			 } else { 
			     // invalid nums for reqs and errs
			     //fprintf(stderr, "invalid nums for reqs and errors\n");
			     servers_arr[i].status = 0;
			     // if server down, dont need to worry about numbers, we dont choose it
			 }
	             } else { // incorrect format of response, consider server as down
	                 servers_arr[i].status = 0;
			 // if server is down, don't need to worry about nums, we won't choose it
	             }
		  }
	       }
	    }
	    // Now we have saved num errors and num requests for server[i]
	    //fprintf(stderr, "\nHealthcheck data for server port: %d, numreqs:%d, numerrs:%d\n", servers_arr[i].port, servers_arr[i].num_requests, servers_arr[i].num_errors);
	    close(curr_connfd);
	} // end for 
        
        
        // pick best server now that all entries/errors have been updated
        for (int i=0; i<numServers; i++) { 
	    // compare with best server
            //fprintf(stderr, "checking if port:%d is best server in healthcheck\n", servers_arr[i].port);
	    if (servers_arr[i].status == 1) { // curr server is up (1)
 	        if (servers_arr[bestIndex].status == 0) { // best servers status is down (0)
                    bestIndex = i; // replace best server with i
	        } else if (servers_arr[bestIndex].num_requests > servers_arr[i].num_requests) { 
		    bestIndex = i; // replace best server with i
		} else if (servers_arr[bestIndex].num_requests == servers_arr[i].num_requests) {
		    // same num requests
		    if (servers_arr[bestIndex].num_errors > servers_arr[i].num_errors) {
                        // best server has more errors, replace with i
			bestIndex = i;
		    }
		}
	    }
	}
 
	//fprintf(stderr, "Finishing healthcheck: Best server is : servers_arr[%d]: %d\n", bestIndex, servers_arr[bestIndex].port);
        
        //*(servers_data.server_to_use) = servers_arr[bestIndex];	
        servers_data.server_to_use = &(servers_arr[bestIndex]); // point to addr of same server in arr	
        
	// if all servers are down, best server will be down
        // we deal with this in doWork(), if best server is down
	
        //fprintf(stderr, "in healthcheck: attempt server_to_use unlock\n");
	pthread_mutex_unlock(&server_to_use);
        //fprintf(stderr, "in healthcheck: finished server_to_use unlock\n");

        //fprintf(stderr, "doHealthcheck: leaving\n\n");
    
        pthread_mutex_lock(&total_reqs);
        pthread_cond_wait(&health_thread_cond, &total_reqs);
        pthread_mutex_unlock(&total_reqs);
    } // end while

}


/** signalHealthcheck()
 * This function signals to the healthcheck thread to start working,
 * if we need to do a healthcheck
 * Then the healthcheck thread will begin working in doHealthcheck()
 */
//void signalHealthcheck(servers_t* servers_data) {
void signalHealthcheck(int newRequest) {
    // if it is not time for a healthcheck, return
    // do a healthcheck at 0 requests and every Rth request after
    
    pthread_mutex_lock(&total_reqs);
    if (newRequest == 1) {
       TOTAL_REQS++;
       // can we cause thread to wait here for a second
       // it is grabbing server_to_use lock before the doWorkFcn which is not good
       // healthcheck should happen after the nth request not before
       //usleep(10);
    }
    if (TOTAL_REQS % REQ_HEALTH == 0) {
        pthread_cond_signal(&health_thread_cond);
    }
    pthread_mutex_unlock(&total_reqs);
	    
}


int main(int argc, char *argv[]) {
  int listenfd;
  uint16_t port;
  uint16_t server;                // holds current server port num (that we receive from) 
  int numThreads = 5;             // default, -N flag
  // must do all healthcheck on servers when we start (0) and every R requests after
  int req_healthchecks = 5;       // default, -R flag
  int cache_capacity = 3;         // default, -s flag
  int max_file_size = 1024;       // default, -m flag
  int modify_cache_behavior = 0;  // -u flag
  int r, gotN = 0, gotR = 0, gots = 0, gotm = 0, gotu = 0, cmp1, cmp2, cmp3, cmp4;
  int numServers = argc - 2; // num of port_to_connects, first two args always progname and port (listening port)
  int gotListeningPort = 0;

  // struct objects: servers_data and worker_data
  // so that thread can take its own information with it throughout the functions
  servers_t servers_data;
  //worker_t worker_data;
  //worker_data.noServerUsed = 0;
  //worker_data.storeFileContents = 0;
  
  /*
  //worker_data.server_to_use = malloc(sizeof(server_t)); // create space for server_to_use

  // we want servers_data's server_to_use to be the same as worker_data's server_to_use
  servers_data.server_to_use = worker_data.server_to_use; // set pointers =, so they contain same value
  */

  if (argc < 3) {
    errx(EXIT_FAILURE, "wrong arguments: %s port_to_listen, port_to_connect", argv[0]);
  }

  /* Parse the arguments */
  for (int i=1; i<argc; i++) {
    if (strcmp(argv[i-1], "-N") == 0) {
	if (gotN == 0) {
           gotN = 1;
	   r = isValidNonZero(argv[i]);
	   if (r == 0) {
              errx(EXIT_FAILURE, "invalid number for -N: %s", argv[i]);
	   }
	   numThreads = atoi(argv[i]);
	   numServers-=2;
        } else { // we already got N, have duplicate
           errx(EXIT_FAILURE, "duplicate -N value: %s", argv[i]);
        }
    } 
    else if (strcmp(argv[i-1], "-R") == 0) {
        if (gotR == 0) {
           gotR = 1;
           r = isValidNonZero(argv[i]);
           if (r == 0) {
              errx(EXIT_FAILURE, "invalid number for -R: %s", argv[i]);
           }
           req_healthchecks = atoi(argv[i]);
	   //servers_data.req_healthcheck = req_healthchecks;
           numServers-=2;
        } else { // we already got N, have duplicate
           errx(EXIT_FAILURE, "duplicate -R value: %s", argv[i]);
        }
    }        
     else if (strcmp(argv[i-1], "-s")  == 0) {
        if (gots == 0) {
           gots = 1;
           r = isValid(argv[i]);
           if (r == 0) {
              errx(EXIT_FAILURE, "invalid number for -s: %s", argv[i]);
           }
           cache_capacity = atoi(argv[i]);
           numServers-=2;
        } else { // we already got N, have duplicate
           errx(EXIT_FAILURE, "duplicate -s value: %s", argv[i]);
        }
    }
    else if (strcmp(argv[i-1], "-m") == 0) {
        if (gotm == 0) {
           gotm = 1;
           r = isValid(argv[i]);
           if (r == 0) {
              errx(EXIT_FAILURE, "invalid number for -m: %s", argv[i]);
           }
	   //fprintf(stderr, "Before aoi(), max_file_size = %s\n", argv[i]);
           max_file_size = atoi(argv[i]);
	   //fprintf(stderr, "HERE!!! MAX FILE SIZE IS: %d\n", max_file_size);
           numServers-=2;
        } else { // we already got N, have duplicate
           errx(EXIT_FAILURE, "duplicate -m value: %s", argv[i]);
        }
    }
    else if (strcmp(argv[i], "-u") == 0) {
        if (gotu == 0) {
           gotu = 1;
           modify_cache_behavior = 1;
           numServers-=1;
        } else { // we already got N, have duplicate
           errx(EXIT_FAILURE, "duplicate -u");
        }
    }
    else {
	// get here if curr arg does not have a known flag behind it
        cmp1 = strcmp(argv[i], "-N");
        cmp2 = strcmp(argv[i], "-R");
        cmp3 = strcmp(argv[i], "-s");
        cmp4 = strcmp(argv[i], "-m");
	// if no cmp's are 0, then we either should have a port number (possibility of invalid flag or port)
	if (cmp1 == 0 || cmp2 == 0 || cmp3 == 0 || cmp4 == 0) {
            // argv[i] is a known flag, do nothing
	} else { 
	    // argv[i] is not a known flag
	    // check if valid port num
            server = strtouint16(argv[i]);
            if (server == 0) {
               errx(EXIT_FAILURE, "invalid port number: %s", argv[i]);
            }
	    if (gotListeningPort == 0) { // the first port num is the listening port
                gotListeningPort = 1;
		port = server; 
	    }
	}
     }
  }
  /* End Parsing Arguments */  

  /* Update Globals */
  // If there is no caching to be done
  if (cache_capacity == 0 || max_file_size == 0) {
     CACHING = 0; 
     CACHE_CAPACITY = 0;
     MAX_FILE_SIZE = 0;
     MODIFY_CACHE_BEHAVIOR = 0;
  } else { // caching to be done, update global variables
     CACHING = 1;
     CACHE_CAPACITY = cache_capacity;
     MAX_FILE_SIZE = max_file_size;
     //fprintf(stderr, "saving MAX_FILE_SIZE:%d\n", MAX_FILE_SIZE);
     MODIFY_CACHE_BEHAVIOR = modify_cache_behavior;
    // if (modify_cache_behavior == 1)fprintf(stderr, "MODIFYING CACHE BEHAVIOR\n");
    // else fprintf(stderr, "NOT MODIFYING CACHE BEHAVIOR\n");
     CACHE = newList();
     //freeList(CACHE);
     //CACHE = newList(); 
  }
  REQ_HEALTH = req_healthchecks;
  NUM_SERVERS = numServers;

  // turn caching off
  // CACHING = 0;

  // If we have 0 or less connect-to ports, exit
  if (numServers < 1) {
     errx(EXIT_FAILURE, "must have atleast one connect-to-port");
  }

  // If we did not get a listening port, exit
  if (gotListeningPort == 0) { // if we never received a port number
     errx(EXIT_FAILURE, "no port numbers provided");
  }
  /* End Updating Globals */


  /* Loop through arguments again and save server port numbers into array */
  // Now we know how many server port numbers (receive-from ports) to grab
  // exclude the first one (which is the listening port)
  gotListeningPort = 0;

  //fprintf(stderr, "numServers: %d\n", numServers);
  //uint16_t servers[numServers];
  //int workingServers[numServers];

  servers_data.servers_array = malloc(sizeof(server_t)*(numServers)); 
  servers_data.server_to_use = servers_data.servers_array;

  //worker_data.servers_array = servers_data.servers_array; // set pointers =, should contain same value now 
 
  //worker_data.server_to_use = &servers_data.servers_array[0]; // set to random server addr in array 
  // servers_data.server_to_use = worker_data.server_to_use; // set pointers =, so they contain same value
  //servers_data.server_to_use = &worker_data.server_to_use; // set pointers =, so they contain same value
  
  int serverIndex = 0;

  for (int i=1; i<argc; i++) {
     if (strcmp(argv[i-1], "-N") == 0) { // do nothing
     } else if (strcmp(argv[i-1], "-R") == 0) { 
     } else if  (strcmp(argv[i-1], "-s") == 0) { 
     } else if  (strcmp(argv[i-1], "-m") == 0) { 
     } else if  (strcmp(argv[i], "-u") == 0) { 
     } else { // prev item is not a flag
       cmp1 = strcmp(argv[i], "-N");
       cmp2 = strcmp(argv[i], "-R");
       cmp3 = strcmp(argv[i], "-s");
       cmp4 = strcmp(argv[i], "-m");
       // check if curr item is also not a flag
       if (!(cmp1 == 0 || cmp2 == 0 || cmp3 == 0 || cmp4 == 0)) {
          //fprintf(stderr, "found a valid server port: %s\n", argv[i]);
	  if (gotListeningPort == 0) {
              // at first port (listening port), do not save into connect-to-ports
              gotListeningPort = 1;
	  } else {
       	      server = strtouint16(argv[i]);
	      //servers[serverIndex] = server;
	       
	      servers_data.servers_array[serverIndex].port = server;
	      //fprintf(stderr, "saving servers data array info here: servers_data.servers_array[%d].port= %d\n", serverIndex, server);
	      serverIndex++;
	  } 
       }   
    }
  }
  /* End saving server port numbers into array */

  /* Testing */
  /*
  fprintf(stderr, "Args:\nport:%d\n-N:%d\n-R:%d\n-s:%d\n-m:%d\n-u:%d\nServers: ", (int)port, numThreads, req_healthchecks, cache_capacity, max_file_size, MODIFY_CACHE_BEHAVIOR); 

  for (int i=0; i<numServers; i++) {
     fprintf(stderr, "servers_data.servers_array[%d].port: %d\n", i, servers_data.servers_array[i].port);
  }
  fprintf(stderr, "\n");
  */
  /* End Testing */
  
  pthread_t workers[numThreads + 1]; // num parallel connections we can service at one time 
  
  worker_t workers_array[numThreads]; // num of worker threads

  listenfd = create_listen_socket(port);

  for (int i=0; i<numThreads; i++) {
    workers_array[i].servers_array = servers_data.servers_array; // set pointers =, should contain same value now 
    workers_array[i].server_to_use = servers_data.server_to_use; // server_to_use is a ptr to 1 server in servers_array
    workers_array[i].noServerUsed = 0;
    workers_array[i].storeFileContents = 0;
    workers_array[i].idx = 0;
    workers_array[i].contentlen = 0; 
    workers_array[i].storeFromGet = 0; 
    
    pthread_create(&workers[i], NULL, doWork, &workers_array[i]);
  }
  
    
  // create one thread to do healthchecks
  pthread_create(&workers[numThreads], NULL, doHealthcheck, &servers_data);

  // set total requests processed to 0
  TOTAL_REQS = 0;

  //signalHealthcheck(&servers_data); // do one healthcheck initially
  signalHealthcheck(0);

  while(1) {
    // check if we need to signal healthcheck thread to perform healthcheck
    // this updates SERVER_TO_USE
    //signalHealthcheck(&servers_data);
    //signalHealthcheck();

    int client_connfd = accept(listenfd, NULL, NULL);
    if (client_connfd < 0) {
      warn("accept error");
      continue;
    } else {
      // signal to worker threads there is work to do (clientfd on queue)
      pthread_mutex_lock(&queue);
      enqueue(client_connfd);
      pthread_mutex_unlock(&queue);
      pthread_cond_signal(&cond_var);

      signalHealthcheck(1);
    }
    // see if we need to signal healthcheck thread to perform healthcheck
    // call fcn that signals to the healthcheck thread to do healthcheck work
    // this function is only called when there is new work to do in queue (when there is a request)
    //signalHealthcheck(&servers_data); // pass it the servers array
  }

  free(servers_data.servers_array); // will not execute but just in case
  return EXIT_SUCCESS;
}
  
