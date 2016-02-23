#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include "threadpool.h"

int dispatch_function(void *arg);

int main(int argc, char *argv[])
{
	threadpool *t = create_threadpool(10);
	int i, k;
	for(i = 1; i <= 20; i++)
	{
		k = i;
		dispatch(t, dispatch_function, (void*)k);
	}
	destroy_threadpool(t);
	printf("DONE\n");
	return 1;
}

int dispatch_function(void *arg)
{
	sleep(1);
	printf("-- test number: %d --\n", (int)(arg));
	return 0;
}
