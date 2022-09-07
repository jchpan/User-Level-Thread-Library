#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "queue.h"
#include "thread.h"
#include "tps.h"

/* data structures */

typedef struct TPS_page {
	void* mapped_area;
	unsigned int ref_counter; // the number of TPSes sharing this page; shall be >= 1
} TPS_page;

typedef struct TPS {
	TPS_page* page;
	pthread_t tid;
} TPS;

/* internal "global" variables */

static queue_t tps_queue = NULL; // nodes are TPS structs
static int tps_initialized = false;

/* internal functions */

static void* init_mapped_area_helper(void)
{
	// allocate memory, and then check if allocation is successful
	void* mapped_area = mmap(/* addr = */NULL, /* length = */TPS_SIZE,
		/* protection = */PROT_NONE,
		/* flag = */MAP_PRIVATE | MAP_ANONYMOUS,
		/* fd = */-1, // ignored when using MAP_ANONYMOUS as flag; but some inplementations requires fd to be -1 (see manpage mmap(2))
		/* offset = */0);
	if (mapped_area == MAP_FAILED) {
		return MAP_FAILED;
	}
	return mapped_area;
}

// HELPER FUNCTION: initialize new TPS with a specified @tid, a @tps_page_to_use, and a @mapped_area
// if @tps_page_to_use is NULL, allocate a new page (used when creating a TPS or performing copy-on-write)
// if @tps_page_to_use is not NULL, use it for the new TPS (used when cloning a TPS); in this case @mapped_area is NOT used
// return the pointer to TPS
// return NULL if failed to create TPS or TPS page
static TPS* init_new_tps_helper(pthread_t tid, TPS_page* tps_page_to_use, void* mapped_area)
{
	TPS* new_tps = (TPS*)malloc(sizeof(TPS));
	if (!new_tps) {
		return NULL;
	}

	new_tps->tid = tid;

	// initialize the page of the new TPS
	// allocate a new page when creating a new TPS / performing copy-on-write
	if (!tps_page_to_use) {
		new_tps->page = (TPS_page*)malloc(sizeof(TPS_page));
		if (!(new_tps->page)) {
			free(new_tps);
			return NULL;
		}

		new_tps->page->mapped_area = mapped_area;
		new_tps->page->ref_counter = 1;
	}
	// use a given page when cloning a TPS
	else {
		new_tps->page = tps_page_to_use;
		++(new_tps->page->ref_counter);
		assert(new_tps->page->ref_counter > 0);
	}

	return new_tps;
}

// HELPER FUNCTION: create the @queue if it was NULL; otherwise do nothing
// return the pointer to the queue created
static queue_t check_create_queue(queue_t* queue) {
	if (!(*queue)) {
		*queue = queue_create();
	}
	return *queue;
}

// HELPER FUNCTION: destroy the @queue if it was empty; otherwise do nothing
// return -1 if @queue is NULL or non-empty
// return 0 if successfully destroyed
static int check_destroy_queue(queue_t* queue) {
	if ((!queue) || (queue_length(*queue) > 0)) {
		return -1;
	}
	queue_destroy(*queue);
	*queue = NULL;
	return 0;
}

// the callback function for checking tid, used for the function queue_iterate() in the second argument
// to learn about the function queue_iterate(), see "queue.h"
// return -1 if @tps_node is NULL
// return 0 if @tid_to_check does not match
// return 1 if @tid_to_check matches
static int check_tid_callback(void* tps_node, void* tid_to_check)
{
	if (!tps_node) {
		return -1;
	}

	pthread_t tps_node_tid = ((TPS*)tps_node)->tid;
	if (tps_node_tid == (pthread_t)tid_to_check) {
		return 1;
	}
	return 0;
}

// HELPER FUNCTION: find @tid in @queue, and propagate the pointer of the corresponding TPS into @result_tps
// return -1 if @queue or @result_tps is NULL
// return 0 if not found
// return 1 if found
static int iterate_find_tid(queue_t queue, pthread_t tid, TPS** result_tps)
{
	if ((!queue) || (!result_tps)) {
		return -1;
	}

	*result_tps = NULL;
	queue_iterate(queue, check_tid_callback, (void*)tid, (void**)result_tps);
	// if the @tid is found in the @queue, @result_tps will be set to the corresponding tps that contains the given @tid
	if (*result_tps) {
		return 1;
	}
	return 0;
}

