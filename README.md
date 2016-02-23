# ThreadPool
------------------
- Author - Amir Lavi.

- Description - 
	* Threadpool to any kind of work assigning such as; TCP server
	* Threads are waiting for a new job assignment
	* Once a new job arrived in queue, a thread passes the mutex lock barrier and execute it
	* Using the -lpthread when compiling causes the pthread library to be linked, without pre-defined macros
	* Enjoy
