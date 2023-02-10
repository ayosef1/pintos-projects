#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

#define WORD_SIZE sizeof (void *)       /* Word size for use by stack setup. */

/* Argument passed into the thread function start_process when a child
   thread is created in process_execute (). Process execute does initial
   parsing so stores executable name EXEC_NAME as well as pointer SAVE_PTR
   to rest of the command line.
   
   PAGE is a pointer to the temporary buffer on which command line is stored
   so child process can free it.
   
   LOADED used to return load status of child back to parent and LOADED_SEMA
   to synchronize parent return from process_execute () and child completing
   load. */
struct process_arg 
    {
        char *exec_name;                /* Name of executable. */
        char *save_ptr;                 /* Save ptr from tokenizing cmd line. */
        char *page;                     /* Page on which cmd_line stored. */
        bool loaded;                    /* Whether child load successful. */
        struct semaphore loaded_sema;   /* Ensure parent waits for child to
                                           load. */
    };

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /* userprog/process.h */
