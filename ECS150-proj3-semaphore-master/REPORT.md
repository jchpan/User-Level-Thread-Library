# ECS 150 project 3: semaphore and threaded private storage

## Brief code description
In this project, we have primarily implemented two things: the **semaphore**,
and the **threaded private storage (TPS)**.
* The **semaphore** is an auxiliary mechanism used in multi-threaded programs,
proposed by Dijkstra in 1965. It is an object that is shared among the threads
of a process. Using a protection technique called "critical section", it ensures
the safety of operating on shared data. Moreover, using a waiting queue, it also
achieves the objective of thread synchronization.
* The **threaded private storage (TPS)** is a design pattern that allocates a
dedicated memory region for each thread. This enables each thread to have a
private space in which it can access its own data, and such data are isolated
from other threads.

## Phase 1: semaphore

### Data structure
For the semaphore struct, we used a `blocked_queue` member to hold the threads
that are blocked, and a `count` member to keep track of the number of available
resources associated with that semaphore.

A thread is blocked and put into the `blocked_queue` if the thread tries to grab
a resource using `sem_down()` from a semaphore with count 0 (meaning that the
resource is not available), and this thread will wait for execution until
another thread calls `sem_up()` and release an available resource.

### API functions

#### `sem_create()` and `sem_destroy()`
In `sem_create()`, we allocate the memory for the semaphore using `malloc()`,
create the `blocked_queue` for it using `queue_create()`, and initialize its
`count` as specified.

`sem_destroy()` does the reverse things. We destroy the `blocked_queue` of the
semaphore, and then free the struct of the semaphore.

#### `sem_down()` and `sem_up()`

Both of the functions call `enter_critical_section()` at the beginning, and call
`exit_critical_section()` at the end, so that the shared resources are protected
and can be safely operated by each thread.

The function `sem_down()` takes a resource from a specified semaphore. If that
semaphore has count 0, we block the current thread and put it into the
`blocked_queue`; this thread is blocked until another thread calls `sem_up()`
and makes a resource available to the this thread. Once it is unblocked, it can
take the resource, and we decrement the `count` by 1.

The function `sem_up()` does the inverse process. The current thread releases
the resource it has used, so we increment the `count` by 1. Then we dequeue the
first thread in the `blocked_queue` and unblock it, so it is ready to run.

#### `sem_getvalue()`
This function inspects a semaphore and gives different results depending on the
internal state of the semaphore.
* If the count of the semaphore is positive, propagate `sval` with that count.
* If the count of the semaphore is 0, propagate `sval` with the negative of the
number of threads blocked.

### Testing
To test semaphore, we used three test programs given by professor Porquet.
* In `sem_count.c`, we check if our semaphore works by checking if the numbers
0-19 are printed in increasing order, and if the thread cycle is 2-1-2-1-...
If not, there must be an error.
* In `sem_buffer.c`, we check if the number consumer takes is smaller than the
largest number that producer produces. If the consumer takes a number greater
than the largest number that producer produces, it must be an error. Besides,
if the largest value that producer produces or consumer takes is larger than
the `MAXCOUNT`, it must also be an error.
* In `sem_prime.c`, we check if all numbers the program prints out are prime
numbers; if not, there must be an error.

## Phase 2: threaded private storage (TPS)

### Data structure
When we design the data structure for the TPS, we first read through the entire
phase 2 in the project prompt, and then designed the data structure from a
holistic approach, so that we can added the copy-and-write functionality without
substantial modification later on.
* In the `TPS` struct, we put a `TPS_page*` pointer to the `page` the TPS is
using, as well as a `tid` number. Each thread has its dedicated TPS.
* Then in the `TPS_page` struct, we put a `void*` pointer to the `mapped_area`,
the memory area the page would use, and also a `ref_counter` number that keeps
track of how many TPSes are using that page.
	* When cloning a TPS, we can let the new cloned TPS temporarily "borrow" the
`page` used by the TPS being cloned, and increment the `ref_counter`. The cloned
TPS will access this "borrowed" page temporarily and read data from its
`mapped_area`.
	* Later, if the cloned TPS attempts to write some data, it must be dissociated
from the page it used to borrow, and must be provided with a dedicated new page
that links to the actual `mapped_area` where it will write data to. We will
create the new `page` and the new `mapped_area`, copy the memory data to the new
page's `mapped_area`, and decrement the original page's `ref_counter`. Then the
cloned TPS has its own `page` and `mapped_area`, and can safely write data into
the region isolated from other threads. This is called **copy-on-writing**.

### API functions

All of these functions will call `enter_critical_section()` at the beginning,
and `exit_critical_section()` at the end, to protect the thread safety of
operating on shared resources.

