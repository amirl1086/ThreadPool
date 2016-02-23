/* ======= Written by: Amir Lavi, ====== */
/* ============ threadpool.c =========== */
/* ===================================== */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "threadpool.h"

//macros
#define ACCEPT 0
#define DONT_ACCEPT 1

//the threads constructor
threadpool* create_threadpool(int num_threads_in_pool)
{
	//checking the number of threads requested
	if (num_threads_in_pool > MAXT_IN_POOL || num_threads_in_pool < 1)
	{
		printf("Illegal number of threads requested\n");
		return NULL;
	}
	
	//initialize the object
	threadpool *my_threadpool = (threadpool*)calloc(1, sizeof(threadpool));
	if (!my_threadpool)
	{
		perror("Threads pool object memory allocation failed\n");
		return NULL;
	}
	
	//set the field num_threads to the requested number of threads by the user
	my_threadpool->num_threads = num_threads_in_pool;
	
	//initializing the array for the threads, size of num_threads
	my_threadpool->threads = (pthread_t*)calloc(my_threadpool->num_threads, sizeof(pthread_t));
	if (!my_threadpool->threads)
	{
		perror("Threads array memory allocation failed\n");
		free(my_threadpool);
		return NULL;
	}
	
	//initializing the list (empty at first)
	my_threadpool->qhead = NULL;
	my_threadpool->qtail = NULL;
	my_threadpool->qsize = 0;
	
	//initializing the flags (ACCEPT = 0)
	my_threadpool->shutdown = ACCEPT;
      my_threadpool->dont_accept = ACCEPT;
      
	// initializing mutex and condition variables, all returns zero if successful
	if (pthread_mutex_init(&(my_threadpool->qlock), NULL))
	{
		perror("Object Mutex lock initializing failed\n");
   		free(my_threadpool->threads);
   		free(my_threadpool);
   		return NULL;
	}
	if (pthread_cond_init(&(my_threadpool->q_empty), NULL))
	{
		perror("Object Mutex lock initializing failed\n");
   		pthread_mutex_destroy(&(my_threadpool->qlock));
   		free(my_threadpool->threads);
   		free(my_threadpool);
   		return NULL;
	}
	if (pthread_cond_init(&(my_threadpool->q_not_empty), NULL))
	{
   		perror("Object conditions initializing error\n");
   		pthread_mutex_destroy(&(my_threadpool->qlock));
		pthread_cond_destroy(&(my_threadpool->q_empty));
		free(my_threadpool->threads);
   		free(my_threadpool);
   		return NULL;
   	}
	
	//initializing a pool of threads using
	int i;
	for (i = 0; i < my_threadpool->num_threads; i++)
	{
		/* pthread_create will make the threads to start running the function 
		"do_work", inside that function we will catch all the treads in an infinite 
		loop and make them wait for jobs. pthread_create returns zero if successful */
		if (pthread_create(&(my_threadpool->threads[i]), NULL, do_work, my_threadpool))
		{
			perror("Thread initializing failed\n");
			pthread_mutex_destroy(&(my_threadpool->qlock));
			pthread_cond_destroy(&(my_threadpool->q_empty));
			pthread_cond_destroy(&(my_threadpool->q_not_empty));
			free(my_threadpool->threads);
   			free(my_threadpool);
			return NULL;
		}
	}
	
	return my_threadpool;
}

