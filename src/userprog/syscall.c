#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/directory.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
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

static char *get_arg_string (void *esp, int pos, int limit);
static void *get_arg_buffer (void *esp, int pos, int size);
static int get_arg_int (void *esp, int pos);

static bool is_valid_memory (void *buffer, unsigned size);
static bool is_valid_address (const void *uaddr);
static bool is_valid_fd (int fd);

#define CMD_LINE_MAX 128        /* Maximum number of command line characters */

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

  if (!is_valid_memory (f->esp, sizeof(char *)))
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
      exit (-1);
  }
}

void
exit (int status)
{
  thread_current ()->exit_status = status;
  
  /* We clean everything up in process_exit since we might not 
     go through here. */

  thread_exit ();
}

void
sys_halt ()
{
  shutdown_power_off ();
}

void
sys_exit (uint32_t *esp)
{
  int status;
  status = get_arg_int (esp, 1);
  exit (status);
}

pid_t
sys_exec (uint32_t *esp)
{
  char *cmd_line;

  cmd_line = get_arg_string (esp, 1, CMD_LINE_MAX);
  if (cmd_line == NULL)
    return TID_ERROR;
  
  return process_execute (cmd_line);
}

int
sys_wait (uint32_t *esp)
{
  pid_t pid;

  pid = get_arg_int (esp, 1);

  return process_wait (pid);
}

bool
sys_create (uint32_t *esp)
{
  char *fname;
  off_t initial_size;
  bool ret = false;

  fname = get_arg_string (esp, 1, NAME_MAX);

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
sys_remove (uint32_t *esp)
{
  bool ret = false;
  char *fname = get_arg_string (esp, 1, NAME_MAX);

  if (fname != NULL)
  {
    lock_acquire (&filesys_lock);
    ret = filesys_remove (fname);
    lock_release (&filesys_lock);
  }
  return ret;
}

int
sys_open (uint32_t *esp)
{
  int ret = -1;
  char *fname = get_arg_string (esp, 1, NAME_MAX);
  struct thread *cur = thread_current ();

  if (fname == NULL)
    return ret;

  lock_acquire (&filesys_lock);
  struct file *fp = filesys_open (fname);
  lock_release (&filesys_lock);

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
  int size;
  
  fd = get_arg_int (esp, 1);

  if (!is_valid_fd (fd) || fd == STDIN_FILENO || fd == STDOUT_FILENO)
    exit (-1);
  
  struct file *file = thread_current ()->fdtable[fd];

	if (file == NULL)
		exit (-1);
  
  lock_acquire (&filesys_lock);
	size = file_length (file);
  lock_release (&filesys_lock);
  
  return size;
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

#define BUF_MAX 512;            /* Max bytes to write to console in one call */

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
      bytes_written = -1;
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
    bytes_written = file_write(fp, buffer, size);
    lock_release (&filesys_lock);
  }

  return bytes_written;
}

static void
sys_seek (uint32_t *esp)
{
  int fd;
  unsigned pos;
  struct thread *cur;

  fd = get_arg_int (esp, 1);
  pos = get_arg_int (esp, 2);
  cur = thread_current ();

  if (!is_valid_fd(fd) || cur->fdtable[fd] == NULL)
    exit (-1);
  
  file_seek (cur->fdtable[fd], pos);
}

static unsigned
sys_tell (uint32_t *esp)
{
  int fd;
  struct thread *cur;

  fd = get_arg_int (esp, 1);
  cur = thread_current ();

  if (cur->fdtable[fd] == NULL)
    exit (-1);

  return file_tell (cur->fdtable[fd]);
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

  /* Have previously closed the given file descriptor. */
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
  if (!is_valid_address (arg))
    exit (-1);

  return *(int *)arg;
}

static void *
get_arg_buffer (void *esp, int pos, int size)
{
  void **arg;

  arg = (void **)esp + pos;
  
  if (!is_valid_memory (arg, sizeof(char *)) || !is_valid_memory (*arg, size))
    exit (-1);

  return *(void **)arg;
}

static char *
get_arg_string (void *esp, int pos, int limit)
{
  char **str_ptr;
  char *cur;
  char *end;

  str_ptr = (char **)esp + pos;

  if (!is_valid_memory (str_ptr, sizeof (char *)))
    exit (-1);

  end = *str_ptr + limit + 1;

  
  for (cur = *str_ptr; cur < end; cur++)
    {
      if (!is_valid_address (cur))
        exit (-1);
      
      if (*cur == '\0')
        break;
      
    }
  
  /* Null indicates that filename
     is either empty or too long. */
  if (cur == *str_ptr || cur == end)
    *str_ptr = NULL;
  
  return *str_ptr;
}

static bool 
is_valid_memory (void *start, unsigned size)
{
  /* TODO: depending on operation, might need to double check permissions. */
  uint8_t *cur;
  uint8_t *end;
  unsigned start_offs;
  
  end = (uint8_t *)start + size;
  start_offs = pg_ofs (start);

  for (cur = start - start_offs; cur < end; cur += PGSIZE)
    {
      if (!is_valid_address (cur))
        return false;
    }
  return true;
}

/* Returns whether VADDR is a valid memory address. This means it is
   in user space and has been allocated in the page table */
static bool 
is_valid_address (const void *vaddr)
{
  return vaddr != NULL && is_user_vaddr (vaddr)
                       && pagedir_get_page (thread_current ()->pagedir, vaddr);
}

static bool
is_valid_fd (int fd) {
  return fd >= 0 && fd < MAX_FILES;
}
