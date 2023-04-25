#include <iostream>
#include <stdio.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/time.h>
#include <queue>
#include <set>
#include <unordered_map>
#include "uthreads.h"
#include <algorithm>
#define SECOND 1000000
#define ERROR (-1)
#define T_SLEEP 0
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
  int sleep_counter = T_SLEEP;
  static std::unordered_map<int, IThread*> all_threads;  // tid and pointer to thread
  static std::queue<IThread*> ready_q;
  static std::vector<IThread*> block_vector;
  static std::vector<IThread*> sleep_vector;
  static int q_counter; // Counts total amount of quantums passed

  IThread(int id=0, STATE state=RUNNING, int count=1) : tid(id), tstate(state), t_q_counter(count){}
};

std::unordered_map<int, IThread*> IThread::all_threads;  // tid and pointer to thread
std::queue<IThread*> IThread::ready_q;
std::vector<IThread*> IThread::block_vector;
std::vector<IThread*> IThread::sleep_vector; // A sleeping thread can be at the same time in blocked+sleeping vectors but not in ready queue
int IThread::q_counter;

IThread* running_thread;
struct itimerval timer;
struct sigaction sa;
sigjmp_buf env[MAX_THREAD_NUM];
sigset_t new_set;


int timer_setup(int quantum_usecs);
int timer_reset(int quantum_usecs);
void print_err(bool is_system, const std::string& str);
void jumping(int thread);
void update_sleeping_threads();


void schedule()
{
  running_thread = IThread::ready_q.front();
  running_thread->tstate = RUNNING;
  running_thread->t_q_counter++;
  IThread::q_counter++;
  IThread::ready_q.pop();
}

void quantum_handler(int sig)
{
  sigprocmask(SIG_BLOCK, &new_set, nullptr);
  update_sleeping_threads();
  int current_thread = running_thread->tid;
  running_thread->tstate = READY;
  IThread::ready_q.push(running_thread);
  schedule();
  jumping(current_thread);
  sigprocmask(SIG_UNBLOCK, &new_set, nullptr);
}


void update_sleeping_threads()
{
  std::vector<IThread*> to_remove;;
  for (auto thread : IThread::sleep_vector){
    thread->sleep_counter--;
    if (thread->sleep_counter == T_SLEEP)
    {
      IThread* waking_thread = thread;
      to_remove.push_back(waking_thread);
      if (waking_thread->tstate != BLOCKED) //tstate must be BLOCKED or ready
      {
        IThread::ready_q.push(waking_thread);
      }
    }
  }
  for (auto thread : to_remove){
    IThread::sleep_vector.erase(std::find(IThread::sleep_vector.begin(), IThread::sleep_vector.end(), thread));
  }
}


void jumping(int set_thread) {
  int ret_val = sigsetjmp(env[set_thread], 1);
  if (ret_val == 0)
  {
    siglongjmp(env[running_thread->tid], 1);
  }
}


int timer_reset(int quantum_usecs)
{
  timer.it_value.tv_sec = 0;
  timer.it_value.tv_usec = quantum_usecs;
  timer.it_interval.tv_sec = 0;
  timer.it_interval.tv_usec = quantum_usecs;
  if (setitimer(ITIMER_VIRTUAL, &timer, NULL))
  {
    print_err(true, "Couldn't assign timer with virtual timer");
    return ERROR;
  }
  return true;
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
  return timer_reset(quantum_usecs);
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
  if (quantum_usecs <= 0)
  {
    print_err(false,"Invalid quantum_usecs");
    return ERROR;
  }

  IThread::q_counter = 1;
  IThread::ready_q = std::queue<IThread*>();
  IThread::block_vector = std::vector<IThread*>();
  if (timer_setup(quantum_usecs) == ERROR)
  {
    return ERROR;
  }

  IThread* first_thread = new IThread {0, RUNNING, 1};
  running_thread = first_thread;
  IThread::all_threads[0] = first_thread;
  sigemptyset(&new_set);
  sigaddset(&new_set, SIGVTALRM);
  return EXIT_SUCCESS;
}

