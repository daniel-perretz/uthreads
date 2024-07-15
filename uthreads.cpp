#include "iostream"
#include <map>
#include "uthreads.h"
#include <iostream>
#include <csetjmp>
#include <csignal>
#include <queue>
#include <sys/time.h>
#include <cstring>


static int running_thread;
static int blocked;
static std::priority_queue<int,std::vector<int>,std::greater<int>> tid_heap;
static struct sigaction sa;
sigset_t blocked_sig;
static struct itimerval timer;
static int total_quantums;


void delete_all_threads();



#ifdef __x86_64__
/* code for 64 bit Intel arch */

typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7

/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%fs:0x30,%0\n"
                 "rol    $0x11,%0\n"
            : "=g" (ret)
            : "0" (addr));
    return ret;
}

#else
/* code for 32 bit Intel arch */

typedef unsigned int address_t;
#define JB_SP 4
#define JB_PC 5


/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%gs:0x18,%0\n"
                 "rol    $0x9,%0\n"
    : "=g" (ret)
    : "0" (addr));
    return ret;
}


#endif



/* System error messages */
#define SYSTEM_ERROR "system error: "
#define SIGPROMASK_ERROR "sigpromask error"
#define TIMER_ERROR "setitimer error"
#define SIGACTION_ERROR "sigaction error"
#define ALLOC_ERROR "allocation failed"


/* Thread library error messages */
#define LIBRARY_ERROR "thread library error: "
#define INVALID_QUANTUM "invalid quantum value (must be positive)"
#define NULL_ENTRY_POINT "the entry point function can't be null"
#define MAXIMUM_THREADS "reached the maximal number of threads"
#define TERMINATE_MAIN "can't terminate the main thread"
#define BLOCK_MAIN "can't block the main thread"
#define TID_NOT_EXISTS "thread with this tid not exists"



/*
 * Enum for thread's possible states
 */
enum State {

    READY,RUNNING,BLOCKED
};


/*
 * Thread struct
 */
typedef struct Thread {
    int tid;
    State state;
    int thread_quantums;
    sigjmp_buf env;
    char stack[STACK_SIZE];
    int wake_up_time = -1;
}Thread;


static std::map<int,Thread*> all_threads;
static std::deque<int> ready_queue;
static std::vector<int> sleeping;


/*
 * Initialize the tid heap
 */
void init_heap()
{
  for (int i = 0; i < MAX_THREAD_NUM; ++i) {
      tid_heap.push(i);
    }
}


/*
 * Blocks the signal mask.
 */
void block() {
    if (sigprocmask(SIG_BLOCK, &blocked_sig, nullptr) != 0) {
        std::cerr << SYSTEM_ERROR << SIGPROMASK_ERROR <<std::endl;
        exit(EXIT_FAILURE);
    }
}


/*
 * Unblocks the signal mask.
 */
void unblock() {
    if (sigprocmask(SIG_UNBLOCK, &blocked_sig, nullptr) != 0) {
        std::cerr << SYSTEM_ERROR << SIGPROMASK_ERROR <<std::endl;
        exit(EXIT_FAILURE);
    }
}


/*
 * Deletes a thread: frees the memory of it, deletes from all_threads and add
 * its tid back to the tid_heap.
 */
void delete_thread(Thread **thread) {
    all_threads.erase((*thread)->tid);
    tid_heap.push((*thread)->tid);
    delete *thread;
    *thread = nullptr;
}


/*
 * Deletes all the threads
 */
void delete_all_threads() {
    int size = all_threads.size();

    for (int i = 0; i < size; ++i) {
        delete_thread(&all_threads[i]);
    }

    all_threads.clear();
    ready_queue.clear();
    sleeping.clear();
}


/*
 * Resets the virtual timer.
 */
void reset_timer() {
    if (setitimer(ITIMER_VIRTUAL,&timer, nullptr))
    {
        std::cerr << SYSTEM_ERROR << TIMER_ERROR <<std::endl;
        exit(EXIT_FAILURE);
    }
}


/*
 * Initializes the thread's properties and creates it.
 */