//the add work function
void dispatch(threadpool* from_me, dispatch_fn dispatch_to_here, void *arg)
{
	//critical section - checking the object
	pthread_mutex_lock(&(from_me->qlock));
	//destructor started and raised the flag "dont accept"
	if(from_me->dont_accept == DONT_ACCEPT)
		return;
	//end of critical section, give back the lock
	pthread_mutex_unlock(&(from_me->qlock));
	
	/* if the parameter "dispatch_to_here" is NULL, there's a need to exit the program.
	That parameter is a pointer to function that was sent from the main (its a constant).
	if it was NULL, it will stay NULL, there's no reason letting the main thread continue
	running. (the threadpool was already allocated, start destruction procedure) */
	if (!dispatch_to_here)
	{
		printf("Dispatch function not assigned correctly\n");
		destroy_threadpool(from_me);
	}
	
	/* if the parameter arg is NULL, the main can try again. for example for a server
	it could be a socket that failed setting up and the next one might succeed */
	if (!arg)
	{
		printf("Argument not assigned correctly\n");
		return;
	}
	
	//initializing the new work
	work_t *new_work = (work_t*)calloc(1, sizeof(work_t));
	if (!new_work) //if allocating memory was unsuccessful
	{
		perror("Allocating memory for the request failed\n");
		return;
	}
	
	//init the work fields
	new_work->routine = dispatch_to_here;
	new_work->arg = arg;
	new_work->next = NULL;
	
	//critical section - addind a job to the queue
	pthread_mutex_lock(&(from_me->qlock));
	
	//check again if destructor started flag is up
	if(from_me->dont_accept == DONT_ACCEPT)
	{	
		free(new_work);
		return;
	}
	//add the job to the queue
	if (!from_me->qsize)//the list is empty
	{
		from_me->qhead = new_work;
		from_me->qtail = new_work;
	}
	else//add to the queue at the end of the queue (FIFO) 
	{
		from_me->qtail->next = new_work;
		from_me->qtail = from_me->qtail->next;
	}
	from_me->qsize++; //increase the size of the queue
	//signal the threads that the queue is not empty, one one them will take it
	pthread_cond_signal(&(from_me->q_not_empty));
	//end of critical section, give back the lock
	pthread_mutex_unlock(&(from_me->qlock));
}

//the threads function 
void* do_work(void* p)
{	
	//casting before going to work
	threadpool *thread_pool = (threadpool*)p;
	while (1)
	{
		//critical section - checking the object
		pthread_mutex_lock(&(thread_pool->qlock));
		
		//check if destructor has started
		if(thread_pool->shutdown) 
		{
			//return the lock before ending
			pthread_mutex_unlock(&(thread_pool->qlock));
			return NULL;
		}
		
		//while the queue is empty
		while (!thread_pool->qsize)
		{		
			/*all threads will wait for the condition to flip, and when it happens 
			(the mutex is unlocked) only a single thread passes and lock the mutex */
			pthread_cond_wait(&(thread_pool->q_not_empty),&(thread_pool->qlock));
			if (thread_pool->shutdown) //check again if destructor has started
			{
				//return the lock before ending
				pthread_mutex_unlock(&(thread_pool->qlock));
				return NULL;
			}
		}	
		//if a thread reached here he's about to take a job
		thread_pool->qsize--; //decrease the queue size
		work_t *temp = thread_pool->qhead; //pull the first job (FIFO)
		if (!thread_pool->qsize) //if the queue is empty, initialize it again
		{
			thread_pool->qhead = NULL;
			thread_pool->qtail = NULL;
			//queue is empty, check again if the destructor wants to start
			if (thread_pool->dont_accept) //signal the distructor
				pthread_cond_signal(&(thread_pool->q_empty));
		}
		else //advance the head to the next node
			thread_pool->qhead = thread_pool->qhead->next;
		//end of critival section, give the lock back
		pthread_mutex_unlock(&(thread_pool->qlock));
		
		//doing the job 
		if (temp->routine(temp->arg) < 0) //if failed, returns -1
			printf("Processing the request failed\n");
		free(temp);
	}
	return NULL;
}

//destroy the thread pool
void destroy_threadpool(threadpool* destroyme)
{
	//critical section - locking the mutex
	pthread_mutex_lock(&(destroyme->qlock));
	//raise don't accept new jobs flag
	destroyme->dont_accept = DONT_ACCEPT;
	//wait for the threads to finish all the jobs
	while (destroyme->qsize)
		pthread_cond_wait(&(destroyme->q_empty),&(destroyme->qlock));
	//raise shutdown has began flag
	destroyme->shutdown = DONT_ACCEPT;
	//all the threads waiting in do_work wake up and see the shut downflag
	pthread_cond_broadcast(&(destroyme->q_not_empty)); //tell them to continue
	
	//end of critical section, give the lock back
	pthread_mutex_unlock(&(destroyme->qlock));
	
	/* this join loop will make the main thread wait for all the
	threads that are still working, if there are any at all */
	int i; void *status;
	for (i = 0; i < destroyme->num_threads; i++)
		pthread_join(destroyme->threads[i], &status);
	
	//free the mutex, conditions and the object itself
	pthread_mutex_destroy(&(destroyme->qlock));
	pthread_cond_destroy(&(destroyme->q_empty));
	pthread_cond_destroy(&(destroyme->q_not_empty));
	free(destroyme->threads);
	free(destroyme);
}