// the callback function for checking tid, used for the function queue_iterate() in the second argument
// to learn about the function queue_iterate(), see "queue.h"
// return -1 if @tps_node is NULL, or its page pointer is NULL
// return 0 if @tid_to_check does not match
// return 1 if @tid_to_check matches
static int check_mapped_area_callback(void* tps_node, void* mapped_area_arg)
{
	if ((!tps_node) || (!(((TPS*)tps_node)->page))) {
		return -1;
	}

	void* tps_node_mapped_area = ((TPS*)tps_node)->page->mapped_area;
	if (tps_node_mapped_area == mapped_area_arg) {
		return 1;
	};
	return 0;
}

// HELPER FUNCTION: iterate through all the TPS areas in @queue, and find if the specified in @mapped_area_arg matches one of them
// return -1 if @queue or @result_tps is NULL
// return 0 if not found
// return 1 if found
static int iterate_find_mapped_area(queue_t queue, void* mapped_area_arg, TPS** result_tps)
{
	if ((!queue) || (!result_tps)) {
		return -1;
	}

	*result_tps = NULL;
	queue_iterate(queue, check_mapped_area_callback, mapped_area_arg, (void**)result_tps);
	// if the @mapped_area_arg is found in the @queue, @result_tps will be set to the corresponding tps that contains the given @mapped_area_arg
	if (*result_tps) {
		return 1;
	}
	return 0;
}

// signal handler function
// snippet provided by professor Porquet
static void segv_handler(int sig, siginfo_t *si, void *context)
{
	// get the address corresponding to the beginning of the page where the fault occurred
	void *p_fault = (void*)((uintptr_t)si->si_addr & ~(TPS_SIZE - 1));

	// iterate through all the TPS areas and find if p_fault matches one of them
	TPS* faulty_tps = NULL;
	iterate_find_mapped_area(tps_queue, p_fault, &faulty_tps);
	// if there is a match
	if (faulty_tps) {
		fprintf(stderr, "TPS protection error!\n");
	}

	// in any case, restore the default signal handlers
	signal(SIGSEGV, SIG_DFL);
	signal(SIGBUS, SIG_DFL);
	// and transmit the signal again in order to cause the program to crash
	raise(sig);
}

// HELPER FUNCTION: check TPS access error, with specified @offset, @length, and @buffer
// return -1 if buffer is NULL, or reading out of bounds
// return 0 if no error
static int check_tps_access_error(size_t offset, size_t length, char *buffer)
{
	if (!buffer) {
		return -1;
	}
	// check if reading / writing out of bound
	else if ((offset >= TPS_SIZE) || (offset + length > TPS_SIZE)) {
		return -1;
	}
	return 0;
}

// HELPER FUNCTION: get the TPS with given tid
// return the pointer to the TPS, or return NULL if not found
static TPS* get_tps_with_tid(queue_t queue, pthread_t tid)
{
	TPS* tps_with_current_tid = NULL;
	iterate_find_tid(tps_queue, tid, &tps_with_current_tid);
	if (!tps_with_current_tid) {
		return NULL;
	}
	return tps_with_current_tid;
}

/* API functions */

// initialize TPS
// @segv: activate segfault handler if @segv != 0; the handler will recognize TPS protection errors and display "TPS protection error" on stderr
// return -1 if TPS API already initialized, or on failure in initialization
// return 0 if successfully initialized
int tps_init(int segv)
{
	if (tps_initialized) {
		return -1;
	}

	tps_initialized = true;

	// provided snippet from professor Porquet
	if (segv) {
		struct sigaction sa;

		sigemptyset(&sa.sa_mask);
		sa.sa_flags = SA_SIGINFO;
		sa.sa_sigaction = segv_handler;
		sigaction(SIGBUS, &sa, NULL);
		sigaction(SIGSEGV, &sa, NULL);
	}
	// end of provided snippet

	return 0;
}

// create a TPS, and associate it to the current thread; the TPS is initialized to all zeros
// return -1 if current thread already has a TPS, or on failure in creation
// return 0 if successful
int tps_create(void)
{
	enter_critical_section();

	pthread_t current_tid = pthread_self();

	// first check if current thread already has a TPS
	TPS* current_thread_tps = get_tps_with_tid(tps_queue, current_tid);
	if (current_thread_tps) {
		exit_critical_section();
		return -1;
	}

	// allocate memory, and then check if allocation is successful
	void* mapped_area = init_mapped_area_helper();
	if (mapped_area == MAP_FAILED) {
		exit_critical_section();
		return -1;
	}

	// create the new TPS, and initialize its tid and page
	TPS* new_tps = init_new_tps_helper(current_tid,
		/* TPS page to use = */NULL, mapped_area);
	if (!new_tps) {
		exit_critical_section();
		return -1;
	}

	// enque new_tps into tps_queue
	check_create_queue(&tps_queue); // if the tps_queue was NULL, create it first
	queue_enqueue(tps_queue, new_tps);

	exit_critical_section();
	return 0;
}