void createThread(Thread *thread, int tid, thread_entry_point entry_point) {
    thread->tid = tid_heap.top();
    tid_heap.pop();
    thread->state = READY;
    thread->thread_quantums = 0;

    address_t sp = (address_t) thread->stack + STACK_SIZE - sizeof(address_t);
    address_t pc = (address_t) entry_point;

    unblock();
    sigsetjmp(thread->env, 1);
    block();

    (thread->env->__jmpbuf)[JB_SP] = translate_address(sp);
    (thread->env->__jmpbuf)[JB_PC] = translate_address(pc);
    sigemptyset(&thread->env->__saved_mask);
}


/*
 * Wakes up all the sleeping threads that needs to wake up during the current
 * quantum.
 */
void wake_up_threads() {
    for (auto it = sleeping.begin(); it != sleeping.end();) {
        if (all_threads[*it]->wake_up_time == total_quantums) {
            all_threads[*it]->wake_up_time = -1;

            if (all_threads[*it]->state == BLOCKED) {
                all_threads[*it]->state = READY;
                ready_queue.push_back(*it);
            }

            it = sleeping.erase(it);
        }

        else {
            ++it;
        }
    }
}


/*
 * Timer handler
 */
void timer_handler(int sig)
{
    block();

    // the thread's quantum is over --> push it to the back of the queue
    if(all_threads.at(running_thread)->state == RUNNING){
        all_threads.at(running_thread)->state = READY;
        ready_queue.push_back(running_thread);
    }

    total_quantums++;
    wake_up_threads();

    unblock();
    int ret_val = sigsetjmp(all_threads[running_thread]->env, 1);
    block();

    bool did_just_save_bookmark = ret_val == 0;
    if (did_just_save_bookmark)
    {
        running_thread = ready_queue.front();
        ready_queue.pop_front();
        all_threads.at(running_thread)->state = RUNNING;
        all_threads.at(running_thread)->thread_quantums ++;
        unblock();
        siglongjmp(all_threads[running_thread]->env, 1);
    }

    else {
        unblock();
    }

}


/**
 * @brief initializes the thread library.
 *
 * Once this function returns, the main thread (tid == 0) will be set as
 * RUNNING. There is no need to provide an entry_point or to create a stack
 * for the main thread - it will be using the "regular" stack and PC.
 * You may assume that this function is called before any other thread library
 * function, and that it is called exactly once.
 * The input to the function is the length of a quantum in micro-seconds.
 * It is an error to call this function with non-positive quantum_usecs.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_init(int quantum_usecs)
{
    if(quantum_usecs <= 0) {
        std::cerr << LIBRARY_ERROR << INVALID_QUANTUM << std::endl;
        return -1;
    }

    Thread *main_thread = new (std::nothrow) Thread();
    init_heap();
    main_thread->tid = tid_heap.top();
    tid_heap.pop();
    main_thread->state = RUNNING;
    main_thread->thread_quantums = 1;
    total_quantums = 1;
    running_thread = main_thread->tid;
    all_threads[main_thread->tid] = main_thread;

    sigemptyset(&blocked_sig);
    sigaddset(&blocked_sig, SIGVTALRM);

    sa.sa_handler = &timer_handler;
    if(sigaction(SIGVTALRM,&sa, nullptr)<0){
        std::cerr << SYSTEM_ERROR << SIGACTION_ERROR <<std::endl;
        exit(EXIT_FAILURE);
    }

    timer.it_value.tv_sec = quantum_usecs / 1000000;
    timer.it_value.tv_usec = quantum_usecs % 1000000;
    timer.it_interval.tv_sec = quantum_usecs / 1000000;
    timer.it_interval.tv_usec = quantum_usecs % 1000000;

    reset_timer();
    return 0;
}


/**
 * @brief Creates a new thread, whose entry point is the function entry_point
 * with the signature void entry_point(void).
 *
 * The thread is added to the end of the READY threads list.
 * The uthread_spawn function should fail if it would cause the number of
 * concurrent threads to exceed the limit (MAX_THREAD_NUM).
 * Each thread should be allocated with a stack of size STACK_SIZE bytes.
 * It is an error to call this function with a null entry_point.
 *
 * @return On success, return the ID of the created thread. On failure, return
 * -1.
*/
int uthread_spawn(thread_entry_point entry_point)
{
    block();

    // null entry point
    if (!entry_point) {
        std::cerr << LIBRARY_ERROR << NULL_ENTRY_POINT << std::endl;
        unblock();
        return -1;
    }

    // reached the maximum amount of threads
    if (tid_heap.empty()) {
        std::cerr << LIBRARY_ERROR << MAXIMUM_THREADS << std::endl;
        unblock();
        return -1;
    }

    int tid = tid_heap.top();
    all_threads[tid]  = new(std::nothrow) Thread();

    // failed to allocate thread
    if (!all_threads[tid]) {
        std::cerr << SYSTEM_ERROR << ALLOC_ERROR << std::endl;
        exit(EXIT_FAILURE);
    }

    createThread(all_threads[tid] ,tid,entry_point);
    ready_queue.push_back(tid);

    unblock();
    return tid;
}


