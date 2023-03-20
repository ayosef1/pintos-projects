#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/directory.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "filesys/inode.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
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
static bool sys_chdir (uint32_t *esp);
static bool sys_mkdir (uint32_t *esp);
static bool sys_readdir (uint32_t *esp);
static bool sys_isdir (uint32_t *esp);
static int sys_inumber (uint32_t *esp);

static void exit (int status);

static char *get_arg_path (void *esp, int pos);
static char *get_arg_string (void *esp, int pos, int limit);
static void *get_arg_buffer (void *esp, int pos, int size);
static int get_arg_int (void *esp, int pos);

static bool is_valid_memory (void *buffer, unsigned size);
static bool is_valid_address (void *uaddr);

static bool is_valid_path (char *path);
static bool is_valid_fd (int fd);
static bool is_valid_file (struct fdt_entry *fdt_entry);

#define CMD_LINE_MAX 128        /* Maximum number of command line characters */

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
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
    case SYS_CHDIR:
      f->eax = sys_chdir (f->esp);
      break;
    case SYS_MKDIR:
      f->eax = sys_mkdir (f->esp);
      break;
    case SYS_READDIR:
      f->eax = sys_readdir (f->esp);
      break;
    case SYS_ISDIR:
      f->eax = sys_isdir (f->esp);
      break;
    case SYS_INUMBER:
      f->eax = sys_inumber (f->esp);
      break;
    default:
      exit (-1);
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
  char *fname;
  off_t initial_size;

  fname = get_arg_path (esp, 1);
  if (fname == NULL)
    return false;

  initial_size = get_arg_int (esp, 2);
  return filesys_create (fname, initial_size, IS_FILE);
}

bool
sys_remove (uint32_t *esp)
{
  char *fname;

  fname = get_arg_path (esp, 1);
  if (fname == NULL)
    return false;

  return filesys_remove (fname);
}

int
sys_open (uint32_t *esp)
{
  char *fname;
  struct thread *cur;

  fname = get_arg_path (esp, 1);
  if (fname == NULL)
    return -1;

  cur = thread_current ();
  
  /* No available FDs. */
  int fd = cur->next_fd;
  if (fd < 0)
    return -1;

  if (!filesys_open (fname, cur->fdtable + fd))
    return -1;
  
  thread_update_next_fd (cur);

  return fd;

}

int
sys_filesize (uint32_t *esp)
{
  int fd;
  int size;
  
  fd = get_arg_int (esp, 1);
  if (!is_valid_fd (fd) || fd == STDIN_FILENO || fd == STDOUT_FILENO)
    exit (-1);
  
  struct fdt_entry *fdt_entry = thread_current ()->fdtable + fd;
  if (!is_valid_file (fdt_entry))
    exit (-1);
  
	size = file_length (fdt_entry->fp.file);
  
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
      struct fdt_entry *fdt_entry = thread_current ()->fdtable + fd;
      if (!is_valid_file (fdt_entry))
        return -1;

      bytes_read = file_read(fdt_entry->fp.file, buffer,
                              size);

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
      bytes_written = -1;
    }
  else if (fd == STDOUT_FILENO)
    {
      int remaining = size;
      while (remaining > 0)
        {
          int to_write = remaining > BUF_MAX ? BUF_MAX : remaining;
          putbuf (buffer, to_write);
          remaining -= to_write;
          buffer += to_write;
        }
      bytes_written = size;
    }
  else 
    {
      struct fdt_entry *fdt_entry =  thread_current ()->fdtable + fd;
      if (!is_valid_file (fdt_entry))
        return -1;
      bytes_written = file_write(fdt_entry->fp.file, buffer,
                                 size);
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

  struct fdt_entry *fdt_entry = cur->fdtable + fd;
  if (!is_valid_fd(fd) || !is_valid_file (fdt_entry))
    exit (-1);
  
  file_seek (fdt_entry->fp.file, pos);
}

static unsigned
sys_tell (uint32_t *esp)
{
  int fd;
  struct thread *cur;

  fd = get_arg_int (esp, 1);
  cur = thread_current ();

  struct fdt_entry *fdt_entry = cur->fdtable + fd;
  if (!is_valid_fd(fd) || !is_valid_file (fdt_entry))
    exit (-1);

  return file_tell (fdt_entry->fp.file);
}

static void
sys_close (uint32_t *esp)
{
  int fd;

  fd = get_arg_int (esp, 1);

  if (fd >= MAX_FILES || fd == STDIN_FILENO || fd == STDOUT_FILENO)
    return;
  
  struct thread *cur = thread_current ();
  thread_close_fd (cur, fd);
  
}

static bool
sys_chdir (uint32_t *esp)
{
  char *dirpath;
  struct dir *dir;

  dirpath = get_arg_path (esp, 1);
  if (dirpath == NULL)
    return false;

  dir = dir_pathname_lookup (dirpath);

  if (dir == NULL)
    return false;

  dir_close (thread_current ()->cwd);
  thread_current ()->cwd = dir;
  return true;
}

