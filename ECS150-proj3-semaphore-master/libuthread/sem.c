#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <assert.h>

#include "queue.h"
#include "sem.h"
#include "thread.h"

struct semaphore {
	queue_t blocked_queue;
	size_t count;
};

// create a semaphore with a given @count
// return the pointer to the semaphore
// return NULL if failed to create
sem_t sem_create(size_t count)
{
	struct semaphore *sem = (struct semaphore*)
			malloc(sizeof(struct semaphore));

	if (!sem) {
		return NULL;
	}

	sem->blocked_queue = queue_create();
	sem->count = count;

	return sem;
}

// destroy the specified @sem
// return -1 if @sem is NULL or nonempty
// return 0 if succeeded
int sem_destroy(sem_t sem)
{
	if (!sem) {
		return -1;
	}
	else if (queue_destroy(sem->blocked_queue) == -1) {
		free(sem);
		return -1;
	}
	free(sem);
	return 0;
}

// take a resource from semaphore @sem
// taking an unavailable semaphore will cause the caller thread to be blocked, until the semaphore becomes available
// return -1 if @sem is NULL
// return 0 if the action is successful
int sem_down(sem_t sem)
{
	if (!sem) {
		return -1;
	}

	enter_critical_section();

	pthread_t tid = pthread_self();
	if (sem->count == 0) {
		queue_enqueue(sem->blocked_queue, (void*)tid);
		thread_block();
	}
	--(sem->count);

	exit_critical_section();

	return 0;
}

// release a resource from semaphore @sem
// if the waiting list associated to @sem is not empty, releasing a resource also causes the first (oldest) thread in the waiting list to be unblocked
// return -1 if @sem is NULL
// return 0 if the action is successful
int sem_up(sem_t sem)
{
	if (!sem) {
		return -1;
	}

	enter_critical_section();

	++(sem->count);

	void* ptr = NULL;
	int dequeue_ret = queue_dequeue(sem->blocked_queue, (void**)&ptr);
	if (dequeue_ret == 0) {
		pthread_t tid = (pthread_t)ptr;
		thread_unblock(tid);
	}

	exit_critical_section();
	return 0;
}

// inspect internal state of @sem, and propagate the result to @sval
// if @sem's count > 0, propagate the internal count to @sval
// if @sem's count == 0, propagate the negative of the number of blocked threads to @sval
// return -1 if @sem or @sval is NULL
// return 0 if succeeded
int sem_getvalue(sem_t sem, int *sval)
{
	if ((!sem) && (!sval)) {
		return -1;
	}

	int size_blk = queue_length(sem->blocked_queue);
	if (sem->count > 0) {
		*sval = sem->count;
	} else if (sem->count == 0) {
		*sval = -size_blk;
	}

	return 0;
}
