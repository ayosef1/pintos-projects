#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "devices/timer.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#include "vm/page.h"
#include <hash.h>
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* List all sleeping processes, that is, proccesses that are
   blocked by a call to timer_sleep (). Ordered by time until
   wakeup.  */
static struct list sleeping_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

/* Estimate of average number of threads ready to run over the past minute. */
static fixed_point load_avg;

/* The number of threads in the ready_list */
static int num_ready;


static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static struct thread *highest_priority_ready (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static bool init_child (struct thread *t);
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);
static bool compare_wake_time (const struct list_elem *a,
                               const struct list_elem *b,
                               void *aux UNUSED);

static void update_all_recent_cpu_times (void);
static void update_system_load_avg (void);
static void update_all_priorities (void);
static void update_recent_cpu_time (struct thread *t, void *aux UNUSED);
static void update_mlfqs_priority (struct thread *t, void *aux UNUSED);
static int bound (int x, int lower, int upper);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  /* Initialize system load average at system boot. */
  load_avg = int_to_fp(0);

  /* Naturally, there are zero ready threads at boot. */
  num_ready = 0;

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  list_init (&sleeping_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;
  /* Using advanced scheduler. */
  if (thread_mlfqs) 
    {
      /* Every timer interrupt, recent cpu is incremented by 1 for
        the running thread only, unless the idle thread is running. */
      if (t != idle_thread) 
        {
          t->recent_cpu_changed = true;
          t->recent_cpu_time = add_int_to_fp (t->recent_cpu_time, 1);
        }
      /* Update system load average and recalculate
        recent cpu for every thread once per second */
      if (timer_ticks () % TIMER_FREQ == 0) 
        {
          update_system_load_avg ();
          update_all_recent_cpu_times ();
        } 
      /* Update all priorities every fourth tick. */
      if (timer_ticks () % TIME_SLICE == 0) 
        update_all_priorities ();
    }
  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Initialize thread's child struct if applicable. */
  #ifdef USERPROG
    if (!init_child (t))
      return TID_ERROR;
  #endif
  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
  thread_unblock (t);

  if (t->priority > thread_get_priority())
    thread_yield ();
  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;
  ASSERT (is_thread (t));
  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_push_back(&ready_list, &t->elem);
  num_ready++;
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  struct thread *cur = thread_current ();

  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  cur->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread)
  {
    list_push_back(&ready_list, &cur->elem);
    num_ready++;
  }
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{
  if (thread_mlfqs) return;

  enum intr_level old_level;
  ASSERT (!intr_context ());
  old_level = intr_disable ();

  struct thread *cur = thread_current ();

  bool change_priority = cur->original_priority == cur->priority
                         || new_priority > cur->priority;

  if (change_priority)
    cur->priority = new_priority;

  cur->original_priority = new_priority;

  if (!list_empty (&ready_list)) 
  {
    
    if (highest_priority_ready ()->priority > cur->priority)
      thread_yield ();
  }
  intr_set_level (old_level);
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  enum intr_level old_level;
  int priority;

  old_level = intr_disable ();
  priority = thread_current ()->priority;
  intr_set_level (old_level);

  return priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice) 
{
  enum intr_level old_level;
  old_level = intr_disable ();
  struct thread *cur = thread_current ();
  cur->niceness = bound(nice, NICE_MIN, NICE_MAX);
  update_mlfqs_priority (cur, NULL);

  if (!list_empty (&ready_list)) 
  {
    if (highest_priority_ready ()->priority > cur->priority)
      thread_yield();
  }
  intr_set_level (old_level);
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  enum intr_level old_level;
  int nice;

  old_level = intr_disable ();
  nice = thread_current ()->niceness;
  intr_set_level (old_level);

  return nice;
}

/* Returns 100 times the system load average,
   rounded to the nearest integer. */
int
thread_get_load_avg (void) 
{
  enum intr_level old_level;
  int scaled_load_avg;

  old_level = intr_disable ();
  scaled_load_avg = fp_to_int (mult_fp_by_int (load_avg, PRINT_FP_CONST));
  intr_set_level (old_level);

  return scaled_load_avg;
}

/* Returns 100 times the current thread's recent_cpu value,
   rounded to the nearest integer. */