static bool
sys_mkdir (uint32_t *esp)
{
  char *dir;

  dir = get_arg_path (esp, 1);
  if (dir == NULL)
    return false;

  return filesys_create (dir, 0, IS_DIR);
}

static bool
sys_readdir (uint32_t *esp)
{
  int fd;
  char *buffer;
  struct fdt_entry *fdt_entry;

  fd = get_arg_int (esp, 1);
  if (!is_valid_fd (fd))
    return false;

  fdt_entry = thread_current ()->fdtable + fd;
  if (fdt_entry->type != DIR || fdt_entry->fp.dir == NULL)
    return false;
  
  buffer = get_arg_buffer (esp, 2, NAME_MAX + 1);
  bool result = dir_readdir (fdt_entry->fp.dir, buffer);
  
  return result;
}

static bool
sys_isdir (uint32_t *esp)
{
  int fd;

  fd = get_arg_int (esp, 1);

  struct fdt_entry *fdt_entry = thread_current ()->fdtable + fd;
  if (fdt_entry->fp.file == NULL)
    exit (-1);

  return fdt_entry->type == DIR;
}

static int
sys_inumber (uint32_t *esp)
{
  int fd;
  struct inode *inode;

  fd = get_arg_int (esp, 1);

  struct fdt_entry *fdt_entry = thread_current ()->fdtable + fd;
  if (fdt_entry->fp.file == NULL)
    return false;
  
  if (fdt_entry->type == DIR)
    inode = dir_get_inode (fdt_entry->fp.dir);
  else  
    inode = file_get_inode (fdt_entry->fp.file);
  
  if (inode == NULL)
    return false;
  
  return inode_get_inumber (inode);
}

/* Returns the int at position POS on stack pointed at
   by ESP. Exits is any of int bytes are in invalid
   memory. */
static int
get_arg_int (void *esp, int pos)
{
  uint32_t *arg;
  arg = (uint32_t *)esp + pos;
  if (!is_valid_memory (arg, sizeof (int)))
    exit (-1);

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
    exit (-1);

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

  /* Check the bytes of the char * are all in valid memory. */
  if (!is_valid_memory (str_ptr, sizeof (char *))
      || !is_valid_address(*str_ptr))
    exit (-1);

  /* Empty string. */
  if (**str_ptr == '\0')
    return NULL;

  end = *str_ptr + limit + 1;
  for (cur = *str_ptr; cur < end; cur++)
    {
      if (pg_ofs (cur) == 0 && !is_valid_address (cur))
        exit (-1);
      
      if (*cur == '\0')
        break;
    }
  
  /* String of longer than LIMIT */
  if (cur == end)
    return NULL;
  
  return *str_ptr;
}

/* Returns path string at position POS on stack pointed at by ESP.
   Paths can be arbitrarily long so it first extracts the path then
   returns path if valid. Path is invalid if any of the string bytes 
   are in invalid memory or any file names in the path are longer than
   NAME_MAX. In the former case, calling thread is terminated and in the
   latter NULL is returned. */
static char *
get_arg_path (void *esp, int pos)
{
  char *path;

  path = get_arg_string (esp, pos, INT_MAX);
  /* Check that each file in the path has a name less than NAME_MAX. */
  if (!is_valid_path (path))
    return NULL;
  
  return path;
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
  return vaddr != NULL && is_user_vaddr (vaddr)
                       && pagedir_get_page (thread_current ()->pagedir, vaddr);
}

/* Checks that every file in a path has a name
   that does not exceed NAME_MAX characters. */
static bool
is_valid_path (char *path)
{
  bool valid = true;
  char *path_cpy;
  char *path_cpy_free;

  if (path == NULL)
    return false;
  
  int path_len = strlen (path);
  path_cpy = malloc (path_len + 1);
  ASSERT (path_cpy != NULL);
  path_cpy_free = path_cpy;
  
  strlcpy (path_cpy, path, path_len + 1);
  while (*path_cpy == '/')
    path_cpy++;

  /* Reject paths that end in a '/' unless referring to root directory. */
  if (*path_cpy != '\0' && path_cpy[strlen (path_cpy) - 1] == '/')
    {
      free (path_cpy_free);
      return false;
    }

  char *token = NULL;
  char *save_ptr = NULL;
  for (token = strtok_r (path_cpy, "/", &save_ptr);
       token != NULL && *token != '\0';
       token = strtok_r (NULL, "/", &save_ptr))
    {
      if (strlen (token) > NAME_MAX)
        {
          valid = false;
          break;
        }
    }

  free (path_cpy_free);
  return valid;
}
/* Returns whether FD is between 0 and MAX_FILES */
static bool
is_valid_fd (int fd) 
{
  return fd >= 0 && fd < MAX_FILES;
}

/* Checks whether the file descriptor table entry FDT_ENTRY is a file
   and points to a valid file. */
static bool
is_valid_file (struct fdt_entry *fdt_entry)
{
    return fdt_entry->fp.file != NULL && fdt_entry->type == FILE;
}