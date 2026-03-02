#include "uthreads.h"
#include <stdio.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdbool.h>
#include <csetjmp>
#include <algorithm>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <queue>


#ifdef __x86_64__
/* code for 64 bit Intel arch */
typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7

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


#define SUCCESS 0
#define ERROR -1
#define MICRO_SEC 1000000
#define MAIN_THREAD_ID 0

#define PRINT_SYSTEM_ERR(msg) fprintf(stderr, "system error: %s\n", msg); exit(ERROR)
#define PRINT_LIB_ERR(msg) fprintf(stderr, "thread library error: %s\n", msg)



enum State {
    RUNNING,
    READY,
    BLOCKED
};


/**
* Thread class to represent a thread in the scheduler.
* It contains the thread ID, state, stack, and signal environment.
*/
class thread {
public:
    int _id;
    int _quantums_count;
    State _state;
    char* _stack;
    sigjmp_buf _sigenv;

public:
    thread(int id, State state)
        : _id(id),
          _quantums_count(0),
          _state(state),
          _stack(nullptr) {
        if (id == MAIN_THREAD_ID) {
            return;
        }

        _stack = new char[STACK_SIZE];
        if (_stack == nullptr) {
            throw std::bad_alloc();
        }
    }

    ~thread() {
        if (_stack != nullptr) {
            delete[] _stack;
        }
    }
};


/**
* Scheduler class to manage threads and their states.
* It uses a priority queue to manage available thread IDs and a deque for ready threads.
* It also handles sleeping threads with a hash map.
**/
class scheduler {
    public:
    int _total_quantums;
    int _quantum_usecs;
    thread* _running_thread;
    struct sigaction sa;
    struct itimerval timer;

    std::priority_queue<int, std::vector<int>, std::greater<int>> _available_ids;
    std::unordered_map<int, thread*> _map_threads;
    std::unordered_map<int, int> _sleeping_threads;
    std::deque<thread*> _ready_threads;

public:
    scheduler(int quantum_usecs)
        : _total_quantums(0),
          _quantum_usecs(quantum_usecs),
          _running_thread(nullptr) {
            
         for(int i = 1; i < MAX_THREAD_NUM; ++i) {
            _available_ids.push(i);
         }
    }

    ~scheduler() {
        for (auto& pair : _map_threads) {
            delete pair.second;
        }
        _map_threads.clear();
    }

    void insert_to_ready(thread* thread) {
        if (thread == nullptr) return;
        thread->_state = READY;
        _ready_threads.push_back(thread);
    }

    bool is_thread_sleeping(int tid) {
        return _sleeping_threads.find(tid) != _sleeping_threads.end();
    }

    int get_new_id() {
        if(_available_ids.empty()) {
            return -1;
        }
        int id = _available_ids.top();
        _available_ids.pop();
        return id;
    }

    int add_new_id(int id) {
        if (id < 0 || MAX_THREAD_NUM <= id) {
            return ERROR;
        }

        _available_ids.push(id);
        return SUCCESS;
    }

    bool is_valid_tid(int tid) {
        return 0 <= tid && tid < MAX_THREAD_NUM &&
               _map_threads.find(tid) != _map_threads.end();
    }

    void remove_from_ready(thread* trd) {
        auto it = std::find(_ready_threads.begin(), _ready_threads.end(), trd);
        if (it != _ready_threads.end()) {
            _ready_threads.erase(it);
        }
    }
};



// static instance of scheduler to manage threads
static scheduler* _scheduler = nullptr;



/**
* Function to set up the thread's stack and signal environment.
*/
void setup_thread(thread *trd, thread_entry_point entry_point) {
    address_t sp = (address_t) trd->_stack + STACK_SIZE - sizeof(address_t);
    address_t pc = (address_t) entry_point;
    sigsetjmp(trd->_sigenv, 1);
    (trd->_sigenv->__jmpbuf)[JB_SP] = translate_address(sp);
    (trd->_sigenv->__jmpbuf)[JB_PC] = translate_address(pc);
    sigemptyset(&trd->_sigenv->__saved_mask);
}


/**
* Function to delete a thread.
* It removes the thread from the ready queue if it is in that state.
* It also removes the thread from the sleeping threads map and deletes the thread object. 
*/
static void delete_thread(int tid) {
    thread* trd = _scheduler->_map_threads[tid];

    if (trd->_state == READY) {
        _scheduler->remove_from_ready(trd);
    }

    _scheduler->_sleeping_threads.erase(tid);
    delete trd;
    _scheduler->_map_threads.erase(tid);
}


