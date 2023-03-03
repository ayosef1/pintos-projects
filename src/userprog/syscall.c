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
static mapid_t sys_mmap (uint32_t *esp);
static void sys_munmap (uint32_t *esp);

static char *get_arg_string (void *esp, int pos, int limit);
static void *get_arg_buffer (void *esp, int pos, int size);
static int get_arg_int (void *esp, int pos);

static bool is_valid_memory (void *buffer, unsigned size);
static bool is_valid_address (void *uaddr);
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

  if (!is_valid_address (f->esp))
    exit (SYSCALL_ERROR);
  
  thread_current ()->saved_user_esp = f->esp;

  syscall_num = get_arg_int(f->esp, 0);
  switch (syscall_num)
  {
    case SYS_HALT:
      sys_halt ();
      break;
    case SYS_EXIT:
      sys_exit (f->esp);
      break;
    case SYS_EXEC:
      f->eax = sys_exec (f->esp);
      break;
    case SYS_WAIT:
      f->eax = sys_wait (f->esp);
      break;
    case SYS_CREATE:
      f->eax = sys_create (f->esp);
      break;
    case SYS_REMOVE:
      f->eax = sys_remove (f->esp);
      break;
    case SYS_OPEN:
      f->eax = sys_open (f->esp);
      break;
    case SYS_FILESIZE:
      f->eax = sys_filesize (f->esp);
      break;
    case SYS_READ:
      f->eax = sys_read (f->esp);
      break;
    case SYS_WRITE:
      f->eax = sys_write (f->esp);
      break;
    case SYS_SEEK:
      sys_seek (f->esp);
      break;
    case SYS_TELL:
      f->eax = sys_tell (f->esp);
      break;
    case SYS_CLOSE:
      sys_close (f->esp);
      break;
#ifdef VM
    case SYS_MMAP:
      f->eax = sys_mmap (f->esp);
      break;
    case SYS_MUNMAP:
      sys_munmap (f->esp);
      break;
#endif
    default:
      exit (SYSCALL_ERROR);
  }
}

void
exit (int status)
{
  thread_current ()->exit_status = status;
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
  bool ret;
  char *fname;
  off_t initial_size;

  fname = get_arg_string (esp, 1, NAME_MAX);
  if (fname == NULL)
    return false;

  initial_size = get_arg_int (esp, 2);
  lock_acquire (&filesys_lock);
  ret = filesys_create (fname, initial_size);
  lock_release (&filesys_lock);
  return ret;
}

bool
sys_remove (uint32_t *esp)
{
  bool ret;
  char *fname;
  
  fname = get_arg_string (esp, 1, NAME_MAX);
  if (fname == NULL)
    return false;

  lock_acquire (&filesys_lock);
  ret = filesys_remove (fname);
  lock_release (&filesys_lock);
  return ret;
}

int
sys_open (uint32_t *esp)
{
  int ret;
  char *fname;
  struct thread *cur;

  fname = get_arg_string (esp, 1, NAME_MAX);
  if (fname == NULL)
    return SYSCALL_ERROR;

  lock_acquire (&filesys_lock);
  struct file *fp = filesys_open (fname);
  lock_release (&filesys_lock);

  cur = thread_current ();
  /* File open unsuccessful or file limit hit */
  if (fp == NULL)
    return SYSCALL_ERROR;
  
  if (cur->next_fd < 0)
    {
      file_close (fp);
      return SYSCALL_ERROR;
    }


  ret = cur->next_fd;
  cur->fdtable[ret] = fp;
  
  /* Find next available fd */
  int new_fd = EXEC_FD + 1;
  for (; new_fd < MAX_FILES; new_fd++) 
    {
      if (cur->fdtable[new_fd] == NULL)
      {
        cur->next_fd = new_fd;
        return ret;
      }
    }

  cur->next_fd = SYSCALL_ERROR;
  return ret;

}

int
sys_filesize (uint32_t *esp)
{
  int fd;
  int size;
  
  fd = get_arg_int (esp, 1);

  if (!is_valid_fd (fd) || fd == STDIN_FILENO || fd == STDOUT_FILENO)
    exit (SYSCALL_ERROR);
  
  struct file *file = thread_current ()->fdtable[fd];
	if (file == NULL)
		exit (SYSCALL_ERROR);
  
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
      return SYSCALL_ERROR;
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

        if (fp == NULL)
          return SYSCALL_ERROR;

        lock_acquire (&filesys_lock);
        bytes_read = file_read(fp, buffer, size);
        lock_release (&filesys_lock);
    }
  
  return bytes_read;
}

