Maiah Pardo



This assignment is a proxy server with loadbalancing and caching.

To run:
1) Start instances of httpservers with logging, using the provided binary: ./httpserver port -l logname -N numThreads
   Note: the -l and -N flags are optional, but logging is necessary for the httpproxy healthchecks

2) Start instance of httpproxy: ./httpproxy (proxy port) (httpserver ports) -N numThreads -R numReq -s cachesize -m filesize -u
   Note: all flags and httpserver ports are optional, all arguments may be entered in any order (however the first port is the proxy port)
         -N number of threads, -R number of requests that may occur before a healthcheck probe on all servers, -s number of entries that can be stored in the cache, -m the max file size that can be stored in the cache, if -s or -m is not present, no caching is done. -u modifies the cache to remove entries based on removing Least Recently Accessed entries when full, if no -u present, the cache removes entries based on LIFO

3) Now you may send GET requests to the proxy using curl. 