/**
* Function to initialize the timer for the scheduler.
* returns true if the timer is set successfully, false otherwise.
*/
static bool init_timer(int quantum_usecs) {
    if (quantum_usecs <= 0 || _scheduler == nullptr) {
        return false;
    }

    _scheduler->timer.it_value.tv_sec = quantum_usecs / MICRO_SEC;
    _scheduler->timer.it_value.tv_usec = quantum_usecs % MICRO_SEC;
    _scheduler->timer.it_interval.tv_sec = quantum_usecs / MICRO_SEC;
    _scheduler->timer.it_interval.tv_usec = quantum_usecs % MICRO_SEC;

    return setitimer(ITIMER_VIRTUAL, &_scheduler->timer, nullptr) == SUCCESS;
}


/**
* Function to handle sleeping threads.
* It decrements the sleep count for each sleeping thread and moves it to the ready state if needed.
* If the sleep count reaches zero, the thread is removed from the sleeping threads map.
*/
void sleeping_threads_handler() {
    std::unordered_set<int> finish_sleep;

    for (auto& trd : _scheduler->_sleeping_threads) {

        auto sleeping_thread = _scheduler->_map_threads[trd.first];
        if (--_scheduler->_sleeping_threads[sleeping_thread->_id] <= 0) {
            if (sleeping_thread->_state != BLOCKED) {
                _scheduler->insert_to_ready(sleeping_thread);
            }
            finish_sleep.insert(sleeping_thread->_id);
        }
    }

    for (auto& tid : finish_sleep) {
        _scheduler->_sleeping_threads.erase(tid);
    }
}


/**
* Function to switch context between threads.
* It saves the current thread's context and switches to the next thread in the ready queue.
* It also updates the state of the threads accordingly. 
* The function uses setjmp and longjmp to handle the context switching.
* It sets up the timer for the next quantum and updates the quantum count for the running thread.
*/
static void context_switch() {
    //get the current thread's context and save it
    int ret = sigsetjmp(_scheduler->_running_thread->_sigenv, 1);

    if (ret == 0) {
        _scheduler->_total_quantums++;

        //get the next thread to run
        _scheduler->_running_thread = _scheduler->_ready_threads.front();
        _scheduler->_ready_threads.pop_front();

        _scheduler->_running_thread->_state = RUNNING;
        _scheduler->_running_thread->_quantums_count++;

        init_timer(_scheduler->_quantum_usecs); 

        //jump to the next thread's context
        siglongjmp(_scheduler->_running_thread->_sigenv, 1);
    }
}


/**
* Function to handle the timer signal.
* It checks if the signal is SIGVTALRM and if the scheduler is not null. 
* It then calls the sleeping threads handler and inserts the running thread back to the ready queue.
* Finally, it calls the context switch function to switch to the next thread.
*/
static void timer_handler(int sig) {
    if (sig != SIGVTALRM || _scheduler == nullptr) {
        return;
    }
    
    sleeping_threads_handler();
    _scheduler->insert_to_ready(_scheduler->_running_thread);
    context_switch();
}


