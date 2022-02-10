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

#include <stdio.h>
#include <ctype.h>

// messages (note: resource name means file name)
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

// to do: remove the print statements
// try deleting the : checks for the tokens


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
void handleGet(int connfd, char* file) {
    // if file does not exist, send file not found error
    // if we do not have read permission for file, send forbidden msg
    // open file to read only
    // create buffer of size contentlen to store data
    int file_exists;
    int file_readpermission;
    char msg[150];
    msg[150-1] = '\0';
    // check if file exists
    file_exists = access(file, F_OK); // 0 if access permitted, -1 otherwise
    if (file_exists == -1) {
        // send a file not found message
        //sprintf(msg, FILENOTFOUND, 0L); // contentlen should be 0 if file not found
        send(connfd, FILENOTFOUND, strlen(FILENOTFOUND), 0);
        close(connfd);
        return;
    }
    // check read permission
    file_readpermission = access(file, R_OK); // 0 if access permitted, -1 otherwise
    if (file_readpermission == -1) {
        // send forbidden message
        //sprintf(msg, FORBIDDEN, 0L); // should we send the content len? *********
        send(connfd, FORBIDDEN, strlen(FORBIDDEN), 0);
        close(connfd);
        return;
    }
    int fd = open(file, O_RDONLY); // if error, -1 is returned and errno is set with why failure
    if (fd == -1) {
        int error_num = errno;
        // check if file does not exist
        if (error_num == ENOENT) {
            send(connfd, FILENOTFOUND, strlen(FILENOTFOUND), 0);
            close(connfd);
            return;
        // check if file cannot be accessed (forbidden)
        } else if (error_num == EACCES) {
            send(connfd, FORBIDDEN, strlen(FORBIDDEN), 0);
            close(connfd);
            return;
        // any other error should be an internal error      
        } else {
            send(connfd, INTERNALSERVERERROR, strlen(INTERNALSERVERERROR), 0);             
	    close(connfd);
            return;
        }
    }
    // reached here, the file exists and can be opened
    
    // find the size of the file
    struct stat st;
    stat(file, &st);
    // trying new implementation to see if this works with large files
    off_t size = st.st_size; // file size
    //blksize_t blksize = st.st_blksize;
    ssize_t readbytes = 0;

    // reached here, the file exists and can be opened
    //int bufferSize = (int)blksize;
    int bufferSize = 1000;
    uint8_t Buffer[bufferSize + 1];
    
    // send the OK message
    // then after, send the read bytes from the file
    sprintf(msg, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", size);
    send(connfd, msg, strlen(msg), 0);
    
    // send bytes from file to client
    while(1) {
	    readbytes = read(fd, Buffer, bufferSize);
	    if (readbytes <= 0) { // reached eof
		    break;
	    } else { // we have read in bytes from file, send to client
		    send(connfd, Buffer, readbytes, 0); // changed to 0 from MSG_DONTWAIT

	    }
    }
    
    close(connfd);
    close(fd); //close file descriptor
    return;
}

// put receives file contents
void handlePut(int connfd, char* file, long contentlen) {
    // if file does not exist
    //     create it (send created message)
    // if the file does exist
    //     check if we have write permissions
    //     check if you can open it (if not, forbidden msg)
    //     check if you have write permission (if not, forbidden msg)
    //     Store the data (to a buffer) which is the size of content-len
    //     write it to the file
    //     send OK message 
    int wasCreated = 1;
    int file_exists;
    int file_writepermission;
    char msg[150];
    msg[150-1] = '\0';
    // check if file exists
    file_exists = access(file, F_OK); // 0 if access permitted, -1 otherwise
    if (file_exists == 0) { // if file does exist
        // check if file has write permission
        file_writepermission = access(file, W_OK);
        if (file_writepermission == -1) {
            // send forbidden message
            send(connfd, FORBIDDEN, strlen(FORBIDDEN), 0);
            close(connfd);
            return;
        }
	
	wasCreated = 0; // file was not created, already existed
    }


    // open the file or create it
    // O_WRONLY: write only / O_CREAT: optionally creates if file dne / O_TRUNC: erase all contents
    int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0666); // 00700 
    
    if (fd == -1) {
        int error_num = errno;
        // check if file does not exist
        if (error_num == ENOENT) {
            //sprintf(msg, FILENOTFOUND, 0L); // contentlen should be 0 if file not found
            send(connfd, FILENOTFOUND, strlen(FILENOTFOUND), 0);
            close(connfd);
            return;
        // check if file cannot be accessed (forbidden)
        } else if (error_num == EACCES) {
            send(connfd, FORBIDDEN, strlen(FORBIDDEN), 0);
            close(connfd);
            return;
        // any other error should be an internal error      
        } else {
            send(connfd, INTERNALSERVERERROR, strlen(INTERNALSERVERERROR), 0);
            close(connfd);
            return;
        }
    }

    // file was opened successfully for writing
    int BufferSize = 1000;
    uint8_t Buffer[BufferSize + 1]; // create buffer of size contentlen to hold bytes from client
    ssize_t num_bytes_received = 0; // bytes received from client (should be unsigned size)
    ssize_t recv_data = 0; // (should be unsigned size) - size_t is unsigned
    while(num_bytes_received < contentlen) {
        // receive the bytes from the client
	// only receive the exact num of bytes the client is sending
	if ((contentlen-num_bytes_received) >= BufferSize) { // total bytes left to recv > buff size
             recv_data = recv(connfd, Buffer, BufferSize, 0);
	     // check if recv data is less than 0
	     // if so client may have closed connection
	     // don't want to write non-received data, so
	     // send 500 and go back to listening
	     if (recv_data < 0L) { 
	         send(connfd, INTERNALSERVERERROR, strlen(INTERNALSERVERERROR), 0);
		 close(connfd);
		 return;
	     }
             num_bytes_received += recv_data;
	     write(fd, Buffer, recv_data);
	     
	} else if ((contentlen-num_bytes_received) < BufferSize){ // we need to receive a smaller amount of bytes
	     ssize_t num_bytes_left = contentlen-num_bytes_received;
	     recv_data = recv(connfd, Buffer, num_bytes_left, 0);
	     // don't write non-received data:
	     if (recv_data < 0) {
		     send(connfd, INTERNALSERVERERROR, strlen(INTERNALSERVERERROR), 0);
		     close(connfd);
		     return;
	     }
	     num_bytes_received += recv_data;
	     write(fd, Buffer, recv_data);
	}
    }
    
    // send the created message if it was created
    if (wasCreated == 1) {
        //sprintf(msg, CREATED, contentlen);
        send(connfd, CREATED, strlen(CREATED), 0);
        close(connfd);
    } else { // file was not created, send OK message
	long int val = 3;
	sprintf(msg, OK, val);
	send(connfd, msg, strlen(msg), 0);
	close(connfd);
    }
    close(fd); // close file descriptor
    return;
}


