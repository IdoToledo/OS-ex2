#include <iostream>
#include <stdio.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/time.h>
#include <queue>
#include <set>
#include <unordered_map>
#include "resources/uthreads.h"
#define SECOND 1000000
#define ERROR (-1)
#define JB_SP 6
#define JB_PC 7

typedef unsigned long address_t;
typedef void (*thread_entry_point)(void);
typedef enum STATE{
    RUNNING,
    BLOCKED,
    READY,
}STATE;

class IThread
{
    public:
        int tid;
        STATE tstate;
        char stack[STACK_SIZE];
        int t_q_counter;
        IThread(int id=0, STATE state=RUNNING, int count=1) : tid(id), tstate(state), t_q_counter(count){}
};

std::unordered_map<int, IThread*> all_threads;
std::queue<IThread*>ready_q;
IThread* running_thread;
std::set<IThread*> block_set;
int q_counter; // Counts total amount of quantums
struct itimerval timer;
struct sigaction sa;
sigjmp_buf env[MAX_THREAD_NUM];

int timer_setup(int quantum_usecs);
void print_err(bool is_system, const std::string& str);

void schedule()
{
    running_thread = ready_q.front();
    running_thread->tstate = RUNNING;
    ready_q.pop();
}

void quantum_handler(int sig)
{
    std::cout << "QUANTA!" << std::endl;
    int current_thread = running_thread->tid;
    running_thread->tstate = READY;
    running_thread->t_q_counter++;
    q_counter++;
    ready_q.push(running_thread);

    schedule();
    int ret_val = sigsetjmp(env[current_thread], 1);
    if (ret_val == 0)
    {
        siglongjmp(env[running_thread->tid], 1);
    }
}

int timer_setup(int quantum_usecs)
{
    sa = {nullptr};
    sa.sa_handler = &quantum_handler;
    if (sigaction(SIGVTALRM, &sa, NULL) < 0)
    {
        print_err(true, "Couldn't assign sigaction with SIGVTALRM");
        return ERROR;
    }
    timer.it_value.tv_sec = 1;
    timer.it_value.tv_usec = 0;
    timer.it_interval.tv_sec = 3;
    timer.it_interval.tv_usec = 0;
    if (setitimer(ITIMER_VIRTUAL, &timer, NULL))
    {
        print_err(true, "Couldn't assign timer with virtual timer");
        return ERROR;
    }
    return true;
}


void print_err(bool is_system,const std::string& str) {
    if (is_system){
        std::cerr << "system error: "<< str <<std::endl;
    }
    else{
        std::cerr << "thread library error: "<< str << std::endl;
    }
}


int uthread_init(int quantum_usecs)
{
    if (quantum_usecs < 0)
    {
        print_err(false,"Invalid quantum_usecs");
        return ERROR;
    }

    q_counter = 1;
    ready_q = std::queue<IThread*>();
    block_set = std::set<IThread*>();
    if (timer_setup(quantum_usecs) == ERROR)
    {
        return ERROR;
    }

    IThread* first_thread = new IThread {0, RUNNING, 1};
    running_thread = first_thread;
    all_threads[0] = first_thread;
//    sigsetjmp(env[0], 1);
    return EXIT_SUCCESS;
}

int get_first_id()
{
    for (int i = 0; i < MAX_THREAD_NUM; i++)
    {
        if (all_threads.find(i) == all_threads.end())
        {
            return i;
        }
    }
    print_err(false, "No first id");
    return ERROR;
}

address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%fs:0x30,%0\n"
                 "rol    $0x11,%0\n"
            : "=g" (ret)
            : "0" (addr));
    return ret;
}


int uthread_spawn(thread_entry_point entry_point)
{
    if (entry_point == nullptr)
    {
        print_err(false, "Must receive a valid function as an entry"
                         "point");
        return ERROR;
    }
    if (all_threads.size() == MAX_THREAD_NUM)
    {
        print_err(false, "Max amount of threads reached");
        return ERROR;
    }
    IThread* thread = new IThread(get_first_id(), READY, 0);
    address_t sp = (address_t) thread->stack + STACK_SIZE - sizeof(address_t);
    address_t pc = (address_t) entry_point;
    sigsetjmp(env[thread->tid], 1); // TODO WHAT IS SAVEMASK
    (env[thread->tid]->__jmpbuf)[JB_SP] = translate_address(sp);
    (env[thread->tid]->__jmpbuf)[JB_PC] = translate_address(pc);
    if (sigemptyset(&env[thread->tid]->__saved_mask) == ERROR)
    {
        print_err(false, "Problem in initializing sigemptyset");
        return ERROR;
    }


    all_threads[thread->tid] = thread;
    ready_q.push(thread);
    return thread->tid;
}

