#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"

static void syscall_handler (struct intr_frame *);
static bool is_valid_pointer (const void *uaddr);

static void sys_halt (uint32_t *esp);
static void sys_exit (uint32_t *esp);
static pid_t sys_exec (uint32_t *esp);
static int sys_wait (uint32_t *esp);
static bool sys_create (uint32_t *esp);
static bool sys_remove (uint32_t *esp);
static int sys_open (uint32_t *esp);
static int sys_filesize (uint32_t *esp);
static int sys_read (uint32_t *esp);
static int sys_write (uint32_t *esp);
static void sys_seek (uint32_t *esp);
static unsigned sys_tell (uint32_t *esp);
static void sys_close (uint32_t *esp);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{

  uint32_t *argv0;
  uint32_t syscall_num;

  argv0 = f->esp;

  if (!is_valid_pointer (argv0))
  {
    /* user program tried to give us an invalid pointer. */
    /* exit and clean up*/
  }

  syscall_num = *argv0;
  switch (syscall_num)
  {
    case SYS_HALT:                   /* Halt the operating system. */
      printf ("%s", "halt");
      sys_halt (f->esp);
      break;
    case SYS_EXIT:                   /* Terminate this process. */
      printf("%s", "exit");
      sys_exit (f->esp);
      break;
    case SYS_EXEC:                   /* Start another process. */
      printf("%s", "exec");
      sys_exec (f->esp);
      break;
    case SYS_WAIT:                   /* Wait for a child process to die. */
      printf("%s", "wait");
      sys_wait (f->esp);
      break;
    case SYS_CREATE:                 /* Create a file. */
      printf("%s", "create");
      sys_create (f->esp);
      break;
    case SYS_REMOVE:                 /* Delete a file. */
      printf("%s", "remove");
      sys_remove (f->esp);
      break;
    case SYS_OPEN:                   /* Open a file. */
      printf("%s", "open");
      sys_open (f->esp);
      break;
    case SYS_FILESIZE:               /* Obtain a file's size. */
      printf("%s", "filesize");
      sys_filesize (f->esp);
      break;
    case SYS_READ:                   /* Read from a file. */
      printf("%s", "read");
      sys_read (f->esp);
      break;
    case SYS_WRITE:                  /* Write to a file. */
      printf("%s", "write");
      sys_write (f->esp);
      break;
    case SYS_SEEK:                   /* Change position in a file. */
      printf("%s", "seek");
      sys_seek (f->esp);
      break;
    case SYS_TELL:                   /* Report current position in a file. */
      printf("%s", "tell");
      sys_tell (f->esp);
      break;
    case SYS_CLOSE:                  /* Close a file. */
      printf("%s", "close");
      sys_close (f->esp);
      break;
  }
  // printf ("system call!\n");
  // thread_exit ();
}

void
sys_halt (uint32_t *esp)
{
  /* 0 arguments */
  /* actually perform the sys call */
}

void
sys_exit (uint32_t *esp)
{
  /* 1 argument */
  uint32_t *status;

  status = esp + 1;
  if (!is_valid_pointer (status))
  {
    /* invalid pointers*/
    /* clean up and exit*/
  }
  /* actually perform the sys call */
}

pid_t
sys_exec (uint32_t *esp)
{
  /* 1 argument*/
  uint32_t *cmd_line;
  /* be very careful. cmd_line is supposed to be an array*/

  cmd_line = esp + 1;

  if (!is_valid_pointer (cmd_line))
  {
    /* invalid pointers*/
    /* clean up and exit*/
  }
  /* actually perform the sys call */
}

int
sys_wait (uint32_t *esp)
{
  /* 1 argument*/
  uint32_t *pid;

  pid = esp + 1;

  if (!is_valid_pointer (pid))
  {
    /* invalid pointers*/
    /* clean up and exit*/
  }
  /* actually perform the sys call */
}

bool
sys_create (uint32_t *esp)
{
  /* 2 argument */
  uint32_t *file;
  /* be very careful. file is an array of chars */
  uint32_t *initial_size;

  file = esp + 1;
  initial_size = esp + 2;

  if (!is_valid_pointer (file) || !is_valid_pointer (initial_size))
  {
    /* invalid pointers*/
    /* clean up and exit*/
  }
  /* actually perform the sys call */
}

bool
sys_remove (uint32_t *esp)
{
  /* 1 argument */
  uint32_t *file;
  /* be very careful. file is an array of chars */

  file = esp + 1;

  if (!is_valid_pointer (file))
  {
    /* invalid pointers*/
    /* clean up and exit*/
  }
  /* actually perform the sys call */
}

int
sys_open (uint32_t *esp)
{
  /* 1 argument */
  uint32_t *file;
  /* be very careful. file is an array of chars */

  file = esp + 1;

  if (!is_valid_pointer (file))
  {
    /* invalid pointers*/
    /* clean up and exit*/
  }
  /* actually perform the sys call */
}

int
sys_filesize (uint32_t *esp)
{
  uint32_t *fd;

  fd = esp + 1;

  if (!is_valid_pointer (fd))
  {
    /* invalid pointers*/
    /* clean up and exit*/
  }
  /* actually perform the sys call */
}

int
sys_read (uint32_t *esp)
{
  uint32_t *fd;
  uint32_t *buffer;
  uint32_t *size;

  fd = esp + 1;
  buffer = esp + 2;
  size = esp + 3;

  if (!is_valid_pointer (fd) 
      || !is_valid_pointer (buffer)
      || !is_valid_pointer (size) )
  {
    /* invalid pointers*/
    /* clean up and exit*/
  }
  /* actually perform the sys call */
}

int
sys_write (uint32_t *esp)
{
  uint32_t *fd;
  uint32_t *buffer;
  uint32_t *size;

  fd = esp + 1;
  buffer = esp + 2;
  size = esp + 3;

  if (!is_valid_pointer (fd) 
      || !is_valid_pointer (buffer)
      || !is_valid_pointer (size) )
  {
    /* invalid pointers*/
    /* clean up and exit*/
  }
  /* actually perform the sys call */
}

void
sys_seek (uint32_t *esp)
{
  uint32_t *fd;
  uint32_t *position;

  fd = esp + 1;
  position = esp + 2;

  if (!is_valid_pointer (fd) || !is_valid_pointer (position))
  {
    /* invalid pointers*/
    /* clean up and exit*/
  }
  /* actually perform the sys call */
}

unsigned
sys_tell (uint32_t *esp)
{
  /* 1 argument */
  uint32_t *fd;

  fd = esp + 1;

  if (!is_valid_pointer (fd))
  {
    /* invalid pointers*/
    /* clean up and exit*/
  }
  /* actually perform the sys call */
}

void
sys_close (uint32_t *esp)
{
  /* 1 argument */
  uint32_t *fd;

  fd = esp + 1;

  if (!is_valid_pointer (fd))
  {
    /* invalid pointers*/
    /* clean up and exit*/
  }
  /* actually perform the sys call */
}

/* The kernel must be very careful about doing so, because the user can pass a 
   null pointer, a pointer to unmapped virtual memory, or a pointer to kernel 
   virtual address space (above PHYS_BASE). All of these types of invalid 
   pointers must be rejected without harm to the kernel or other running 
   processes, by terminating the offending process and freeing its resources. */
bool 
is_valid_pointer (const void *vaddr)
{
  if (vaddr == NULL || !is_user_vaddr (vaddr))
    return false;
  return pagedir_get_page (thread_current ()->pagedir, vaddr) != NULL;
}