/**
 * @brief initializes the thread library.
 *
 * Once this function returns, the main thread (tid == 0) will be set as RUNNING. There is no need to 
 * provide an entry_point or to create a stack for the main thread - it will be using the "regular" stack and PC.
 * You may assume that this function is called before any other thread library function, and that it is called
 * exactly once.
 * The input to the function is the length of a quantum in micro-seconds.
 * It is an error to call this function with non-positive quantum_usecs.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_init(int quantum_usecs) {
    if (quantum_usecs <= 0) {
        PRINT_LIB_ERR("quantum_usecs must be positive");
        return ERROR;
    }

    _scheduler = new scheduler(quantum_usecs);
    if (_scheduler == nullptr) {
        PRINT_LIB_ERR("Failed to create scheduler");
        return ERROR;
    }

    _scheduler->sa.sa_handler = &timer_handler;

    if (sigemptyset(&_scheduler->sa.sa_mask) != SUCCESS ||
        sigaddset(&_scheduler->sa.sa_mask, SIGVTALRM) != SUCCESS || 
        sigaction(SIGVTALRM, &_scheduler->sa, nullptr) != SUCCESS) {

        delete _scheduler;
        _scheduler = nullptr;
        PRINT_SYSTEM_ERR("Failed to set signal handler");
        return ERROR;
    }

    // Create the main thread and set it as RUNNING
    thread* main = new thread(MAIN_THREAD_ID, RUNNING);

    if (main == nullptr) {
        delete _scheduler;
        _scheduler = nullptr;
        PRINT_SYSTEM_ERR("Failed to allocate main thread");
        return ERROR;
    }

    _scheduler->_running_thread = main;
    _scheduler->_map_threads[MAIN_THREAD_ID] = main;

    main->_quantums_count++;
    _scheduler->_total_quantums++;

    //set the timer
    if (!init_timer(quantum_usecs)) {
        delete _scheduler;
        _scheduler = nullptr;
        delete main;
        PRINT_SYSTEM_ERR("Failed to initialize timer");
        return ERROR;
    } 

    return SUCCESS;
}


/**
 * @brief Creates a new thread, whose entry point is the function entry_point with the signature
 * void entry_point(void).
 *
 * The thread is added to the end of the READY threads list.
 * The uthread_spawn function should fail if it would cause the number of concurrent threads to exceed the
 * limit (MAX_THREAD_NUM).
 * Each thread should be allocated with a stack of size STACK_SIZE bytes.
 * It is an error to call this function with a null entry_point.
 *
 * @return On success, return the ID of the created thread. On failure, return -1.
*/
int uthread_spawn(thread_entry_point entry_point) {
    if (MAX_THREAD_NUM <= _scheduler->_map_threads.size()) {
        PRINT_LIB_ERR("Maximum number of threads reached");
        return ERROR;
    }

    if (entry_point == nullptr) {
        PRINT_LIB_ERR("entry_point cannot be null");
        return ERROR;
    }

    if (_scheduler == nullptr) {
        PRINT_LIB_ERR("Scheduler is not initialized");
        return ERROR;
    }


    int id = _scheduler->get_new_id();

    if (id == ERROR) {
        PRINT_LIB_ERR("Failed to get new thread ID");
        return ERROR;
    }
    
    // Create a new thread and set its state to READY and insert it to the ready queue
    thread* new_thread = new thread(id, READY);

    if (new_thread == nullptr) {
        delete _scheduler;
        _scheduler = nullptr;
        PRINT_SYSTEM_ERR("Failed to allocate new thread");
        return ERROR;
    }

    //if the thread has already removed and srill in map delete it.
    if(_scheduler->_map_threads[id]){
        delete _scheduler->_map_threads[id];
    }

    _scheduler->_map_threads[id] = new_thread;
    _scheduler->insert_to_ready(new_thread);

    setup_thread(new_thread, entry_point);
    return new_thread->_id;
}


/**
 * @brief Terminates the thread with ID tid and deletes it from all relevant control structures.
 *
 * All the resources allocated by the library for this thread should be released. If no thread with ID tid exists it
 * is considered an error. Terminating the main thread (tid == 0) will result in the termination of the entire
 * process using exit(0) (after releasing the assigned library memory).
 *
 * @return The function returns 0 if the thread was successfully terminated and -1 otherwise. If a thread terminates
 * itself or the main thread is terminated, the function does not return.
*/
int uthread_terminate(int tid) {
    if(_scheduler == nullptr) {
        PRINT_LIB_ERR("Scheduler is not initialized");
        return ERROR;
    }

    if (!_scheduler->is_valid_tid(tid)) {
        PRINT_LIB_ERR("Invalid thread ID");
        return ERROR;
    }


    if (tid == MAIN_THREAD_ID) {
        // Terminate the main thread and exit the process
        _scheduler->_ready_threads.push_front(_scheduler->_map_threads[MAIN_THREAD_ID]);
        context_switch();
        delete _scheduler;
        exit(SUCCESS);
    }

    if (tid == _scheduler->_running_thread->_id) {
        _scheduler->add_new_id(tid);
        context_switch();
        return SUCCESS;
    }

    delete_thread(tid);
    _scheduler->add_new_id(tid);

    return SUCCESS;
}