IThread* pop_thread_from_ready_q(int tid)
{
    IThread* res;
    IThread* tmp;
    for (int i = 0; i < ready_q.size(); i++)
    {
        tmp = ready_q.front();
        ready_q.pop();
        if (tmp->tid == tid)
        {
            ready_q.push(tmp);
        }
        else{
            res = tmp;
        }
    }
    return res;
}
void clear_all()
{
    for (auto pair : all_threads)
    {
        delete pair.second;
    }
    //TODO check if we need to delete the other DAST
}

void clear_thread(int tid)
{
    if (all_threads[tid]->tstate == READY)
    {
        pop_thread_from_ready_q(tid);
    }
    else if (all_threads[tid]->tstate == RUNNING)
    {
        schedule();
    }
    else
    {
        block_set.erase(all_threads[tid]);
    }
    IThread* tmp = all_threads[tid];
    all_threads.erase(tid);
    delete tmp;
}


int uthread_terminate(int tid)
{
    if (all_threads.find(tid) == all_threads.end())
    {
        print_err(false, "Invalid thread ID");
        return ERROR;
    }
    if (tid == 0)
    {
        clear_all();
        exit(0);
    }
    clear_thread(tid);
    return EXIT_SUCCESS;
}


int uthread_block(int tid)
{
    if (all_threads.find(tid) == all_threads.end() || tid == 0)
    {
        print_err(false, "Invalid thread ID to block");
        return ERROR;
    }
    IThread* blocked_thread;
    if (all_threads[tid]->tstate == READY)
    {
        blocked_thread = pop_thread_from_ready_q(tid);
    }
    else if (all_threads[tid]->tstate == RUNNING)
    {
        blocked_thread = all_threads[tid];
        schedule();
    }
    block_set.insert(blocked_thread);
    return EXIT_SUCCESS;
}

int uthread_resume(int tid)
{
    if (all_threads.find(tid) == all_threads.end())
    {
        print_err(false, "Invalid thread ID");
        return ERROR;
    }
    IThread* resumed = all_threads[tid];
    if (all_threads[tid]->tstate == BLOCKED)
    {
        block_set.erase(resumed);
        ready_q.push(resumed);
    }
    return EXIT_SUCCESS;
}


int uthread_sleep(int num_quantums)
{
    if (running_thread->tid == 0)
    {
        print_err(false, "You can't put the main thread to sleep!");
        return ERROR;
    }
}


int uthread_get_tid()
{
    return running_thread->tid;
}


int uthread_get_total_quantums()
{
    return q_counter;
}


int uthread_get_quantums(int tid)
{
    if (all_threads.find(tid) == all_threads.end())
    {
        print_err(false, "Invalid thread ID");
        return ERROR;
    }
    return all_threads[tid]->t_q_counter;
}

void thread1(void)
{
    std::cout << "Thread1" << std::endl;

    int i = 0;
    while (true)
    {
        ++i;
//        std::cout << "in thread1 (" << i <<")"<< std::endl;
        if (i % 3 == 0)
        {
//            std::cout << "thread1: yielding" << std::endl;
        }
//        usleep(SECOND / 3);
    }
}

int main() {
    std::cout << "Hello, World!" << std::endl;

    if (uthread_init(SECOND) == 0)
    {
        uthread_spawn(thread1);
    }
    std::cout << "Thread0" << std::endl;
    int i = 0;
    while (true)
    {
        ++i;
//        std::cout << "in thread0 (" << i <<")"<< std::endl;
        if (i % 3 == 0)
        {
//            std::cout <<"thread0: yielding" << std::endl;
        }
//        usleep(SECOND);
    }
    return 0;
}
