#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

#define WORD_SIZE sizeof (void *)

/* Struct to be passed into start_process that contains the already
   parsed arguments and pointer to remaining arguments */
struct process_arg 
    {
        char *exec_name;                /* Name of executable */
        char *save_ptr;                 /* Save ptr from tokenizing exec */
        char *page;                     /* Page on which cmd_line storage */
        bool loaded;                    /* Child loaded successfully */
        struct semaphore loaded_sema;   /* Ensure parent waits for child 
                                           to load. */
    };

struct child_process
    {
        tid_t tid;                              /* Child's tid. */
        int exit_status;                        /* Child's exit status. */
        struct semaphore exit_status_ready;     /* Sync for waiting parent to
                                                   get exit status of child. */
        struct list_elem child_elem;            /* List element for per thread
                                                   children list. */
    };
tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /* userprog/process.h */
