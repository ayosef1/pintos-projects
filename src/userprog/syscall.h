#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

typedef int pid_t;
/* Coarse grain lock for filesystem access */
struct lock filesys_lock;

void syscall_init (void);
void exit (int status);

#endif /* userprog/syscall.h */
