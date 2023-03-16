#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/cache.h"
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/thread.h"
#include "threads/malloc.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);
static void get_last_token (const char *path, char **except_last,
                            char **last);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();
  cache_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
  cache_write_to_disk (true);
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *pathname, off_t initial_size) 
{
  char *filename;
  char *parent_dir_path;
  struct dir *parent_dir;
  size_t inode_sector = 0;

  get_last_token (pathname, &parent_dir_path, &filename);
  /* Using cwd. */
  if (parent_dir_path == NULL)
      parent_dir = dir_open (inode_open (thread_current ()->cwd));
  else
    {
      parent_dir = dir_pathname_lookup (parent_dir_path);
      if (parent_dir == NULL)
        {
          free (parent_dir_path);
          free (filename);
          return false;
        }
      free (parent_dir_path);
    }
  bool success = (parent_dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, IS_FILE)
                  && dir_add (parent_dir, filename, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector);
  dir_close (parent_dir);

  free (filename);
  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *pathname)
{
  char *filename;
  char *parent_dir_path;
  struct dir *parent_dir;
  struct inode *file_inode;

  get_last_token (pathname, &parent_dir_path, &filename);
  if (parent_dir_path == NULL)
      parent_dir = dir_open (inode_open (thread_current ()->cwd));
  else 
    parent_dir = dir_pathname_lookup (parent_dir_path);

  if (parent_dir != NULL &&
      dir_lookup (parent_dir, filename, &file_inode))
      return file_open (file_inode);

  return NULL;
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *pathname) 
{
  struct dir *dir = dir_open_root ();
  bool success = dir != NULL && dir_remove (dir, pathname);
  dir_close (dir); 

  return success;
}


bool
filesys_mkdir (const char *dir)
{
  char *dirname;
  char *parent_dir_path;
  struct dir *parent_dir;
  size_t parent_dir_sector;
  size_t inode_sector = 0;

  get_last_token (dir, &parent_dir_path, &dirname);
  /* Using cwd. */
  if (parent_dir_path == NULL)
      parent_dir = dir_open (inode_open (thread_current ()->cwd));
  else
    {
      parent_dir = dir_pathname_lookup (parent_dir_path);
      if (parent_dir == NULL)
        {
          free (parent_dir_path);
          free (dirname);
          return false;
        }
      free (parent_dir_path);
    }

  parent_dir_sector = inode_get_inumber (dir_get_inode (parent_dir));
  bool success = (parent_dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && dir_create (inode_sector, parent_dir_sector, 16)
                  && dir_add (parent_dir, dirname, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector);
  dir_close (parent_dir);

  free (dirname);
  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}

/* Given a path PATH, assigns EXCEPT_LAST to be all but the last dirent 
   and LAST to be the last dirent in PATH. If PATH is relative and only
   contains one dirent, EXCEPT_LAST is set to NULL. */
static void
get_last_token (const char *path, char **except_last, char **last)
{
  int path_len = strlen (path);
  char *last_slash;
  
  last_slash = strrchr (path, '/');
  if (last_slash == NULL)
    {
      *except_last = NULL;
      *last = malloc (path_len + 1);
      ASSERT (last != NULL);
      strlcpy (*last, path, path_len + 1);
    }
  else /* TODO: why does strlcpy require + 1? */
    {
      int last_len = strlen (last_slash + 1);
      int except_last_len = path_len - last_len - 1;

      *last = malloc (last_len + 1);
      ASSERT (last != NULL);
      *except_last = malloc (except_last_len + 1);
      ASSERT (except_last != NULL);

      strlcpy (*last, last_slash + 1, last_len + 1);
      strlcpy (*except_last, path, except_last_len + 1);

    }
}