/**
 * @brief Blocks the thread with ID tid. The thread may be resumed later using uthread_resume.
 *
 * If no thread with ID tid exists it is considered as an error. In addition, it is an error to try blocking the
 * main thread (tid == 0). If a thread blocks itself, a scheduling decision should be made. Blocking a thread in
 * BLOCKED state has no effect and is not considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_block(int tid) {
    if(_scheduler == nullptr) {
        PRINT_LIB_ERR("Scheduler is not initialized");
        return ERROR;
    }

    if (!_scheduler->is_valid_tid(tid)) {
        PRINT_LIB_ERR("Invalid thread ID");
        return ERROR;
    }


    if(tid == MAIN_THREAD_ID) {
        PRINT_LIB_ERR("Cannot block main thread (ID 0)");
        return ERROR;
    }


    thread* trd = _scheduler->_map_threads[tid];
    if (trd->_state == READY) {
        _scheduler->remove_from_ready(trd);
    } 

    trd->_state = BLOCKED;

    if (_scheduler->_running_thread->_id == tid) {
        context_switch();
        return SUCCESS;
    }

    return SUCCESS;
}


/**
 * @brief Resumes a blocked thread with ID tid and moves it to the READY state.
 *
 * Resuming a thread in a RUNNING or READY state has no effect and is not considered as an error. If no thread with
 * ID tid exists it is considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_resume(int tid) {
    if(_scheduler == nullptr) {
        PRINT_LIB_ERR("Scheduler is not initialized");
        return ERROR;
    }

    if (!_scheduler->is_valid_tid(tid)) {
        PRINT_LIB_ERR("Invalid thread ID");
        return ERROR;
    }


    thread* to_resume = _scheduler->_map_threads[tid];
    if (to_resume->_state == BLOCKED && !_scheduler->is_thread_sleeping(to_resume->_id)) {
        _scheduler->insert_to_ready(to_resume);
    }
    to_resume->_state = READY;

    return SUCCESS;
}


/**
 * @brief Blocks the RUNNING thread for num_quantums quantums.
 *
 * Immediately after the RUNNING thread transitions to the BLOCKED state a scheduling decision should be made.
 * After the sleeping time is over, the thread should go back to the end of the READY queue.
 * If the thread which was just RUNNING should also be added to the READY queue, or if multiple threads wake up 
 * at the same time, the order in which they're added to the end of the READY queue doesn't matter.
 * The number of quantums refers to the number of times a new quantum starts, regardless of the reason. Specifically,
 * the quantum of the thread which has made the call to uthread_sleep isn’t counted.
 * It is considered an error if the main thread (tid == 0) calls this function.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_sleep(int num_quantums) {
    if(_scheduler == nullptr) {
        PRINT_LIB_ERR("Scheduler is not initialized");
        return ERROR;
    }

    if (num_quantums <= 0) {
        PRINT_LIB_ERR("num_quantums must be positive");
        return ERROR;
    }

    if (_scheduler->_running_thread->_id == MAIN_THREAD_ID) {
        PRINT_LIB_ERR("Main thread cannot sleep");
        return ERROR;
    }


    _scheduler->_sleeping_threads[_scheduler->_running_thread->_id] = num_quantums; 

    if (_scheduler->_running_thread->_state == READY) {
        _scheduler->remove_from_ready(_scheduler->_running_thread);
    } 

    context_switch();

    return SUCCESS;
}


/**
 * @brief Returns the thread ID of the calling thread.
 *
 * @return The ID of the calling thread.
*/
int uthread_get_tid() {
    return _scheduler->_running_thread->_id;
}


/**
 * @brief Returns the total number of quantums since the library was initialized, including the current quantum.
 *
 * Right after the call to uthread_init, the value should be 1.
 * Each time a new quantum starts, regardless of the reason, this number should be increased by 1.
 *
 * @return The total number of quantums.
*/
int uthread_get_total_quantums() {
    return _scheduler->_total_quantums;
}


/**
 * @brief Returns the number of quantums the thread with ID tid was in RUNNING state.
 *
 * On the first time a thread runs, the function should return 1. Every additional quantum that the thread starts should
 * increase this value by 1 (so if the thread with ID tid is in RUNNING state when this function is called, include
 * also the current quantum). If no thread with ID tid exists it is considered an error.
 *
 * @return On success, return the number of quantums of the thread with ID tid. On failure, return -1.
*/
int uthread_get_quantums(int tid) {
    if(_scheduler == nullptr) {
        PRINT_LIB_ERR("Scheduler is not initialized");
        return ERROR;
    }

    if (!_scheduler->is_valid_tid(tid)) {
        PRINT_LIB_ERR("Invalid thread ID");
        return ERROR;
    }

    return _scheduler->_map_threads[tid]->_quantums_count;
}