#### `tps_init()`
In the `tps_init()` function, we adapted the code snippet given by professor
Porquet in the project prompt. The snippet sets up the `segv_handler` that is
triggered when accessing a protected TPS mapped area without permission. Right
before that snippet, we put a global variable `tps_initialized` that is `false`
on program initialization, and it is set to `true` when `tps_init()` runs for
the first time. After that, if `tps_init()` is run again, since
`tps_initialized` is already `true`, the function straightly returns -1 and does
nothing of the rest.

#### `tps_create()` and `tps_destroy()`
The `tps_create()` function will first get the current thread's TID and see if
the TPS is already created and in the `tps_queue`. If the TPS is there, do
nothing and return -1. If not, then the TPS is created with the current TID, and
a mapped area is created and linked to the TPS's page. Finally, the TPS is put
into the `tps_queue`.

The `tps_destroy()` function will first see if the TPS of the current thread is
already in the `tps_queue`. If not, the function will do nothing and return -1.
Then the `ref_counter` of the TPS page is checked:
* If `ref_counter` > 1 (more than one threads are referring to it, meaning that
there are some cloned TPSes that have not gained their own memory page yet),
then we simply decrease the `ref_counter` by 1, since the `mapped_area` is
still being used.
* If `ref_counter` == 1, destroying the TPS means that the page is no longer
used, so we can free the memory the TPS and its page had used.

#### `tps_read()` and `tps_write()`
In both of the two functions, we will first check whether the given parameters
(`offset`, `length`, and `buffer`) will cause any TPS access errors: if
`buffer` is NULL, or if we are reading / writing out of bounds. If such error
occurs, then the functions will do nothing and return -1.

Then we iterate through the `tps_queue`, and see if the TPS is actually in the
queue. If not, the function will not proceed but will also return -1.

If nothing goes wrong, then we can access the `mapped_area` and perform the
reading / writing process.
* In **phase 2.1**, since there is no memory protection, after calling
`enter_critical_section()`, we can directly read from memory or write to memory
using `memcpy()`. Then, we exit the critical section using
`exit_critical_section()`.
* In **phase 2.2**, we started to implement **memory protection**. By default,
 **no reading or writing action** is allowed on the TPS; we only grant such
permissions to threads when they call `tps_read()` or `tps_write()`. We use
`mprotect()` to temporarily disable protection, and use `memcpy()` to perform
reading / writing actions. When we finish, we reenable protection again on the
memory using `mprotect()`. If any of the `mprotect()` calls fail, we stop the
execution of reading / writing and return -1.
* In **phase 2.3**, we implemented **copy-on-write**. This is done by first
checking the `ref_counter` of the page:
	* If `ref_counter` is 1, then the page is exclusively used by one single TPS.
We can do the same thing as before we implement copy-on-write.
	* If `ref_counter` > 1, then we realize that the thread performing a writing
action is "borrowing" the page, and it needs to get its real page before
actually writing. Hence, in the "borrowed" page, we decrement the `ref_counter`
by 1. Then, we allocate a new page using `malloc()`, and a new mapped area using
`mmap()`, to let the TPS use them. After that, we can use `memcpy()` to write
the data into the new mapped memory area. Finally we use `mprotect()` to turn
back on the memory protection. If any `mprotect()` calls fail, we stop and
return -1. At the end we use `exit_critical_section()` to finish.

#### `tps_clone()`
We first detect if the given thread has TPS, and if the current thread has no
TPS. If the given thread has no TPS or current thread already has TPS, that is
an error and we return -1. If there is no error, we find the TPS of the given
thread using `get_tps_with_tid()`. Then we create a new TPS for the current
thread using `init_new_tps_helper()`, and provide the page for the new TPS.
* In **phases 2.1 and 2.2**, we used `memcpy()` to copy the page of the given
TPS into the new TPS.
* In **phase 2.3**, we no longer do the cloning, but let the new TPS "borrow"
the page of the given TPS.

Finally, we enqueue the new TPS in our tps_queue.

### Testing
To test TPS, we wrote the test program based on the one given by professor
Porquet. We created 4 tests in `tps_advanced.c`, using a total of 5 threads to
perform our tests.
* `thread1` is a simple test for **phase 2.1**. It checks if our program can
write to its private storage and read from its storage. Besides, it also checks
if the program can read and write correctly when the offset is not equal to
zero.
* `thread2` is to test all exceptions cases using `assert()`.
* `thread3` and `thread4` are modified based on the testing functions in `tps.c`
given by professor Porquet, and are used to test **phase 2.3**. `thread3` clones
`thread4`'s TPS, and then calls `tps_write()` to see if copy-and-write is
successful.
* Finally, `thread5` tests **phase 2.2**. We try to enter protected memory to
test if the program prints **"TPS protection error!"** as specified in the
`segv_handler`, and raises a segmentation fault.

## References
* Ubuntu man pages: `memcpy`, `mmap`, `mprotect`
* [Memory-mapped I/O](https://www.gnu.org/software/libc/manual/html_mono/libc.html#Memory_002dmapped-I_002fO)