// destroy the TPS associated to current thread
// return -1 if no TPS of current thread
// return 0 if successful
int tps_destroy(void)
{
	enter_critical_section();

	pthread_t current_tid = pthread_self();

	// find the TPS to be destroyed
	// if not found, return -1
	TPS* tps_to_destroy = get_tps_with_tid(tps_queue, current_tid);
	if (!tps_to_destroy) {
		exit_critical_section();
		return -1;
	}

	unsigned int* ref_counter = &(tps_to_destroy->page->ref_counter);
	// if the TPS to be destroyed has only one reference count (only one thread referring to the mapped_area of this TPS), then it is to be deleted from tps_queue and freed
	assert(*ref_counter > 0);
	if (*ref_counter == 1) {
		queue_delete(tps_queue, tps_to_destroy);
		check_destroy_queue(&tps_queue); // if the tps_queue is already empty after deletion, destroy it and set it to NULL

		// unset protection, unmap the mapped area, and free the memory
		if (tps_to_destroy->page) {
			void* mapped_area = tps_to_destroy->page->mapped_area;
			mprotect(mapped_area, TPS_SIZE, PROT_READ|PROT_WRITE);
			munmap(mapped_area, TPS_SIZE);
			free(tps_to_destroy->page);
		}
		free(tps_to_destroy);
	}
	// otherwise, if there are multiple threads referring to the mapped_area of this TPS, then decrement the reference counter, but do nothing in the tps_queue
	else {
		--(*ref_counter);
	}

	exit_critical_section();
	return 0;
}

// read from current thread's TPS, from an @offset by a given @length, and receive the data from @buffer
// return -1 if no TPS of current thread, if reading out of bound, if @buffer is NULL, or on failure
// return 0 if successful
int tps_read(size_t offset, size_t length, char *buffer)
{
	if (check_tps_access_error(offset, length, buffer) == -1) {
		return -1;
	}

	enter_critical_section();

	// find the TPS with the current tid to read from
	// return -1 if not found, or if its page is NULL
	pthread_t current_tid = pthread_self();
	TPS* tps_to_read = get_tps_with_tid(tps_queue, current_tid);
	if ((!tps_to_read) || (!(tps_to_read->page))) {
		exit_critical_section();
		return -1;
	}

	void* tps_mapped_area = tps_to_read->page->mapped_area;
	void* mapped_area_to_read = (void*)(tps_mapped_area + offset);

	// temporarily disable reading protection
	if (mprotect(tps_mapped_area, TPS_SIZE, PROT_READ) == -1) {
		exit_critical_section();
		fprintf(stderr, "Try to set PROT_READ error!\n");
		return -1;
	}

	// read from mapped area to buffer
	memcpy(buffer, mapped_area_to_read, length);

	// reenable protection
	if (mprotect(tps_mapped_area, TPS_SIZE, PROT_NONE) == -1) {
		exit_critical_section();
		fprintf(stderr, "Try to set PROT_NONE error!\n");
		return -1;
	}

	exit_critical_section();
	return 0;
}

