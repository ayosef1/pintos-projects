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

static void *get_arg_buffer (uint32_t *esp, int pos, int size);
static int get_arg_int (uint32_t *esp, int pos);
static bool is_valid_address (const void *uaddr);
static bool is_valid_buffer (void *buffer, unsigned size);

static struct lock filesystem_lock;

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&filesystem_lock);
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  
  uint32_t *esp;
  uint32_t syscall_num;
  
  esp = f->esp;
  syscall_num = get_arg_int (esp, 0);

  switch (syscall_num)
  {
    case SYS_HALT:                   /* Halt the operating system. */
      printf ("%s", "halt");
      sys_halt ();
      break;
    case SYS_EXIT:                   /* Terminate this process. */
      printf("%s", "exit");
      sys_exit (esp);
      break;
    case SYS_EXEC:                   /* Start another process. */
      printf("%s", "exec");
      sys_exec (esp);
      break;
    case SYS_WAIT:                   /* Wait for a child process to die. */
      printf("%s", "wait");
      sys_wait (esp);
      break;
    case SYS_CREATE:                 /* Create a file. */
      printf("%s", "create");
      sys_create (esp);
      break;
    case SYS_REMOVE:                 /* Delete a file. */
      printf("%s", "remove");
      sys_remove (esp);
      break;
    case SYS_OPEN:                   /* Open a file. */
      printf("%s", "open");
      sys_open (esp);
      break;
    case SYS_FILESIZE:               /* Obtain a file's size. */
      printf("%s", "filesize");
      sys_filesize (esp);
      break;
    case SYS_READ:                   /* Read from a file. */
      printf("%s", "read");
      sys_read (esp);
      break;
    case SYS_WRITE:                  /* Write to a file. */
      printf("%s", "write");
      f->eax = sys_write (esp);
      break;
    case SYS_SEEK:                   /* Change position in a file. */
      printf("%s", "seek");
      sys_seek (esp);
      break;
    case SYS_TELL:                   /* Report current position in a file. */
      printf("%s", "tell");
      sys_tell (esp);
      break;
    case SYS_CLOSE:                  /* Close a file. */
      printf("%s", "close");
      sys_close (esp);
      break;
  }
}

void
exit (int status)
{
  struct thread *cur;

  cur = thread_current ();
  printf ("%s: exit(%d)\n", cur->name, status);
  thread_exit ();
}

void
sys_halt ()
{

}

void
sys_exit (uint32_t *esp UNUSED)
{
  int status;

  status = get_arg_int (esp, 1);
  exit(status);
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
  int fd;
  const void *buffer;
  unsigned size;

  lock_acquire (&filesystem_lock);
  fd = get_arg_int (esp, 1);
  size = get_arg_int (esp, 3);
  buffer = get_arg_buffer (esp, 2, size);

  /* only handle writing to console for now */
  if (fd == STDOUT_FILENO)
    {
      int remaining = size;
      while (remaining > 0)
        {
          putbuf (buffer, size);
          remaining -= 512;
        }
    }
  lock_release (&filesystem_lock);
  return size;
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

/* The kernel must be very careful about doing so, because the user can pass a 
   null pointer, a pointer to unmapped virtual memory, or a pointer to kernel 
   virtual address space (above PHYS_BASE). All of these types of invalid 
   pointers must be rejected without harm to the kernel or other running 
   processes, by terminating the offending process and freeing its resources. */
bool 
is_valid_address (const void *vaddr)
{
  if (vaddr == NULL || !is_user_vaddr (vaddr))
    return false;
  return pagedir_get_page (thread_current ()->pagedir, vaddr) != NULL;
}

bool 
is_valid_buffer (void *buffer, unsigned size)
{
  /* depending on operation, might need to double check permissions. */
  uint8_t *cur;
  uint8_t *end;
  
  end = (uint8_t *)buffer + size;
  for (cur = buffer; cur < end; cur++)
  {
    if (!is_valid_address (cur))
      return false;
  }
  return true;
}

void *
get_arg_buffer (uint32_t *esp, int pos, int size)
{
  uint32_t *arg;
  void *buffer;

  arg = esp + pos;
  if (!is_valid_address (arg))
    exit (-1);
  buffer = *(void **)arg;
  if (!is_valid_buffer (buffer, size))
    exit (-1);
  return buffer;
}

int
get_arg_int (uint32_t *esp, int pos)
{
  uint32_t *arg;

  arg = esp + pos;
  if (!is_valid_buffer (arg, sizeof(int)))
    exit (-1);
  return *(int *)arg;
}
