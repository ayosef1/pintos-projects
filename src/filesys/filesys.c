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

/* If the IS_FILE flag is set creates a file with path PATHNAME and
   size INTIAL_SIZE.
   Otherwise creates a directory with path PATHNAME.

   Returns true if successful, false otherwise.
   Fails if a entry named NAME already exists in parent directory, invalid
   path or if internal memory allocation fails. */
bool
filesys_create (const char *pathname, off_t initial_size, bool is_file) 
{
  char *filename;
  char *parent_dir_path;
  struct dir *parent_dir;
  size_t inode_sector = 0;
  bool success = false;

  split_path (pathname, &parent_dir_path, &filename);
    /* Cannot create the root directory as a file. */
  if (strcmp (parent_dir_path, "/") == 0 && strcmp (filename, "/") == 0)
    goto done;
  
  parent_dir = dir_pathname_lookup (parent_dir_path);
  if (parent_dir == NULL || !free_map_allocate (1, &inode_sector))
    goto done;
  
  if (is_file)
    success = inode_create (inode_sector, initial_size, IS_FILE);
  else
    {
      block_sector_t parent_dir_sector = inode_get_inumber (dir_get_inode 
                                                            (parent_dir));
      success = dir_create (inode_sector, parent_dir_sector);
    }

  if (success)
    success =  dir_add (parent_dir, filename, inode_sector);
  
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector);
  
  done:
    free (parent_dir_path);
    dir_close (parent_dir);
    return success;
}

/* Opens the file or directory with the given PATHNAME.
   Stores the relevant file /directory pointer and fdt_entry type in
   FDT_ENTRY.
   Returns true if file / dir was successfully opened.
   Fails if invalid path, or if an internal memory allocation fails. */
bool
filesys_open (const char *pathname, struct fdt_entry *fdt_entry)
{
  char *filename;
  char *parent_dir_path;
  struct dir *parent_dir;
  struct inode *inode;
  bool success = false;

  split_path (pathname, &parent_dir_path, &filename);
  parent_dir = dir_pathname_lookup (parent_dir_path);

  if (parent_dir == NULL)
    goto open_done;

  if (strcmp (filename, "/") == 0)
    {
      fdt_entry->fp.dir = dir_open_root ();
      fdt_entry->type = DIR;
      success = true;
      
    }
  else if (dir_lookup (parent_dir, filename, &inode) && inode != NULL)
    {
      if (inode_is_file (inode))
        {
          fdt_entry->fp.file = file_open (inode);
          fdt_entry->type = FILE;
        }
      else
        {
          fdt_entry->fp.dir = dir_open (inode);
          fdt_entry->type = DIR;
        }
      success = fdt_entry->fp.file != NULL;
    }
  open_done:
    dir_close (parent_dir);
    free (parent_dir_path);
    return success;
}

/* Deletes the file / directory named PATHNAME.
   Returns true if successful, false on failure.
   Fails if PATHNAME invalid, PATHNAME is a non-empty directory
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
      return false;
    }
  parent_dir = dir_pathname_lookup (parent_dir_path);
  if (parent_dir != NULL)
      success = dir_remove (parent_dir, filename); 
  
  free (parent_dir_path);
  inode_close (file_inode);
  dir_close (parent_dir);
  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, ROOT_DIR_SECTOR))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}

/* Given a path PATH, assigns PARENT_DIR_PATH to be everything up to
   the final `/`delimiter and DIRENT to be the last dirent in PATH. */
static void
split_path (const char *path, char **parent_dir_path, char **dirent)
{
  int path_len = strlen (path);
  char *last_slash;

  /* Case 1: an absolute path to root directory consisting only of /'s. */
  if (path[path_len - 1] == '/')
    {
      *parent_dir_path = malloc (2);
      ASSERT (*parent_dir_path != NULL);

      strlcpy (*parent_dir_path, "/", 2);
      *dirent = path + path_len - 1;
    }
  /* Case 2: any path in format a/b/... or /a/b/... */
  else if ((last_slash = strrchr (path, '/')) != NULL)
  {
    int dirent_len;
    int parent_dir_path_len;

    dirent_len = strlen (last_slash + 1);
    parent_dir_path_len = path_len - dirent_len;

    *parent_dir_path = malloc (parent_dir_path_len + 1);
    ASSERT (*parent_dir_path != NULL);

    *dirent = last_slash + 1;
    strlcpy (*parent_dir_path, path, parent_dir_path_len + 1);
  }
  /* Case 3: relative path consisting of just the dirent */
  else
  {
    *parent_dir_path = malloc (2);
    ASSERT (*parent_dir_path != NULL);

    *dirent =  path;
    strlcpy (*parent_dir_path, ".", 2);
  }
}