// write to current thread's TPS, at an @offset by a given @length from @buffer
// if current thread's TPS shares a memory page with another thread's TPS, trigger a copy-and-write operation before actually writing
// return -1 if no TPS of current thread, if writing out of bound, or if @buffer is NULL, or on failure
// return 0 if successfully written to
int tps_write(size_t offset, size_t length, char *buffer)
{
	if (check_tps_access_error(offset, length, buffer) == -1) {
		return -1;
	}

	enter_critical_section();

	// find the TPS with the current tid to write to
	// return -1 if not found, or if its page is NULL, or if mapped area is NULL
	pthread_t current_tid = pthread_self();
	TPS* tps_to_write = get_tps_with_tid(tps_queue, current_tid);
	if ((!tps_to_write) || (!(tps_to_write->page))
		|| (!(tps_to_write->page->mapped_area))) {
		exit_critical_section();
		return -1;
	}

	unsigned int* ref_counter = &(tps_to_write->page->ref_counter);
	assert(*ref_counter > 0);
	// write directly if the current thread's TPS refers to its dedicated TPS page
	if (*ref_counter == 1) {
		void* tps_mapped_area = tps_to_write->page->mapped_area;
		void* mapped_area_to_write = (void*)(tps_mapped_area + offset);

		// temporarily disable writing protection
		if (mprotect(tps_mapped_area, TPS_SIZE, PROT_WRITE) == -1) {
			fprintf(stderr, "Try to set PROT_WRITE error!\n");
			exit_critical_section();
			return -1;
		}

		// write from buffer to mapped area
		memcpy(mapped_area_to_write, buffer, length);

		// reenable protection
		if (mprotect(tps_mapped_area, TPS_SIZE, PROT_NONE) == -1) {
			exit_critical_section();
			fprintf(stderr, "Try to set PROT_NONE error!\n");
			return -1;
		}
	}
	// otherwise, if current thread's TPS temporarily "borrows" the TPS page of some other threads, perform copy-and-write
	// a new TPS page is created, then the current threads gives up the borrowed page, and use the new page instead
	else {
		// update the states of the old (borrowed) TPS
		TPS* old_tps = tps_to_write;
		TPS_page* old_tps_page = old_tps->page;
		if (!old_tps_page) {
			exit_critical_section();
			return -1;
		}
		--(old_tps_page->ref_counter);

		// update tps_to_write with current thread's tid
		pthread_t current_tid = pthread_self();
		tps_to_write->tid = current_tid;

		// let current thread's TPS use the new page
		// create a new TPS page and allocate memory
		TPS_page* new_tps_page = (TPS_page*)malloc(sizeof(TPS_page*));
		if (!new_tps_page) {
			exit_critical_section();
			return -1;
		}
		tps_to_write->page = new_tps_page;

		// create the new_mapped_area for the new page, and set the new page's ref_counter to be 1
		void* new_mapped_area = init_mapped_area_helper();
		if (new_mapped_area == MAP_FAILED) {
			exit_critical_section();
			return -1;
		}
		new_tps_page->mapped_area = new_mapped_area;
		new_tps_page->ref_counter = 1;

		// temporarily disable writing protection
		void* old_mapped_area = old_tps_page->mapped_area;
		if ((mprotect(old_mapped_area, TPS_SIZE, PROT_WRITE) == -1)
			|| (mprotect(new_mapped_area, TPS_SIZE, PROT_WRITE) == -1))  {
			exit_critical_section();
			fprintf(stderr, "Try to set PROT_WRITE error!\n");
			return -1;
		}

		// copy memory from the old TPS page's mapped area to the new TPS page's mapped area, and write the new mapped area using the buffer
		void* mapped_area_to_write = (void*)(new_mapped_area + offset);
		memcpy(new_mapped_area, old_mapped_area, TPS_SIZE);
		memcpy(mapped_area_to_write, buffer, length);

		// reenable protection
		if ((mprotect(old_mapped_area, TPS_SIZE, PROT_NONE) == -1)
			|| (mprotect(new_mapped_area, TPS_SIZE, PROT_NONE) == -1))  {
			exit_critical_section();
			fprintf(stderr, "Try to set PROT_NONE error!\n");
			return -1;
		}
	}

	exit_critical_section();
	return 0;
}

// clone the TPS of @tid
// first phase: copy the TPS's content directly
// last phase: do NOT copy content, but refer to the same memory page
// return -1 if @tid does not have TPS, if current thread has TPS, or on failure
// return 0 if successfully cloned
int tps_clone(pthread_t tid)
{
	enter_critical_section();

	pthread_t current_tid = pthread_self();

	// check if the passed tid does not have a TPS, or if the current thread already has a TPS
	// if it is either case, return -1
	TPS* tps_with_passed_tid = get_tps_with_tid(tps_queue, tid);
	TPS* tps_with_current_tid = get_tps_with_tid(tps_queue, current_tid);
	if ((!tps_with_passed_tid) || tps_with_current_tid) {
		exit_critical_section();
		return -1;
	}

	// create the new TPS for the current thread
	// borrow the the passed TID's TPS page to let the new TPS use it, until the new TPS wants to write (copy-on-writing)
	TPS* tps_to_clone = tps_with_passed_tid;
	TPS* new_tps = init_new_tps_helper(current_tid,
		/* TPS page to use = */tps_to_clone->page,
		/* mapped_area (not used for cloning) = */NULL);
	if (!new_tps) {
		exit_critical_section();
		return -1;
	}

	// enque new_tps into tps_queue
	check_create_queue(&tps_queue); // if the tps_queue was NULL, create it first
	queue_enqueue(tps_queue, new_tps);

	exit_critical_section();
	return 0;
}
