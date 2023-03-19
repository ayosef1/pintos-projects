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
static void split_path (const char *path, char **parent_dir_path,
                        char **dirent);

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

  // get_last_token (pathname, &parent_dir_path, &filename);
  split_path (pathname, &parent_dir_path, &filename);
  
  /* Cannot create the root directory as a file. */
  if (strcmp (parent_dir_path, "/") == 0 && strcmp (filename, "/") == 0)
    return false;
  else
    {
      parent_dir = dir_pathname_lookup (parent_dir_path);
      if (parent_dir == NULL)
        {
          free (parent_dir_path);
          free (filename);
          return false;
        }
    }
  bool success = (parent_dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, IS_FILE)
                  && dir_add (parent_dir, filename, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector);
  
  free (parent_dir_path);
  free (filename);
  dir_close (parent_dir);
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

  // get_last_token (pathname, &parent_dir_path, &filename);
  split_path (pathname, &parent_dir_path, &filename);
  parent_dir = dir_pathname_lookup (parent_dir_path);

  struct file *fp = NULL;

  if (parent_dir != NULL)
    {
      if (strcmp (filename, "/") == 0)
        fp = file_open (inode_open (ROOT_DIR_SECTOR));
      else if (dir_lookup (parent_dir, filename, &file_inode))
        fp = file_open (file_inode);
    }
  dir_close (parent_dir);
  free (filename);
  free (parent_dir_path);
  return fp;  
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *pathname) 
{
  bool success = false;
  char *filename;
  char *parent_dir_path;
  struct dir *parent_dir = NULL;
  struct inode *file_inode = NULL;
  
  split_path (pathname, &parent_dir_path, &filename);
  if (strcmp (parent_dir_path, "/") && strcmp (filename, "/") == 0)
    {
      free (parent_dir_path);
      free (filename);
      return false;
    }
  parent_dir = dir_pathname_lookup (parent_dir_path);
  if (parent_dir != NULL)
      success = dir_remove (parent_dir, filename); 
  
  free (parent_dir_path);
  free (filename);
  inode_close (file_inode);
  dir_close (parent_dir);
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

  split_path (dir, &parent_dir_path, &dirname);
  /* Using cwd. */
  if (strcmp (parent_dir_path, "/") && strcmp (dirname, "/") == 0)
    {
      /* Cannot mkdir the root directory. */
      free (parent_dir_path);
      free (dirname);
      return false;
    }
  parent_dir = dir_pathname_lookup (parent_dir_path);
  parent_dir_sector = inode_get_inumber (dir_get_inode (parent_dir));

  bool success = (parent_dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && dir_create (inode_sector, parent_dir_sector,
                                 INITIAL_DIRENTS)
                  && dir_add (parent_dir, dirname, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector);
  
  free (parent_dir_path);
  free (dirname);
  dir_close (parent_dir);
  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, ROOT_DIR_SECTOR, INITIAL_DIRENTS))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}

/* Given a path PATH, assigns EXCEPT_LAST to be all but the last dirent 
   and LAST to be the last dirent in PATH. */
static void
split_path (const char *path, char **parent_dir_path, char **dirent)
{
  int path_len = strlen (path);
  char *last_slash;

  /* Case 1: an absolute path to root directory. */
  if (path_len == 1 && *path == '/')
    {
      *parent_dir_path = malloc (2);
      ASSERT (*parent_dir_path != NULL);

      *dirent = malloc (2);
      ASSERT (*dirent != NULL);

      strlcpy (*parent_dir_path, "/", 2);
      strlcpy (*dirent, "/", 2);
    }
  /* Case 2: any path in format a/b/... or /a/b/... */
  else if ((last_slash = strrchr (path, '/')) != NULL)
  {
    int dirent_len;
    int parent_dir_path_len;

    dirent_len = strlen (last_slash + 1);
    parent_dir_path_len = path_len - dirent_len;

    *dirent = malloc (dirent_len + 1);
    ASSERT (*dirent != NULL);
    *parent_dir_path = malloc (parent_dir_path_len + 1);
    ASSERT (*parent_dir_path != NULL);

    strlcpy (*dirent, last_slash + 1, dirent_len + 1);
    strlcpy (*parent_dir_path, path, parent_dir_path_len + 1);
  }
  /* Case 3: relative path consisting of just the dirent */
  else
  {
    *parent_dir_path = malloc (2);
    ASSERT (*parent_dir_path != NULL);

    *dirent = malloc (path_len + 1);
    ASSERT (*dirent != NULL);

    strlcpy (*parent_dir_path, ".", 2);
    strlcpy (*dirent, path, path_len + 1);
  }
}