int
thread_get_recent_cpu (void) 
{
  enum intr_level old_level;
  int scaled_recent_cpu_time;

  old_level = intr_disable ();
  struct thread *cur = thread_current ();
  scaled_recent_cpu_time = fp_to_int (mult_fp_by_int (cur->recent_cpu_time,
                                                      PRINT_FP_CONST));
  intr_set_level (old_level);
  
  return scaled_recent_cpu_time;

}


/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->original_priority = priority;
  t->wake_time = 0;
  t->recent_cpu_changed = true;
  t->wake_sema = NULL;
  t->magic = THREAD_MAGIC;
  t->waiting_lock = NULL;

  if (thread_mlfqs) {
    /* Initial thread has recent cpu time and nice value of 0.
       Other threads inherit these values from their parent. */
    if (t == initial_thread)
      {
        t->niceness = NICE_INITIAL;
        t->recent_cpu_time = RECENT_CPU_TIME_INITIAL;
        t->priority = PRI_DEFAULT;
        t->recent_cpu_changed = false;
      }
    else 
      {
        t->niceness = thread_get_nice ();
        t->recent_cpu_time = int_to_fp (thread_current ()->recent_cpu_time);
        update_mlfqs_priority(t, NULL);
      }
  }

  #ifdef USERPROG
    t->exit_status = 0;
    memset (t->fdtable, 0, sizeof (*t->fdtable));
    /* Set fd = 0 to an invalid ptr */
    t->fdtable[RESERVED_FD] = (void *)THREAD_MAGIC;
    /* First two FDs reserved */
    t->next_fd = EXEC_FD + 1;
  
    list_init (&t->children);
  #endif

  /* Initialize the list of locks held by current list*/
  list_init (&t->locks_held);

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/* Initializes the exit information associated with child thread
   T, adds it to the parent thread's children list and adds it to
   the child T. Returns true if successful, including case where
   t is initial thread and requires no set up. Otherwise, returns 
   false if palloc_get_page fails. */
static bool
init_child (struct thread *t)
{
  if (t != initial_thread)
    {
      struct child_exit_info *exit_info = palloc_get_page (0);
      if (exit_info == NULL)
        return false;

      exit_info->tid = t->tid;
      exit_info->exit_status = 0;
      sema_init (&exit_info->exited, 0);
      lock_init (&exit_info->refs_lock);

      /* Both parent and child have a pointer to this child struct. */
      exit_info->refs_cnt = 2;

      list_push_back (&thread_current ()->children, &exit_info->child_elem);
      t->exit_info = exit_info;

#ifdef VM
      hash_init (&t->spt, page_hash, page_less, NULL);
#endif
    }
  return true;
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
  {
    struct thread *highest_pri_ready = highest_priority_ready ();
    list_remove (&highest_pri_ready->elem);
    num_ready--;
    return highest_pri_ready;
  } 
}