int get_first_id()
{
  for (int i = 0; i < MAX_THREAD_NUM; i++)
  {
    if (IThread::all_threads.find(i) == IThread::all_threads.end())
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
  sigprocmask(SIG_BLOCK, &new_set, nullptr);
  if (entry_point == nullptr)
  {
    print_err(false, "Must receive a valid function as an entry"
                     "point");
    sigprocmask(SIG_UNBLOCK, &new_set, nullptr);
    return ERROR;
  }
  if (IThread::all_threads.size() == MAX_THREAD_NUM)
  {
    print_err(false, "Max amount of threads reached");
    sigprocmask(SIG_UNBLOCK, &new_set, nullptr);
    return ERROR;
  }
  IThread* thread = new IThread(get_first_id(), READY, 0);
  IThread::all_threads[thread->tid] = thread;
  IThread::ready_q.push(thread);

  address_t sp = (address_t) thread->stack + STACK_SIZE - sizeof(address_t);
  address_t pc = (address_t) entry_point;
  sigsetjmp(env[thread->tid], 1); // TODO shaat kabala
  (env[thread->tid]->__jmpbuf)[JB_SP] = translate_address(sp);
  (env[thread->tid]->__jmpbuf)[JB_PC] = translate_address(pc);
  if (sigemptyset(&env[thread->tid]->__saved_mask) == ERROR)
  {
    print_err(false, "Problem in initializing sigemptyset");
    sigprocmask(SIG_UNBLOCK, &new_set, nullptr);
    return ERROR;
  }
  sigprocmask(SIG_UNBLOCK, &new_set, nullptr);
  return thread->tid;
}

IThread* pop_thread_from_ready_q(int tid)
{
  IThread* res;
  IThread* tmp;
  int ready_size = (int) IThread::ready_q.size();
  for (int i = 0; i < ready_size; i++)
  {
    tmp = IThread::ready_q.front();
    IThread::ready_q.pop();
    if (tmp->tid != tid)
    {
      IThread::ready_q.push(tmp);
    }
    else{
      res = tmp;
    }
  }
  return res;
}

void clear_all()
{
  for (auto pair : IThread::all_threads)
  {
    delete pair.second;
  }
}


void clear_thread(int tid)
{
  IThread* tmp = IThread::all_threads[tid];
  if (tmp->tstate == READY)
  {
    pop_thread_from_ready_q(tid);
  }
  else if (tmp->tstate == RUNNING)
  {
    schedule();
  }
  else
  {
    IThread::block_vector.erase(std::find(IThread::block_vector.begin(), IThread::block_vector.end(), IThread::all_threads[tid]));
  }
  if (tmp->sleep_counter > 0)
  {
    IThread::sleep_vector.erase(std::find(IThread::sleep_vector.begin(), IThread::sleep_vector.end(), tmp));
  }
  IThread::all_threads.erase(tid);
  delete tmp;
}


int uthread_terminate(int tid)
{
  sigprocmask(SIG_BLOCK, &new_set, nullptr);
  if (IThread::all_threads.find(tid) == IThread::all_threads.end())
  {
    print_err(false, "Invalid thread ID");
    sigprocmask(SIG_UNBLOCK, &new_set, nullptr);
    return ERROR;
  }
  if (tid == 0)
  {
    clear_all();
    exit(0);
  }
  STATE check_running = IThread::all_threads[tid]->tstate;
  clear_thread(tid);
  if (check_running == RUNNING)
  {
    timer_reset(timer.it_value.tv_usec);
    jumping(tid);
  }
  sigprocmask(SIG_UNBLOCK, &new_set, nullptr);
  return EXIT_SUCCESS;
}


int uthread_block(int tid)
{
  sigprocmask(SIG_BLOCK, &new_set, nullptr);
  if (IThread::all_threads.find(tid) == IThread::all_threads.end() || tid == 0)
  {
    print_err(false, "Invalid thread ID to block");
    sigprocmask(SIG_UNBLOCK, &new_set, nullptr);
    return ERROR;
  }
  int running_thread_tid = running_thread->tid;
  IThread* blocked_thread = IThread::all_threads[tid];
  if (blocked_thread->tstate != BLOCKED)
  {
    IThread::block_vector.push_back(blocked_thread);
    if(blocked_thread->tstate == READY)
    {
      if(blocked_thread->sleep_counter == 0){
        blocked_thread = pop_thread_from_ready_q(tid);
      }
      blocked_thread->tstate = BLOCKED;
    }
    else if (blocked_thread->tstate == RUNNING)
    {
      schedule();
      blocked_thread->tstate = BLOCKED;
      timer_reset(timer.it_value.tv_usec);
      jumping(running_thread_tid);
    }
  }
  sigprocmask(SIG_UNBLOCK, &new_set, nullptr);
  return EXIT_SUCCESS;
}


int uthread_resume(int tid)
{
  sigprocmask(SIG_BLOCK, &new_set, nullptr); // TODO can we put this after error check?
  if (IThread::all_threads.find(tid) == IThread::all_threads.end())
  {
    print_err(false, "Invalid thread ID");
    sigprocmask(SIG_UNBLOCK, &new_set, nullptr);
    return ERROR;
  }
  IThread* resumed = IThread::all_threads[tid];
  if (resumed->tstate == BLOCKED)
  {
    IThread::block_vector.erase(std::find(IThread::block_vector.begin(), IThread::block_vector.end(), resumed));
    if(resumed->sleep_counter == 0){
      IThread::ready_q.push(resumed);
    }
    resumed->tstate = READY;
  }
  sigprocmask(SIG_UNBLOCK, &new_set, nullptr);
  return EXIT_SUCCESS;
}


int uthread_sleep(int num_quantums)
{
  sigprocmask(SIG_BLOCK, &new_set, nullptr);
  if (running_thread->tid == 0) {
    print_err(false, "You can't put the main thread to sleep!");
    sigprocmask(SIG_UNBLOCK, &new_set, nullptr);
    return ERROR;
  }
  running_thread->sleep_counter = num_quantums;
  IThread::sleep_vector.push_back(running_thread);
  running_thread->tstate = READY;
  int jump_from = running_thread->tid;
  schedule();
  timer_reset(timer.it_value.tv_usec);
  jumping(jump_from);
  sigprocmask(SIG_UNBLOCK, &new_set, nullptr);
  return EXIT_SUCCESS;
}


int uthread_get_tid()
{
  return running_thread->tid;
}


int uthread_get_total_quantums()
{
  return IThread::q_counter;
}


int uthread_get_quantums(int tid)
{

  if (IThread::all_threads.find(tid) == IThread::all_threads.end())
  {
    print_err(false, "Invalid thread ID");
    return ERROR;
  }

  return IThread::all_threads[tid]->t_q_counter;
}
