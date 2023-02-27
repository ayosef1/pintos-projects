#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "vm/mmap.h"

#define SYSCALL_ERROR -1
typedef int pid_t;
/* Coarse grain lock for filesystem access */
struct lock filesys_lock;

void syscall_init (void);
void exit (int status);
void munmap (mapid_t mapid);

#endif /* userprog/syscall.h */
