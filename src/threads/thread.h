#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <hash.h>
#include <stdint.h>
#include "threads/fixed-point.h"
#include "threads/synch.h"


/* States in a thread's life cycle. */
enum thread_status
  {
    THREAD_RUNNING,     /* Running thread. */
    THREAD_READY,       /* Not running but ready to run. */
    THREAD_BLOCKED,     /* Waiting for an event to trigger. */
    THREAD_DYING        /* About to be destroyed. */
  };

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

#define NICE_MIN -20                    /* Lowest nice value. */
#define NICE_INITIAL 0                  /* Initial thread's nice value. */
#define NICE_MAX 20                     /* Highest nice value. */

#define RECENT_CPU_TIME_INITIAL 0       /* Initial thread's recent cpu time. */

/* Size of the file descriptor table and therefore limit on
   number of files a process can open */
#define MAX_FILES 128

/* Reserved file descriptor should never allocate */
#define RESERVED_FD 0
/* Reserved file descriptor for process executable */
#define EXEC_FD 1

/* The shared child exit information used to synchronize exiting between
   child and parent as well as communicate the child exit status to the
   parent. The parent stores it as an element in its `children` list. The
   child stores a pointer to it in its member `exit_info`.
   
   The 'tid' is the child's TID, used by parent in wait to find correct child.
   When a child exits, it stores its exit status in member 'exit_status' and 
   then signals its potentially waiting parent through 'exited' that it can 
   read the 'exit_status' has been updated. 
   
   The 'refs_lock' and 'refs_cnt' are useful for freeing this shared data
   structure. Either both or one of the parent and child can have a pointer to
   the struct at a given time. Using 'refs_cnt', the procress currently
   accessing this shared memory (either the child or parent) knows if it is the
   last to do so in which case it would eventually free the shared data
   structure. 
   
   Finally, the 'elem' member allows a parent process to keep track of its
   child processes by maintaining a list of child_exit_info structs. */
struct child_exit_info
    {
        tid_t tid;                              /* Child's tid. */
        int exit_status;                        /* Child's exit status. */
        struct semaphore exited;                /* Sync for waiting parent to
                                                   get exit status of child. */
        int refs_cnt;                           /* Number of references to the
                                                   the struct. */   
        struct lock refs_lock;                  /* Lock to access refs_cnt. */
        struct list_elem child_elem;            /* List element for parent's
                                                   children list. */
    };
    
/* A kernel thread or user process.

   Each thread structure is stored in its own 4 kB page.  The
   thread structure itself sits at the very bottom of the page
   (at offset 0).  The rest of the page is reserved for the
   thread's kernel stack, which grows downward from the top of
   the page (at offset 4 kB).  Here's an illustration:

        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             +---------------------------------+
             |              magic              |
             |                :                |
             |                :                |
             |               name              |
             |              status             |
        0 kB +---------------------------------+

   The upshot of this is twofold:

      1. First, `struct thread' must not be allowed to grow too
         big.  If it does, then there will not be enough room for
         the kernel stack.  Our base `struct thread' is only a
         few bytes in size.  It probably should stay well under 1
         kB.

      2. Second, kernel stacks must not be allowed to grow too
         large.  If a stack overflows, it will corrupt the thread
         state.  Thus, kernel functions should not allocate large
         structures or arrays as non-static local variables.  Use
         dynamic allocation with malloc() or palloc_get_page()
         instead.

   The first symptom of either of these problems will probably be
   an assertion failure in thread_current(), which checks that
   the `magic' member of the running thread's `struct thread' is
   set to THREAD_MAGIC.  Stack overflow will normally change this
   value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
   the run queue (thread.c), or it can be an element in a
   semaphore wait list (synch.c).  It can be used these two ways
   only because they are mutually exclusive: only a thread in the
   ready state is on the run queue, whereas only a thread in the
   blocked state is on a semaphore wait list. */

struct thread
  {
   /* Owned by thread.c. */
   tid_t tid;                          /* Thread identifier. */
   enum thread_status status;          /* Thread state. */
   char name[16];                      /* Name (for debugging purposes). */
   uint8_t *stack;                     /* Saved stack pointer. */
   int priority;                       /* Effective Priority. */
   int original_priority;              /* Non-donated Priority. */
   int niceness;                       /* Nice value. */
   int64_t wake_time;                  /* Time at which thread should wake
                                          after being put to sleep. */
   bool recent_cpu_changed;            /* Had recent_cpu change since last
                                          priority change */
   fixed_point recent_cpu_time;        /* Exponentially weighted moving 
                                          average of recent CPU time. */
   struct semaphore *wake_sema;        /* Used to indicate sleeping thread 
                                          should wake up. */
   struct list_elem sleep_elem;        /* List element for sleeping threads
                                          list. */
   struct list_elem allelem;           /* List element for all threads list. */

   /* Shared between thread.c and synch.c. */
   struct list_elem elem;              /* List element. */

   struct list locks_held;             /* List of locks held by this thread. */
   struct lock *waiting_lock;          /* Lock we are waiting for (if any). */

#ifdef USERPROG
   /* Owned by userprog/process.c. */
   uint32_t *pagedir;                  /* Page directory. */
   int next_fd;                        /* Smallest available fd. */
   int exit_status;                    /* Exit status of thread. */
   struct list children;               /* List of children's exit 
                                          information. */
   struct file *fdtable[MAX_FILES];    /* File Descriptor Table. */
   struct child_exit_info *exit_info;  /* Thread's exit information shared with
                                          parent. */
   struct hash spt;                    /* Supplmentary Page Table*/
   struct hash mmap_table;             /* Memory map table. */
#endif

#ifdef VM
   bool in_syscall;
#endif

    /* Owned by thread.c. */
    unsigned magic;                     /* Detects stack overflow. */
  };

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

/* Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func (struct thread *t, void *aux);
void thread_foreach (thread_action_func *, void *);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int nice);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

int64_t thread_get_next_wakeup (void);
void thread_set_next_wakeup (int64_t);

void thread_timer_sleep (struct thread *t, struct semaphore *wake_sema,
                   int64_t wake_time);
void thread_wake_sleeping (int64_t);
int thread_max_waiting_priority (struct thread *);
bool thread_compare_priority (const struct list_elem *a,
                          const struct list_elem *b,
                          void *aux UNUSED);

void thread_update_next_fd (struct thread *t);

#endif /* threads/thread.h */
