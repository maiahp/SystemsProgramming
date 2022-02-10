Maiah Pardo


httpserver.c is an http server that will perform GET, PUT, and HEAD operations for a client. To run this program, one can follow the following command line steps:

make
./httpserver 1234 &

now the server will run in the background and the user is free to use curl to connect to the server.

For GET: (downloads a file named download_file using the contents of 'file' in server's current directory into the client's directory)
    curl http://localhost:1234/file -v -o download_file   
    
For PUT: (puts a file named 'newfile' from the client into the server's current directory)
    curl -T file http://localhost:1234/newfile -v 
    
For HEAD: (sends the client the size of the file 'file')
    curl -I http://localhost:1234/file -v

