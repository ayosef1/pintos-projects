#include <stdio.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "filesys/directory.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "userprog/syscall.h"

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

static char *get_arg_fname (void *esp, int pos);
static void *get_arg_buffer (void *esp, int pos, int size);
static int get_arg_int (void *esp, int pos);
static bool is_valid_address (const void *uaddr);
static bool is_valid_memory (void *buffer, unsigned size);
static bool is_valid_fd (int fd);

static struct lock filesys_lock;

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&filesys_lock);
}

static void
syscall_handler (struct intr_frame *f) 
{
  uint32_t syscall_num;

  if (!is_valid_address (f->esp))
    exit (-1);

  syscall_num = get_arg_int(f->esp, 0);
  switch (syscall_num)
  {
    case SYS_HALT:                   /* Halt the operating system. */
      sys_halt ();
      break;
    case SYS_EXIT:                   /* Terminate this process. */
      sys_exit (f->esp);
      break;
    case SYS_EXEC:                   /* Start another process. */
      f->eax = sys_exec (f->esp);
      break;
    case SYS_WAIT:                   /* Wait for a child process to die. */
      f->eax = sys_wait (f->esp);
      break;
    case SYS_CREATE:                 /* Create a file. */
      f->eax = sys_create (f->esp);
      break;
    case SYS_REMOVE:                 /* Delete a file. */
      f->eax = sys_remove (f->esp);
      break;
    case SYS_OPEN:                   /* Open a file. */
      f->eax = sys_open (f->esp);
      break;
    case SYS_FILESIZE:               /* Obtain a file's size. */
      f->eax = sys_filesize (f->esp);
      break;
    case SYS_READ:                   /* Read from a file. */
      f->eax = sys_read (f->esp);
      break;
    case SYS_WRITE:                  /* Write to a file. */
      f->eax = sys_write (f->esp);
      break;
    case SYS_SEEK:                   /* Change position in a file. */
      sys_seek (f->esp);
      break;
    case SYS_TELL:                   /* Report current position in a file. */
      f->eax = sys_tell (f->esp);
      break;
    case SYS_CLOSE:                  /* Close a file. */
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

  /* TODO: Clean up all file descriptors. */

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
sys_create (uint32_t *esp)
{
  char *fname;
  off_t initial_size;
  bool ret = false;

  fname = get_arg_fname (esp, 1);

  if (fname != NULL)
  {
    initial_size = get_arg_int (esp, 2);
    lock_acquire (&filesys_lock);
    ret = filesys_create (fname, initial_size);
    lock_release (&filesys_lock);
  }
  return ret;
}

bool
sys_remove (uint32_t *esp UNUSED)
{
  return false;
}

int
sys_open (uint32_t *esp)
{
  char *fname = get_arg_fname (esp, 1);
  struct thread *cur;
  int ret = -1;

  if (fname == NULL)
    return ret;

  lock_acquire (&filesys_lock);
  struct file *fp = filesys_open (fname);
  lock_release (&filesys_lock);

  cur = thread_current ();

  /* File open unsuccessful or file limit hit */
  if (fp == NULL || cur->next_fd < 0)
    return ret;

  ret = cur->next_fd;
  cur->fdtable[ret] = fp;
  
  int new_fd = STDOUT_FILENO + 1;
  for (; new_fd < MAX_FILES; new_fd++) 
    {
      if (cur->fdtable[new_fd] == NULL)
      {
        cur->next_fd = new_fd;
        break;
      }
    }

  if (new_fd == MAX_FILES)
    cur->next_fd = -1;
  
  return ret;

}

int
sys_filesize (uint32_t *esp)
{
  int fd;
  
  fd = get_arg_int (esp, 1);

  if (!is_valid_fd (fd) || fd == STDIN_FILENO || fd == STDOUT_FILENO)
    exit (-1);
  
  struct file *file = thread_current ()->fdtable[fd];

	if (file == NULL)
		exit (-1);
  
	return file_length (file);
}

int
sys_read (uint32_t *esp)
{

  int fd;
  uint8_t *buffer;
  unsigned size;
  int bytes_read = 0;

  fd = get_arg_int (esp, 1);
  size = get_arg_int (esp, 3);
  buffer = (uint8_t *) get_arg_buffer (esp, 2, size);

  if (!is_valid_fd (fd) || fd == STDOUT_FILENO)
    {
      return -1;
    }
  else if (fd == STDIN_FILENO)
    {
      while (size--)
        {
          *buffer++ = input_getc ();
          bytes_read++;
        }
    }
  else
    {
        struct thread *cur = thread_current ();
        struct file *fp = cur->fdtable[fd];

        if (fp == NULL) {
          return -1;
        }

        lock_acquire (&filesys_lock);
        bytes_read = file_read(fp, buffer, size);
        lock_release (&filesys_lock);
    }
  
  return bytes_read;
}

#define BUF_MAX 512;

static int
sys_write (uint32_t *esp)
{
  int bytes_written = 0;

  int fd;
  char *buffer;
  unsigned size;

  fd = get_arg_int (esp, 1);
  size = get_arg_int (esp, 3);
  buffer = get_arg_buffer (esp, 2, size);

  if (!is_valid_fd (fd) || fd == STDIN_FILENO)
    {
      return -1;
    }
  else if (fd == STDOUT_FILENO)
    {
      int remaining = size;
      while (remaining > 0)
        {
          putbuf (buffer, size);
          remaining -= BUF_MAX;
        }
      bytes_written = size;
    }
  else 
  {
    struct thread *cur = thread_current ();
    struct file *fp = cur->fdtable[fd];

    if (fp == NULL) 
      return -1;

    lock_acquire (&filesys_lock);
    bytes_written = file_write (fp, buffer, size);
    lock_release (&filesys_lock);
  }
  return bytes_written;
}

static void
sys_seek (uint32_t *esp UNUSED)
{

}

static unsigned
sys_tell (uint32_t *esp UNUSED)
{
  return 1;
}

static void
sys_close (uint32_t *esp)
{
  int fd;

  fd = get_arg_int (esp, 1);

  if (fd >= MAX_FILES || fd == STDIN_FILENO || fd == STDOUT_FILENO)
    return;
  
  struct thread *cur = thread_current ();
  struct file *fp = cur->fdtable[fd];

  if (fp == NULL)
    return;
  
  lock_acquire (&filesys_lock);
  file_close (fp);
  lock_release (&filesys_lock);

  cur->fdtable[fd] = NULL;

  if (fd < cur->next_fd)
    cur->next_fd = fd;

}

static int
get_arg_int (void *esp, int pos)
{
  uint32_t *arg;
  arg = (uint32_t *)esp + pos;
  if (!is_valid_memory (arg, sizeof (int)))
    exit (-1);

  return *(int *)arg;
}

static void *
get_arg_buffer (void *esp, int pos, int size)
{
  void **arg;

  arg = (void **)esp + pos;
  
  if (!is_valid_address (arg) || !is_valid_memory (*arg, size))
    exit (-1);

  return *(void **)arg;
}

static char *
get_arg_fname (void *esp, int pos)
{
  char **fname_ptr;
  char *cur;
  char *end;

  fname_ptr = (char **)esp + pos;

  if (!is_valid_address(fname_ptr))
    exit(-1);

  end = *fname_ptr + NAME_MAX + 1;

  
  for (cur = *fname_ptr; cur < end; cur++)
    {
      if (!is_valid_address (cur) 
          || pagedir_get_page (thread_current ()->pagedir, cur) == NULL)
        exit(-1);
      
      if (*cur == '\0')
        break;
      
    }
  
  /* Null indicates that filename
     was either empty or too long. */
  if (cur == *fname_ptr || cur == end)
    *fname_ptr = NULL;
  
  return *fname_ptr;
}

static bool 
is_valid_memory (void *start, unsigned size)
{
  /* TODO: depending on operation, might need to double check permissions. */
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

static bool 
is_valid_address (const void *vaddr)
{
  return vaddr != NULL && is_user_vaddr (vaddr);
}

static bool
is_valid_fd (int fd) {
  return fd >= 0 && fd < MAX_FILES;
}