#define BUF_MAX 512           /* Max bytes to write to console in one call */

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
      bytes_written = SYSCALL_ERROR;
    }
  else if (fd == STDOUT_FILENO)
    {
      int remaining = size;
      while (remaining > 0)
        {
          int to_write = remaining > BUF_MAX ? BUF_MAX : remaining;
          putbuf (buffer, to_write);
          remaining -= to_write;
        }
      bytes_written = size;
    }
  else 
    {
      struct thread *cur = thread_current ();
      struct file *fp = cur->fdtable[fd];

      if (fp == NULL) 
        return SYSCALL_ERROR;

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
    exit (SYSCALL_ERROR);
  
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
    exit (SYSCALL_ERROR);

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

#ifdef VM
static mapid_t
sys_mmap (uint32_t *esp)
{
  int fd = get_arg_int (esp, 1);
  void *addr = get_arg_buffer (esp, 2, 0);
  mapid_t ret = SYSCALL_ERROR;
  struct thread *cur = thread_current ();
  
  struct file *fp;
  off_t file_len;
  int pg_cnt;

  /* Any issues with file descriptors. */
  if (!is_valid_fd (fd) || fd == STDIN_FILENO || fd == STDOUT_FILENO || 
      cur->next_fd == SYSCALL_ERROR)
    goto done;
  
  cur = thread_current ();
  fp = cur->fdtable[fd];
  if (fp == NULL)
    goto done;
  
  /* Issue with the address passed int. */
  if (!is_user_vaddr (addr) || addr == NULL ||  pg_ofs (addr) != 0)
    goto done;

  lock_acquire (&filesys_lock);
  file_len = file_length (fp);
  lock_release (&filesys_lock);

  if (file_len == 0 || !is_user_vaddr (addr + file_len))
    goto done;

  pg_cnt = (pg_round_up (addr + file_len) - addr) / PGSIZE;
  for (int pg = 0; pg < pg_cnt; pg++ )
    {
      /* Check not already mapped. */
      /* TODO: Change this to pagedir_get_spt when implemented
         correctly. */
      if (pagedir_get_spte (cur->pagedir, addr, false) != NULL)
        /* Should you release the lock? */
        goto done;
    }

  lock_acquire (&filesys_lock);
  fp = file_reopen (fp);
  lock_release (&filesys_lock);
  
  if (fp == NULL)
    goto done;
  
  if (!spt_try_add_mmap_pages (addr, fp, pg_cnt, file_len % PGSIZE))
    {
      goto done;
    }

  ret = mmap_insert (addr, pg_cnt);
  if (ret == -1)
    spt_remove_mmap_pages (addr, pg_cnt);

  done:
    return ret;
}

void 
sys_munmap (uint32_t *esp)
{
  mapid_t mapid = get_arg_int (esp, 1);
  struct mmap_table_entry *entry = mmap_find (mapid);
  if (entry == NULL)
    return;
  spt_remove_mmap_pages (entry->begin_upage, entry->pg_cnt);
  mmap_remove (mapid);
}
#endif

/* Returns the int at position POS on stack pointed at
   by ESP. Exits is any of int bytes are in invalid
   memory. */
static int
get_arg_int (void *esp, int pos)
{
  uint32_t *arg;
  arg = (uint32_t *)esp + pos;
  if (!is_valid_memory (arg, sizeof (int)))
    exit (SYSCALL_ERROR);

  return *(int *)arg;
}

/* Returns the buffer at position POS on stack pointed at
   by ESP, validating SIZE buffer bytes. Exits is any of 
   buffer pointer bytes or buffer bytes are in invalid
   memory. */
static void *
get_arg_buffer (void *esp, int pos, int size)
{
  void **arg;

  arg = (void **)esp + pos;
  
  if (!is_valid_memory (arg, sizeof(char *)) || !is_valid_memory (*arg, size))
    exit (SYSCALL_ERROR);

  return *(void **)arg;
}

/* Returns the argument string at position POS on stack pointed at
   by ESP, reading at most LIMIT bytes. Exits if any of the bytes
   of the char * or actually string bytes are in invalid memory. Returns
   NULL if its an empty string or the string is larger than LIMIT */
static char *
get_arg_string (void *esp, int pos, int limit)
{
  char **str_ptr;
  char *cur;
  char *end;

  str_ptr = (char **)esp + pos;

  /* Check the bytes of the char * are all in valid memory */
  if (!is_valid_memory (str_ptr, sizeof (char *)))
    exit (SYSCALL_ERROR);

  end = *str_ptr + limit + 1;

  
  for (cur = *str_ptr; cur < end; cur++)
    {
      if (!is_valid_address (cur))
        exit (SYSCALL_ERROR);
      
      if (*cur == '\0')
        break;
      
    }
  
  /* Either empty string of greater than LIMIT */
  if (cur == *str_ptr || cur == end)
    return NULL;
  
  return *str_ptr;
}

/* Returns whether bytes starting at START are in valid user space */
static bool 
is_valid_memory (void *start, unsigned size)
{
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
is_valid_address (void *vaddr)
{
  return vaddr != NULL && is_user_vaddr (vaddr);
}

/* Returns whether FD is between 0 and MAX_FILES */
static bool
is_valid_fd (int fd) 
{
  return fd >= 0 && fd < MAX_FILES;
}
