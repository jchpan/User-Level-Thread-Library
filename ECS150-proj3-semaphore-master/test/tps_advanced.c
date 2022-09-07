#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <tps.h>
#include <sem.h>


static char msg1[TPS_SIZE] = "Hello world!\n";
static char msg2[TPS_SIZE] = "hello world!\n";

static sem_t sem1, sem2;
static pthread_t tid[4];
void *latest_mmap_addr;

// global variable to make address returned by mmap accessible
void *__real_mmap(void *addr, size_t len, int prot, int flags, int fildes, off_t off);

void *__wrap_mmap(void *addr, size_t len, int prot, int flags, int fildes, off_t off)
{
	latest_mmap_addr = __real_mmap(addr, len, prot, flags, fildes, off);
	return latest_mmap_addr;
}

// tester for phase 2.2: attempting protected memory
// if it works properly, the program wil print "TPS protection error!" and raise a segmentation fault
void *thread5(void* arg)
{
	/* Create TPS */
	tps_create();

	/* Get TPS page address as allocated via mmap() */
	char *tps_addr = latest_mmap_addr;

	/* Cause an intentional TPS protection error */
	printf("Prepare to have seg fault\n");
	tps_addr[0] = '\0';

	/* If no seg fault, we free the current tps */
	tps_destroy();

	return NULL;
}

void *thread4(void* arg)
{
	char *buffer = malloc(TPS_SIZE);

	tps_create();

	tps_write(0, TPS_SIZE, msg1);

	/* Read from TPS and make sure it contains the message */
	memset(buffer, 0, TPS_SIZE);
	tps_read(0, TPS_SIZE, buffer);
	assert(!memcmp(msg1, buffer, TPS_SIZE));
	printf("thread4: read OK!\n");

	/* Transfer CPU to thread 3 and get blocked */
	sem_up(sem1);
	sem_down(sem2);

	/* When we're back, read TPS and make sure it sill contains the original */
	memset(buffer, 0, TPS_SIZE);
	tps_read(0, TPS_SIZE, buffer);
	assert(!memcmp(msg1, buffer, TPS_SIZE));
	printf("thread4: read OK!\n");

	/* Transfer CPU to thread 3 and get blocked */
	sem_up(sem1);
	sem_down(sem2);

	/* When we are back, destroy TPS and quit */
	tps_destroy();
	free(buffer);
	return NULL;
}

void *thread3(void* arg)
{
	pthread_t tid;
	char *buffer = malloc(TPS_SIZE);

	/* Create thread 4 and get blocked */
	pthread_create(&tid, NULL, thread4, NULL);
	sem_down(sem1);

	/* When we're back, clone thread 4's TPS */
	tps_clone(tid);

	/* Read the TPS and make sure it contains the original */
	memset(buffer, 0, TPS_SIZE);
	tps_read(0, TPS_SIZE, buffer);
	assert(!memcmp(msg1, buffer, TPS_SIZE));
	printf("thread3: read OK!\n");

	/* Modify TPS to cause a copy on write */
	buffer[0] = 'h';
	tps_write(0, 1, buffer);

	/* Transfer CPU to thread 4 and get blocked */
	sem_up(sem2);
	sem_down(sem1);

	/* When we're back, make sure our modification is still there */
	memset(buffer, 0, TPS_SIZE);
	tps_read(0, TPS_SIZE, buffer);
	assert(!strcmp(msg2, buffer));
	printf("thread3: read OK!\n");

	/* Transfer CPU to thread 4 */
	sem_up(sem2);

	/* Wait for thread 4 to die, and quit */
	pthread_join(tid, NULL);
	tps_destroy();
	free(buffer);
	return NULL;
}

