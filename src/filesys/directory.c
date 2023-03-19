#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/thread.h"

/* A directory. */
struct dir 
  {
    struct inode *inode;                /* Backing store. */
    off_t pos;                          /* Current position. */
  };

/* A single directory entry. */
struct dir_entry 
  {
    block_sector_t inode_sector;        /* Sector number of header. */
    char name[NAME_MAX + 1];            /* Null terminated file name. */
    bool in_use;                        /* In use or free? */
  };

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (block_sector_t sector, block_sector_t parent_sector)
{
  struct dir *dir;
  
  if (!inode_create (sector, NUM_INITIAL_DIRENTS * sizeof (struct dir_entry),
                     IS_DIR))
    return false;
  
  dir = dir_open (inode_open (sector));
  if (dir == NULL)
    return false;
  
  dir_add (dir, ".", sector);
  dir_add (dir, "..", parent_sector);
  dir_close (dir);
  return true;
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
    {
      dir->inode = inode;
      dir->pos = 0;
      return dir;
    }
  else
    {
      inode_close (inode);
      free (dir);
      return NULL; 
    }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  return dir_open (inode_open (ROOT_DIR_SECTOR));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
    {
      inode_close (dir->inode);
      free (dir);
    }
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (e.in_use && !strcmp (name, e.name)) 
      {
        if (ep != NULL)
          *ep = e;
        if (ofsp != NULL)
          *ofsp = ofs;
        return true;
      }
  return false;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
            struct inode **inode) 
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  inode_lock_dir (dir->inode);

  if (lookup (dir, name, &e, NULL))
    *inode = inode_open (e.inode_sector);
  else
    *inode = NULL;

  inode_unlock_dir (dir->inode);

  return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, const char *name, block_sector_t inode_sector)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;

  inode_lock_dir (dir->inode);
  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.
     
     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

 done:
  inode_unlock_dir (dir->inode);
  return success;
}

static size_t
get_num_dirents (struct dir *dir)
{
  struct dir_entry e;
  size_t ofs;
  size_t num_dirents;

  ASSERT (dir != NULL);

  num_dirents = 0;
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (e.in_use) 
      num_dirents++;
  return num_dirents - 2;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  inode_lock_dir (dir->inode);
  /* Find directory entry. */
  if (!lookup (dir, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;
  
  /* Not allowed to remove nonempty directories, cwd, 
     or directories currently open in process. */
  if (!inode_is_file (inode) && 
      (get_num_dirents (dir_open (inode)) != 0 ||
       thread_current ()->cwd == inode_get_inumber (inode) ||
       inode_get_open_cnt (inode) > 1))
    goto done;

  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

 done:
  inode_close (inode);
  inode_unlock_dir (dir->inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;
  inode_lock_dir (dir->inode);
  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) 
    {
      dir->pos += sizeof e;
      if (e.in_use && 
          strcmp(e.name, ".") != 0 &&
          strcmp(e.name, "..") != 0)
        {
          strlcpy (name, e.name, NAME_MAX + 1);
          inode_unlock_dir (dir->inode);
          return true;
        } 
    }
  inode_unlock_dir (dir->inode);
  return false;
}

/* Returns the dir struct associated with the directory 
 * specified at pathname or NULL if an error is encountered. */
struct dir *
dir_pathname_lookup (const char *pathname) 
{
  char *pathname_cpy;
  struct dir *dir = NULL;
  
  /* Dealing with absolute path. */
  if (pathname[0] == '/')
    {
      while (*pathname == '/')
        pathname++;
      dir = dir_open_root ();
    }
  /* Dealing with relative path. */
  else
    dir = dir_open (inode_open (thread_current ()->cwd));

  pathname_cpy = malloc (strlen (pathname) + 1);
  ASSERT (pathname != NULL);

  char *pathname_cpy_free = pathname_cpy;
  strlcpy (pathname_cpy, pathname, strlen (pathname) + 1);

  bool in_dir = true;
  char *token = NULL;
  char *save_ptr = NULL;
  struct inode *inode = NULL;
  for (token = strtok_r (pathname_cpy, "/", &save_ptr);
       token != NULL && *token != '\0';
       token = strtok_r (NULL, "/", &save_ptr))
    {
      /* Every dirent must be inside a directory. */
      if (!in_dir)
        {
          dir = NULL;
          inode_close (inode);
          break;
        }
      
      if (!dir_lookup (dir, token, &inode))
        {
          dir_close (dir);
          dir = NULL;
          break;
        }
      
      dir_close (dir);
      if (inode_is_file (inode))
          in_dir = false;
      else
        dir = dir_open (inode);

    }

  free (pathname_cpy_free);
  return dir;
}