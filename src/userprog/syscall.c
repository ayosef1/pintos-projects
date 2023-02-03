#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"

static void syscall_handler (struct intr_frame *);

static void sys_halt (void);
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

static void exit (int status);

static void *get_arg_buffer (void *esp, int pos, int size);
static int get_arg_int (void *esp, int pos);
static bool is_valid_address (const void *uaddr);
static bool is_valid_memory (void *buffer, unsigned size);

static struct lock filesystem_lock;

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&filesystem_lock);
}

static void
syscall_handler (struct intr_frame *f) 
{
  uint32_t syscall_num;

  if (!is_valid_address (f->esp))
    exit (-1);

  syscall_num = get_arg_int(f->esp, 0);
  // printf("syscall_num = %d\n", syscall_num);
  switch (syscall_num)
  {
    case SYS_HALT:                   /* Halt the operating system. */
      // printf ("%s\n", "about to halt");
      sys_halt ();
      break;
    case SYS_EXIT:                   /* Terminate this process. */
      // printf("%s\n", "about to exit");
      sys_exit (f->esp);
      break;
    case SYS_EXEC:                   /* Start another process. */
      // printf("%s\n", "about to exec");
      sys_exec (f->esp);
      break;
    case SYS_WAIT:                   /* Wait for a child process to die. */
      // printf("%s\n", "about to wait");
      sys_wait (f->esp);
      break;
    case SYS_CREATE:                 /* Create a file. */
      // printf("%s\n", "about to create");
      sys_create (f->esp);
      break;
    case SYS_REMOVE:                 /* Delete a file. */
      // printf("%s\n", "about to remove");
      sys_remove (f->esp);
      break;
    case SYS_OPEN:                   /* Open a file. */
      // printf("%s\n", "about to open");
      sys_open (f->esp);
      break;
    case SYS_FILESIZE:               /* Obtain a file's size. */
      // printf("%s\n", "about to filesize");
      sys_filesize (f->esp);
      break;
    case SYS_READ:                   /* Read from a file. */
      // printf("%s\n", "about to read");
      sys_read (f->esp);
      break;
    case SYS_WRITE:                  /* Write to a file. */
      // printf("%s\n", "about to write");
      f->eax = sys_write (f->esp);
      break;
    case SYS_SEEK:                   /* Change position in a file. */
      // printf("%s\n", "about to seek");
      sys_seek (f->esp);
      break;
    case SYS_TELL:                   /* Report current position in a file. */
      // printf("%s\n", "about to tell");
      sys_tell (f->esp);
      break;
    case SYS_CLOSE:                  /* Close a file. */
      // printf("%s\n", "about to close");
      sys_close (f->esp);
      break;
    default:
      exit(-1);
  }
}

void
exit (int status)
{
  struct thread *cur;

  cur = thread_current ();
  printf ("%s: exit(%d)\n", cur->name, status);
  /* Clean up all file descriptors. */
  thread_exit ();
}

void
sys_halt ()
{

}

void
sys_exit (uint32_t *esp)
{
  int status;
  status = get_arg_int (esp, 1);
  // printf("status is %d\n", status);
  exit (status);
}

pid_t
sys_exec (uint32_t *esp UNUSED)
{
  return 0;
}

int
sys_wait (uint32_t *esp UNUSED)
{
  return 0;
}

bool
sys_create (uint32_t *esp UNUSED)
{
  return false;
}

bool
sys_remove (uint32_t *esp UNUSED)
{
  return false;
}

int
sys_open (uint32_t *esp UNUSED)
{
  return 0;
}

int
sys_filesize (uint32_t *esp UNUSED)
{
  return 0;
}

int
sys_read (uint32_t *esp UNUSED)
{
  return 0;
}

int
sys_write (uint32_t *esp)
{
  int written;

  int fd;
  char *buffer;
  unsigned size;

  lock_acquire (&filesystem_lock);
  fd = get_arg_int (esp, 1);
  size = get_arg_int (esp, 3);
  buffer = get_arg_buffer (esp, 2, size);
  // printf("fd is %d\n", fd);
  // printf("size is %d\n", size);
  // printf("buffer is %s\n", (char *)buffer);

  /* only handle writing to console for now */
  if (fd == STDOUT_FILENO)
    {
      int remaining = size;
      while (remaining > 0)
        {
          putbuf (buffer, size);
          remaining -= 512;
        }
      written = size;
    }
  lock_release (&filesystem_lock);
  return written;
}

void
sys_seek (uint32_t *esp UNUSED)
{

}

unsigned
sys_tell (uint32_t *esp UNUSED)
{
  return 1;
}

void
sys_close (uint32_t *esp UNUSED)
{

}

int
get_arg_int (void *esp, int pos)
{
  uint32_t *arg;
  arg = (uint32_t *)esp + pos;
  if (!is_valid_memory (arg, sizeof(int)))
    exit (-1);

  return *(int *)arg;
}

void *
get_arg_buffer (void *esp, int pos, int size)
{
  uint32_t *arg;

  arg = (uint32_t *)esp + pos;
  if (!is_valid_memory (arg, size))
    exit (-1);
  return *(void **)arg;
}

bool 
is_valid_memory (void *start, unsigned size)
{
  /* depending on operation, might need to double check permissions. */
  uint8_t *cur;
  uint8_t *end;
  
  end = (uint8_t *)start + size;
  for (cur = start; cur < end; cur++)
  {
    if (!is_valid_address (cur) 
        || pagedir_get_page (thread_current ()->pagedir, cur) == NULL)
      return false;
  }
  return true;
}

bool 
is_valid_address (const void *vaddr)
{
  return vaddr != NULL && is_user_vaddr (vaddr);
}