void *thread2(void* arg)
{
	/* Test case to check all exception cases */
	char *buffer = malloc(TPS_SIZE);

	printf("Test cases for exception cases");

	/* Test for destroy exception case */
	printf("Test destroy if current thread doesn't have a TPS \n");
	assert(tps_destroy() == -1);
	printf("Good! Test destory exception case successfully\n");

	/* Test for read exception case */
	memset(buffer, 0, TPS_SIZE);
	printf("Test read if current thread doesn't have a TPS\n");
	assert(tps_read(0, TPS_SIZE, buffer) == -1);
	printf("Good! Test read exception case successfully\n");

	/* Test for write exception case */
	printf("Test write if current thread doesn't have a TPS\n");
	assert(tps_write(0, TPS_SIZE, buffer) == -1);
	printf("Good! Test write exception case successfully\n");

	/* Test for clone exception case */
	printf("Test clone if target tid doesn't have a TPS\n");
	/* tid[3] is a thread that do not have TPS yet */
	assert(tps_clone(tid[3]) == -1);
	printf("Good! Test clone exception case successfully\n");

	/* Create tps for current thread */
	/* So the current thread will have a TPS */
	tps_create();

	/* Test for create exception case */
	printf("Test create if current thread already has a TPS\n");
	assert(tps_create() == -1);
	printf("Good! Test create exception case successfully\n");

	/* Test cases reading & writing operation out of bound */
	printf("Test read if the reading operation is out of bound\n");
	assert(tps_read(0, TPS_SIZE + 1, buffer) == -1);
	printf("Good! Test reading out-of-bound case successfully\n");

	printf("Test read if the writing operation is out of bound\n");
	assert(tps_write(0, TPS_SIZE + 1, buffer) == -1);
	printf("Good! Test writing out-of-bound case successfully\n");

	/* Test cases reading & writing operation when buffer is NULL */
	printf("Test read if the buffer is NULL\n");
	assert(tps_read(0, TPS_SIZE, NULL) == -1);
	printf("Good! Test reading NULL buffer successfully\n");

	printf("Test read if buffer is NULL\n");
	assert(tps_write(0, TPS_SIZE, NULL) == -1);
	printf("Good! Test writing NULL buffer successfully\n");

	/* Test clone when the current thread has TPS */
	printf("Test clone if current tid have a TPS\n");
	assert(tps_clone(tid[0]) == -1);
	printf("Good! Test clone exception case successfully\n");
	printf("Done! Passed all exception cases\n");

	tps_destroy();
	free(buffer);
	return NULL;
}

void *thread1(void* arg)
{
	/* simple test case checking when offsets is not equal to 0 */
	char *buffer = malloc(TPS_SIZE);

	tps_create();

	tps_write(0, TPS_SIZE, msg1);

	/* Read the TPS and make sure it contains the original */
	memset(buffer, 0, TPS_SIZE);
	tps_read(0, TPS_SIZE, buffer);
	assert(!memcmp(msg1, buffer, TPS_SIZE));
	printf("Simple read successfully\n");

	/* Read the TPS and set offsets to be 10 bytes */
	size_t offsets = 10;

	memset(buffer+offsets, 0, TPS_SIZE-offsets);
	tps_write(offsets, TPS_SIZE-offsets, msg2);
	tps_read(offsets, TPS_SIZE-offsets, buffer);
	assert(!memcmp(msg2, buffer, TPS_SIZE-offsets));
	printf("Simple read with 10 bytes offsets successfully!\n");

	tps_destroy();
	free(buffer);
	return NULL;
}

int main(int argc, char **argv)
{
	/* Create two semaphores for thread synchro */
	sem1 = sem_create(0);
	sem2 = sem_create(0);

	/* Init TPS API */
	tps_init(1);

	/* Create thread 1 (simple test case) and join */
	pthread_create(&tid[0], NULL, thread1, NULL);
	pthread_join(tid[0], NULL);

	/* Create thread 2 (exception tests) and join */
	pthread_create(&tid[1], NULL, thread2, NULL);
	pthread_join(tid[1], NULL);

	/* Create thread 3 (copy and write) and wait */
	pthread_create(&tid[2], NULL, thread3, NULL);
	pthread_join(tid[2], NULL);

	/* Create thread 5 (protection test case) and join */
	pthread_create(&tid[3], NULL, thread5, NULL);
	pthread_join(tid[3], NULL);

	/* Destroy resources and quit */
	sem_destroy(sem1);
	sem_destroy(sem2);
	return 0;
}
