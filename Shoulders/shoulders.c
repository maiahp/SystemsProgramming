#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdbool.h>
#include <err.h>

// method which echo's std in to std out for a specified number of lines
void echo_input(int max_lines, char* Buffer, int bufferSize, int fd){
  int currline = 0;
  char buff[1];
  memset(buff,'\0',1); // sets each elem of buffer to \0
  while (currline < max_lines) {
      ssize_t readbytes = read (fd, Buffer, bufferSize);    // 0 if eof, -1 if error
      if (readbytes < 0) {
          warn("an error has occurred when reading in\n");
	  break;
      }
      if (readbytes == 0){ // eof was reached
          break;
      }
      for (int i=0; i<(int)readbytes; i++) {//loop through entire buffer to check for newlines
          buff[0] = Buffer[i]; // place next character into buff
          if(buff[0] == '\n')currline++; // if it is a new line
          write (STDOUT_FILENO, buff, 1); // write character 
          if(currline == max_lines){ // if we reach max num lines, stop
               break;
          }
          memset(buff,'\0',1); // resets the memory to null terminator
	  			// don't really need this
      }
      if(currline == max_lines)break; // if we reach max num lines, stop
  }
}

void io(int max_lines, char* Buffer, int bufferSize, int fd){
  int currline = 0;
  int buffsize = 1024;
  char buff[buffsize];
  memset(buff,'\0',buffsize);
  while (currline < max_lines) {
      ssize_t readbytes = read (fd, Buffer, bufferSize);    // 0 if eof, -1 if error
      if (readbytes < 0) {
          warn("an error has occurred when reading in\n");
	  break;
      }
      if (readbytes == 0){ // eof was reached
          break;
      }

      // notes:
      // don't think about \0 at the end of a character string here
      // that is read in or written from
      // read() and write() don't care about null terminators
      // only File* cares about it (lines read in from fgets do not contain \0, we have to
      // put them in.
      // we would handle reading in by (if we want 80 bytes or characters) our buffer is size 81
      //    and the last slot we make the \0
      // all File* write functions require that c-strings be null terminated
      // if we had more in our buffer than what we wanted to print, we could just
      // place a null terminator in a slot and it would stop reading from there

      int j = 0; // place holder for where we are in buff
      int end = (int)readbytes - 1; // the total num of bytes read in
      for (int i=0; i<(int)readbytes; i++) { // loop through buffer to check for newlines
	      // use i<readbytes since that is the amount of chars currently in buffer
           buff[j] = Buffer[i]; // first slot of buff is first char on Buffer (cont adding to buff)
	   j++;
	   if (buff[j-1] == '\n') { // if the prev character is a newline
		currline++; // found end of line
	        write(STDOUT_FILENO, buff, j); // write the line
	        memset(buff, '\0', j); // set buff to \0
	        j=0; // we are re-reading in another line into buff (reset)
	        if (currline == max_lines) { // finished writing
		     break;
	        }
           } else if (i == end) { // if we reached the end of the read Buffer
                write(STDOUT_FILENO, buff, j); // then we write (read in more to Buffer later)
                memset(buff, '\0', j); // reset buff
                j=0; // we are reading another line into buff (reset)
           }
           if (j == buffsize) { // buff[15] is the last spot in buff, then we will run out of room at 16
	        write(STDOUT_FILENO, buff, j); // write when j=16
                memset(buff, '\0', j); // reset buff
                j=0; // reset where we are in buff
           }
      }
      if (currline == max_lines)break; // if we reached max lines, break
   }
}

int main (int argc, const char *argv[]) {
  int bufferSize = 8192;
  char Buffer[bufferSize + 1];
  Buffer[bufferSize] = '\0';

  int default_num_lines = 10;    // default number of lines to show
  int num_lines = default_num_lines;

  if (argc == 1) {
      // no args given
      // error, must have atleast one argument for num lines
      warn("requires an argument");
      exit(0);
  } else {
      int nofiles = 1;        // bool to check if we do have files
      // determine if first argument is a number
      // quit if it is an invalid number
      int isNumber;
      char *ptr;
      // strtol will set ptr to the first part of string that was unable to convert
      // to a number, or \0 if entire string converted to a num
      long num = strtol (argv[1], &ptr, 0);
      if (ptr == argv[1]) {
          // if entire argv[1] is made up of chars
          // then could not convert to num
          isNumber = 0;
      } else if (*ptr == '\0') { // entire string converted to num
          // set num_lines to new value
          isNumber = 1; 
          num_lines = num;
      } else {          // part of the string was unconverted, so we do not have a number
          // consider as a file name
          isNumber = 0;
      }

      if (isNumber == 1) {  // arg 1 is a number
          if (num < 0) {            // check if it is invalid
            fprintf (stderr, "%s: invalid number of lines: '%s'\n", argv[0], argv[1]);
            exit (0);        // are we allowed to use exit()?
          }
      } else { // first arg is not a number
            fprintf(stderr, "%s: invalid number of lines: '%s'\n", argv[0], argv[1]);
            exit(0);
      } 
      
      int isNotDash;
      for (int i=2; i < argc; i++) { // for each argument in rest of args
          // check for '-'
          nofiles = 0;        // reached here, there are files
          isNotDash = strcmp (argv[i], "-");    // 0 if equal
          if (isNotDash != 0) {            // if it is not a dash
              int file = open (argv[i], O_RDONLY); // file = the file descriptor   
              if (file == -1) {   // if file cannot be opened it is = to -1
		  warn("cannot open '%s' for reading", argv[i]);
                  fflush(STDIN_FILENO); // must flushing stdin
              }else{
                  io(num_lines, Buffer, bufferSize, file);
              }
              close(file);
          } else {            // is a dash
              echo_input(num_lines, Buffer, bufferSize, STDIN_FILENO);
          }
      }

      // if we had no files, just a number for num lines,
      // then we must still execute echo_input()
      if (nofiles == 1) {
          echo_input(num_lines, Buffer, bufferSize, STDIN_FILENO);
      }
   }
}

// note:
      // posix names:
      //   STDIN_FILENO = 0 (for stdin)
      //   STDOUT_FILENO = 1 (for stdout)
      //   STDERR_FILENO = 2 (for stderr)