/**
 * @brief Terminates the thread with ID tid and deletes it from all relevant
 * control structures.
 *
 * All the resources allocated by the library for this thread should be
 * released. If no thread with ID tid exists it is considered an error.
 * Terminating the main thread (tid == 0) will result in the termination of
 * the entire process using exit(0) (after releasing the assigned library
 * memory).
 *
 * @return The function returns 0 if the thread was successfully terminated
 * and -1 otherwise. If a thread terminates itself or the main thread is
 * terminated, the function does not return.
*/
int uthread_terminate(int tid)
{
    block();

    // try to delete the main thread
    if (tid == 0) {
        delete_all_threads();
        unblock();
//        std::cerr << LIBRARY_ERROR << TERMINATE_MAIN << std::endl;
        exit(0);
    }

    // try to delete a thread that not exists
    else if (all_threads.find(tid) == all_threads.end())
    {
        std::cerr << LIBRARY_ERROR << TID_NOT_EXISTS << std::endl;
        unblock();
        return -1;
    }

    // delete the running thread
    else if (tid == running_thread)
    {
        delete_thread(&all_threads[tid]);
        reset_timer();
        running_thread = ready_queue.front();
        total_quantums++;
        wake_up_threads();
        all_threads.at(running_thread)->state = RUNNING;
        all_threads.at(running_thread)->thread_quantums++;
        ready_queue.pop_front();
        unblock();
        siglongjmp(all_threads[running_thread]->env, 1);
    }

    // delete a ready thread
    else if (all_threads[tid]->state == READY)
    {
        delete_thread(&all_threads[tid]);
        for (auto i = ready_queue.begin(); i != ready_queue.end() ; i++) {
            if(tid == *i){
                ready_queue.erase(i);
                break;
            }
        }
    }

    // delete a blocked thread
    else if (all_threads[tid]->state == BLOCKED)
    {
        delete_thread(&all_threads[tid]);
        for (auto i = ready_queue.begin(); i != ready_queue.end() ; i++) {
            if(tid == *i){
                ready_queue.erase(i);
                break;
            }
        }
    }

    unblock();
    return 0;
}