/* Returns the highest priority thread in the ready list */
static struct thread *
highest_priority_ready (void)
{
  ASSERT ( intr_get_level () == INTR_OFF);

  return list_entry (list_max (&ready_list, 
              thread_compare_priority, NULL), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Puts the thread T to sleep because of call to timer_sleep () until
   timer ticks >= WAKE_TIME and it is awoken by thread_wake_sleeping ().
   WAKE_SEMA is initialized assigned to T's wake_sema field.
   T's wake_time is set to WAKE_TIME. */
void 
thread_timer_sleep (struct thread *t, struct semaphore *wake_sema,
                    int64_t wake_time)
{
  sema_init (wake_sema, 0);
  t->wake_sema = wake_sema;
  t->wake_time = wake_time;

  enum intr_level old_level = intr_disable ();
  list_insert_ordered (&sleeping_list, &(t->sleep_elem),
                       compare_wake_time, NULL);
  
  intr_set_level (old_level);
  sema_down (t->wake_sema);
  
  t->wake_time = 0;
  t->wake_sema = NULL;
}

/* Removes all threads from the sleeping list whose wake_time's are
less than TIME. */
void 
thread_wake_sleeping (int64_t time)
{
  struct list_elem *cur;
  struct thread *t;

  while (!list_empty (&sleeping_list))
    {
      cur = list_front (&sleeping_list);
      t = list_entry (cur, struct thread, sleep_elem);

      if (time < t->wake_time)
        {
          return;
        }
      
      list_remove (cur);
      sema_up (t->wake_sema);
    }
}

/* Find the max priority of the threads waiting on all the locks 
   held by the thread CUR */
int
thread_max_waiting_priority (struct thread * cur)
{
  int new_priority = cur->original_priority;
  struct list_elem *e;
  for (e = list_begin (&cur->locks_held); e != list_end (&cur->locks_held);
        e = list_next (e))
    {
      struct lock *lock_owned_by_current_thread = list_entry (e, struct lock, 
                                                              locks_held_elem);
      if (!list_empty (&lock_owned_by_current_thread->semaphore.waiters))
        {
          struct thread *highest_priority_waiter = list_entry (list_max (
                          &lock_owned_by_current_thread->semaphore.waiters, 
                          thread_compare_priority, NULL), struct thread, elem);
          
          new_priority = (new_priority) > (highest_priority_waiter->priority) ? 
                          (new_priority) : (highest_priority_waiter->priority);
        }
    }
  return new_priority;
}

/* Compares priorities of threads corresponding to elem A and B.
   Returns true if A is lower priority than B. */
bool
thread_compare_priority (const struct list_elem *a,
                         const struct list_elem *b,
                         void *aux UNUSED)
{
  struct thread *t1 = list_entry (a, struct thread, elem);
  struct thread *t2 = list_entry (b, struct thread, elem);
  return t1->priority < t2->priority;
}


/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);

/* Compares wake time of threads corresponding to elem A and B
   and returns true if thread A has an earlier wake time. */
static bool 
compare_wake_time (const struct list_elem *a,
                   const struct list_elem *b,
                   void *aux UNUSED)
{
  struct thread *t1 = list_entry (a, struct thread, sleep_elem);
  struct thread *t2 = list_entry (b, struct thread, sleep_elem);
  return t1->wake_time < t2->wake_time;
}

/* Recalculates recent cpu time for every thread */
static void
update_all_recent_cpu_times (void)
{
  thread_foreach (update_recent_cpu_time, NULL);
}

static void
update_all_priorities (void) {
  thread_foreach (update_mlfqs_priority, NULL);
}

/* Updates system load average according to this formula: 
   load_avg = (59/60)*load_avg + (1/60)*ready_threads. */
static void
update_system_load_avg (void)
{
  int ready_threads = num_ready;

  if (thread_current () != idle_thread) 
    ready_threads++;

  load_avg = fp_add (fp_mult (LOAD_WEIGHT, load_avg),
                     mult_fp_by_int (READY_WEIGHT, ready_threads));
}

/* Updates a thead's recent_cpu according to this formula:
   (2*load_avg)/(2*load_avg + 1) * recent_cpu + nice */
static void
update_recent_cpu_time (struct thread *t, void *aux UNUSED)
{
  fixed_point new_time;
  fixed_point double_load_avg;
  fixed_point coeff;
  fixed_point scaled_recent_cpu;

  double_load_avg = mult_fp_by_int(load_avg, 2);
  coeff = fp_div (double_load_avg, add_int_to_fp (double_load_avg, 1));
  scaled_recent_cpu = fp_mult (coeff, t->recent_cpu_time);
  new_time = add_int_to_fp (scaled_recent_cpu, t->niceness);

  /* Check if recent CPU time changed. */
  t->recent_cpu_changed = t->recent_cpu_time != new_time;
  t->recent_cpu_time = new_time;
}

/* Updates a thead's priority according to this formula:
   priority = PRI_MAX - (recent_cpu / 4) - (nice * 2) */
static void
update_mlfqs_priority (struct thread *t, void *aux UNUSED)
{

  if (t != idle_thread && t->recent_cpu_changed)
    {
      fixed_point unbounded_priority;
  
      unbounded_priority = int_to_fp (PRI_MAX);
      unbounded_priority = fp_sub (unbounded_priority,
                                  div_fp_by_int (t->recent_cpu_time, 4));
      unbounded_priority = sub_int_from_fp (unbounded_priority, 
                                            t->niceness * 2);
      t->priority = bound (fp_to_int (unbounded_priority),
                          PRI_MIN, PRI_MAX);

      t->recent_cpu_changed = false;
    }
}

/* Returns value of X bounded by LOWER and UPPER. */
static int
bound (int x, int lower, int upper)
{
  if (x < lower) return lower;
  if (x > upper) return upper;
  return x;
}