void handleHead(int connfd, char* file) {
    // check if the file exists (if not, file not found msg)
    // check if we have read permission for the file (if not, forbidden msg)
    // try to open file
    // if file cannot be opened:
    //     if file does not exist, send file not found msg
    //     if file cannot be accessed, send forbidden msg
    //     any other error, send internal error msg
    // reached here then file does exist and can be read from
    // get the size of the file
    // return an OK message with Content-Length: size of file
   
    int file_exists;
    int file_readpermission;
    char msg[150];
    msg[150-1] = '\0';

    // check if file exists
    file_exists = access(file, F_OK); // 0 if access permitted, -1 otherwise
    if (file_exists == -1) {
        // send a file not found message
        //sprintf(msg, FILENOTFOUND, 0L); // contentlen should be 0 if file not found
        send(connfd, FILENOTFOUND, strlen(FILENOTFOUND), 0);
        close(connfd);
	return;
    }
    // check read permission
    file_readpermission = access(file, R_OK); // 0 if access permitted, -1 otherwise
    if (file_readpermission == -1) { 
        // send forbidden message
        //sprintf(msg, FORBIDDEN, 0L); // should we send the content len? *********
        send(connfd, FORBIDDEN, strlen(FORBIDDEN), 0);
        close(connfd);
        return;	
    }
    int fd = open(file, O_RDONLY); // if error, -1 is returned and errno is set with why failure
    if (fd == -1) {
	    int error_num = errno;
	    // check if file does not exist
            if (error_num == ENOENT) {
                //sprintf(msg, FILENOTFOUND, 0L); // contentlen should be 0 if file not found
                send(connfd, FILENOTFOUND, strlen(FILENOTFOUND), 0);
                close(connfd);
                return;
            // check if file cannot be accessed (forbidden)
	    } else if (error_num == EACCES) {
                send(connfd, FORBIDDEN, strlen(FORBIDDEN), 0);
                close(connfd);
                return;
	    // any other error should be an internal error	
	    } else { 
		//sprintf(msg, INTERNALSERVERERROR, 0L);
		send(connfd, INTERNALSERVERERROR, strlen(INTERNALSERVERERROR), 0);
		close(connfd);
		return;
	    }

    }    
    // reached here, file exists and we can read it
    // send OK message and the file size to the client

    struct stat st;
    stat(file, &st);
    long int size = st.st_size;
    
    sprintf(msg, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", size);
    send(connfd, msg, strlen(msg), 0);
    close(connfd);
    return;

}

void handle_connection(int connfd) {
  // connfd is the connection file descriptor

  // create buffer to hold the string containing the command
  int bufferSize = 4096; // changed from 4096
  char Buffer[bufferSize + 1];
  memset(Buffer, '\0', bufferSize+1);
  //ssize_t bytes = 0;
  //ssize_t bytesSoFar = 0;

  // recv(2) function receives a message from a socket
  // recv(int sockfd, void *buf, size_t len, int flags);
  // buf = pointer to a buffer where the message is to be stored
  // len = length in bytes of the buffer where the message is to be stored
  // if flags=0, no flags
  // read in the bytes passed in 
  ssize_t readbytes =  recv(connfd, Buffer, bufferSize, 0);
  if (readbytes < 0) {
     send(connfd, INTERNALSERVERERROR, strlen(INTERNALSERVERERROR), 0);
  }

  // now Buffer contains the message
  // PUT /filename HTTP/1.1\r\nHost:10.0.0.5:8080\r\nContent-length: 3\r\n\r\nabc
  // GET /filename HTTP/1.1\r\nHost: 127.0.0.1:1234\r\n\r\n
  // HEAD /filename HTTP/1.1\r\nHost: localhost:8080\r\n\r\n

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
  // for debugging purposes
  //fprintf(stderr, "request type: %s\nfilename: %s\nhttpversion:  %s\n", requestType, filename_in, httpversion);


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
  //char *ptr;
  //char msg[150];
  //msg[150-1] = '\0';
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
      
      
      //token1 = sscanf(token, "Content-Length: %ld", &contentlen);
      // not sure if above lineis causing error
      // if 'Content-length:6' then there is no space before the num, which we assume there is always a space
      
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
                    //fprintf(stderr, "In a PUT, Content-Length is: %ld\n", contentlen);
	       } else {
	           // not a number, send bad request
                   send(connfd, BADREQUEST, strlen(BADREQUEST), 0);
                   close(connfd);
                   return;
	       }
           }
      }
      
      memset(buff, '\0', 20); 
      tokenHost = sscanf(token, "Host: %s %s", hostname, buff);
      if(tokenHost == 2){ // if sscanf saved 2 items, then header is bad header
         send(connfd, BADREQUEST, strlen(BADREQUEST), 0);
	 close(connfd);
	 return;
      }

      // sscanf returns the number of fields that were saved into variables
      
      // fprintf(stderr, "token1 is: %ld\n", token1);
      if (token1 != 1 && tokenHost != 1) {
	      // then we have not grabbed content-len or Host  
	      // and have reached a different header
	      // must check that all other headers are valid header:body format

	      token2 = sscanf(token, "%s %s", header, body);
              if (token2 == 2) { // correct header:body format, check for ':'
		     //fprintf(stderr, "header: %s body: %s\n", header, body);
		     if (header[strlen(header)-1] != ':') { // if invalid request
			     //fprintf(stderr, "Invalid header; header: %s, body: %s\n", header, body);
			     //sprintf(msg, BADREQUEST, 0L);
			     send(connfd, BADREQUEST, strlen(BADREQUEST), 0);
			     close(connfd);
			     return;
		     } else {
			     // do nothing, valid header: body
	             }
	      
              } else { // token1 != 1 (not contentlen) and token2 != 2 (invalid header: body pair)
		   // we have reached inalid header: body 
		   //sprintf(msg, BADREQUEST, 0L);
		    send(connfd, BADREQUEST, strlen(BADREQUEST), 0);
		    close(connfd);
		    return;
	      }
        } 

      // update token to move to next one
      token = strtok(NULL, "\r\n");
    }
  
  // check httpversion is correct 
  if(strcmp(httpversion, "HTTP/1.1") != 0) {
	  //sprintf(msg, BADREQUEST, 0L); // contentlen = 0 here
	  send(connfd, BADREQUEST, strlen(BADREQUEST), 0);
          close(connfd);
          return;
  }

  // save filename = filename_in+1 to filename_in (exclude the first '/')
  memmove(filename, filename_in+1, strlen(filename_in));


  // check filename is ok
  int len = (int)strlen(filename);

  if (len > 19) { // if length of incoming file name is > 19 chars
	send(connfd, BADREQUEST, strlen(BADREQUEST), 0); 
        close(connfd); // close connection
	return; // return back to listening for other connections
  }	


  // filename must only contain alphanumeric, . and _ only (exclude the first '/')
  len = (int)strlen(filename);
  for(int k=1; k < len; k++) {
          if (!(isalpha(filename[k]) || isdigit(filename[k]) || filename[k] == '_'|| filename[k] == '.')) {
		send(connfd, BADREQUEST, strlen(BADREQUEST), 0);
                close(connfd);
                return;
          }
  }

  
  // for debugging purposes
  // fprintf(stderr, "The file name is now: %s\n", filename);

  // request is properly formatted but the type is incorrect
  // so we must check here that request type is valid
  // check request_type is either GET, PUT, or HEAD, otherwise close connection
  int isGet = strcmp(requestType, "GET"); // 0 if true
  int isPut = strcmp(requestType, "PUT");
  int isHead = strcmp(requestType, "HEAD");

  if (isGet == 0) {
	  // do something for get
	  handleGet(connfd, filename);
  } else if (isPut == 0) {
	  // do something for put
	  handlePut(connfd, filename, contentlen);
  } else if (isHead == 0) {
	 // do something for head
	 handleHead(connfd, filename);
  } else { // is not get, put or head
	//sprintf(msg, NOTIMPLEMENTED, 0L); // content len = 0 
	send(connfd, NOTIMPLEMENTED, strlen(NOTIMPLEMENTED), 0);
  }  

  // when done, close socket (we close socket inside of handle*() fcns
  //close(connfd);
  return;
}

int main(int argc, char *argv[]) {
  int listenfd;
  uint16_t port;

  if (argc != 2) {
    errx(EXIT_FAILURE, "wrong arguments: %s port_num", argv[0]);
  }
  port = strtouint16(argv[1]);
  if (port == 0) {
    errx(EXIT_FAILURE, "invalid port number: %s", argv[1]);
  }
  listenfd = create_listen_socket(port); // creates listening socket to listen for connections

  while(1) {

    // accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
    // sockfd: sock file descriptor 
    // *addr: the client's address  
    // *addrlen: the client address's length
    // our client address is null because as us (the caller) we are not 
    // interested in the client's address (we are a local client) 	  
    int connfd = accept(listenfd, NULL, NULL); 
    
    if (connfd < 0) {
      warn("accept error");
      continue;
    }
    handle_connection(connfd);
  }
  return EXIT_SUCCESS;
}