/**
 * @brief Blocks the thread with ID tid. The thread may be resumed later using
 * uthread_resume.
 *
 * If no thread with ID tid exists it is considered as an error. In addition,
 * it is an error to try blocking the main thread (tid == 0). If a thread
 * blocks itself, a scheduling decision should be made. Blocking a thread in
 * BLOCKED state has no effect and is not considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_block(int tid) {
    block();

    // try to block the main thread
    if (tid == 0) {
        std::cerr << LIBRARY_ERROR << BLOCK_MAIN << std::endl;
        unblock();
        return -1;
    }

    // try to block a thread that not exists
    if (all_threads.find(tid) == all_threads.end())
    {
        std::cerr << LIBRARY_ERROR << TID_NOT_EXISTS << std::endl;
        unblock();
        return -1;
    }

    // block the running thread
    if (tid == running_thread)
    {
        all_threads[tid]->state = BLOCKED;
        reset_timer();
        timer_handler(0);
        return 0;
    }

    // block a ready thread
    if (all_threads[tid]->state == READY)
    {
        for (auto i = ready_queue.begin(); i != ready_queue.end() ; i++) {
            if(tid == *i){
                ready_queue.erase(i);
                break;
            }
        }
    }

    all_threads[tid]->state = BLOCKED;
    unblock();
    return 0;
}


/**
 * @brief Resumes a blocked thread with ID tid and moves it to the READY state.
 *
 * Resuming a thread in a RUNNING or READY state has no effect and is not
 * considered as an error. If no thread with ID tid exists it is considered an
 * error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_resume(int tid) {
    block();

    // try to resume a thread that not exists
    if (all_threads.find(tid) == all_threads.end())
    {
        std::cerr << LIBRARY_ERROR << TID_NOT_EXISTS << std::endl;
        unblock();
        return -1;
    }

    // resume the thread if it is blocked and not sleeping
    if(all_threads[tid]->state == BLOCKED && all_threads[tid]->wake_up_time == -1)
    {
        all_threads[tid]->state = READY;
        ready_queue.push_back(tid);
    }

    unblock();
    return 0;

}


/**
 * @brief Blocks the RUNNING thread for num_quantums quantums.
 *
 * Immediately after the RUNNING thread transitions to the BLOCKED state a
 * scheduling decision should be made.
 * After the sleeping time is over, the thread should go back to the end of the
 * READY queue.
 * If the thread which was just RUNNING should also be added to the READY
 * queue, or if multiple threads wake up at the same time, the order in which
 * they're added to the end of the READY queue doesn't matter.
 * The number of quantums refers to the number of times a new quantum starts,
 * regardless of the reason. Specifically, the quantum of the thread which has
 * made the call to uthread_sleep isn’t counted.
 * It is considered an error if the main thread (tid == 0) calls this function.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_sleep(int num_quantums) {
    block();

    // num_quantums cannot be non-positive
    if (num_quantums <= 0) {
        std::cerr << LIBRARY_ERROR << INVALID_QUANTUM << std::endl;
        unblock();
        return -1;
    }

    // try to put asleep the main thread
    if (running_thread == 0) {
        std::cerr << LIBRARY_ERROR << BLOCK_MAIN <<std::endl;
        unblock();
        return -1;
    }

    all_threads[running_thread]->state = BLOCKED;
    all_threads[running_thread]->wake_up_time = total_quantums + num_quantums;
    sleeping.push_back(running_thread);
    reset_timer();
    timer_handler(0);
    unblock();
    return 0;
}


/**
 * @brief Returns the thread ID of the calling thread.
 *
 * @return The ID of the calling thread.
*/
int uthread_get_tid() {
    return running_thread;
}


/**
 * @brief Returns the total number of quantums since the library was
 * initialized, including the current quantum.
 *
 * Right after the call to uthread_init, the value should be 1.
 * Each time a new quantum starts, regardless of the reason, this number
 * should be increased by 1.
 *
 * @return The total number of quantums.
*/
int uthread_get_total_quantums() {
    return total_quantums;
}


/**
 * @brief Returns the number of quantums the thread with ID tid was in RUNNING
 * state.
 *
 * On the first time a thread runs, the function should return 1.
 * Every additional quantum that the thread starts should increase this value
 * by 1 (so if the thread with ID tid is in RUNNING state when this function
 * is called, include also the current quantum).
 * If no thread with ID tid exists it is considered an error.
 *
 * @return On success, return the number of quantums of the thread with ID tid.
 * On failure, return -1.
*/
int uthread_get_quantums(int tid) {
    block();

    // the thread not exists
    if (all_threads.find(tid) == all_threads.end())
    {
        std::cerr << LIBRARY_ERROR << TID_NOT_EXISTS << std::endl;
        unblock();
        return -1;
    }

    unblock();
    return all_threads[tid]->thread_quantums;
}



