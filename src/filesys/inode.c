#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <stdio.h>
#include <string.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define NUM_DIRECT_POINTERS 122
#define SINGLE_INDIRECT_INDEX NUM_DIRECT_POINTERS
#define DOUBLE_INDIRECT_INDEX NUM_DIRECT_POINTERS + 1
#define NUM_BLOCK_POINTERS 124
#define POINTERS_PER_BLOCK  128
#define MAX_INDICIES 3
#define MAX_FILE_BYTES (NUM_DIRECT_POINTERS + NUM_BLOCK_POINTERS + \
                        NUM_BLOCK_POINTERS * NUM_BLOCK_POINTERS)   \
                        * BLOCK_SECTOR_SIZE                        \


/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    off_t length;                               /* File size in bytes. */
    block_sector_t blocks [NUM_BLOCK_POINTERS]; /* Block Pointers. */
    bool is_file;                               /* Whether inode represents
                                                   file or dir.*/
    unsigned magic;                             /* Magic number. */
  };

static void inode_update_length (struct inode *inode, off_t write_end);
static off_t ofs_to_indicies (off_t ofs, off_t *indicies);
static block_sector_t get_read_block (block_sector_t inode_sector,
                                      off_t offset);
static block_sector_t get_write_block (block_sector_t inode_sector,
                                       off_t offset);

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    int write_cnt;                      /* Number of writers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct condition no_writers;        /* Condition variable to appropriately
                                           set deny_write_cnt. */
    
    struct lock lock;                   /* Sync for file extension. */
  };

/* Returns the logical index of the block that byte POS resides in
   in an inode. */
static inline off_t
direct_idx (off_t pos) 
{
  return pos / BLOCK_SECTOR_SIZE;
}

/* Returns the offest within a singly indirect block the offset
   LOGICAL_IDX from the first sector accessed via doubly indirect. */
static inline off_t
singly_indirect_idx (off_t logical_idx)
{
  return logical_idx % NUM_BLOCK_POINTERS;
}

/* Returns the offest within a doubly indirect block the offset
   LOGICAL_IDX from the first sector accessed via doubly indirect. */
static inline off_t
doubly_indirect_idx (off_t logical_idx)
{
  return logical_idx / NUM_BLOCK_POINTERS;
}


/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;
/* Sync for the list of opening inodes. */
static struct lock open_inodes_lock;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
  lock_init (&open_inodes_lock);
}

/* Initializes an inode with LENGTH bytes of data.
   Returns true if successful.
   Fails is LENGTH is not positive. */
bool
inode_create (block_sector_t sector, off_t length, bool is_file)
{
  struct cache_entry *cache_entry;
  struct inode_disk *disk_inode;

  ASSERT (length >= 0);
  cache_entry = cache_add_sector (sector, true);
  disk_inode = (struct inode_disk  *) cache_entry->data;
  disk_inode->length = length;
  disk_inode->magic = INODE_MAGIC;
  disk_inode->is_file = is_file;
  cache_entry->dirty = true;
  lock_release (&cache_entry->lock);
  return true;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  lock_acquire (&open_inodes_lock);
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode->open_cnt++;
          lock_release (&open_inodes_lock);
          return inode; 
        }
    }
  lock_release (&open_inodes_lock);

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->write_cnt = 0;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init (&inode->lock);
  cond_init (&inode->no_writers);

  lock_acquire (&open_inodes_lock);
  list_push_front (&open_inodes, &inode->elem);
  lock_release (&open_inodes_lock);

  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL) {
    lock_acquire (&open_inodes_lock);
    inode->open_cnt++;
    lock_release (&open_inodes_lock);
  }
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  lock_acquire (&open_inodes_lock);
  if (--inode->open_cnt == 0)
    {
        list_remove (&inode->elem);
        lock_release (&open_inodes_lock);

        /* Deallocate blocks if removed. */
        if (inode->removed) 
          {
            free_map_release (inode->sector);
            /* Need to do recursion through blocks to release. */
          }
        free (inode);
        return;
    }
  lock_release (&open_inodes_lock);
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;
      
      block_sector_t sector_idx = get_read_block (inode->sector, offset);
      if (sector_idx == 0)
          memset(buffer + bytes_read, 0, chunk_size);
      else
        {
          struct cache_entry *to_read = cache_get_entry (sector_idx, R_SHARE);
          memcpy (buffer + bytes_read, to_read->data + sector_ofs, chunk_size);
          cache_release_entry (to_read, R_SHARE);
        }
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   Write to end of file extends teh inode. */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  lock_acquire (&inode->lock);
  if (inode->deny_write_cnt)
    {
      lock_release (&inode->lock);
      return 0;
    }
  inode->write_cnt++;
  lock_release (&inode->lock);

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = MAX_FILE_BYTES - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      enum cache_use_type type;
      block_sector_t sector_id = get_write_block (inode->sector, offset);
      if (sector_id == 0)
        break;
      
      /* If extended the file need to . */
      if (offset + chunk_size > inode_length(inode))
        type = W_EXCL;
      else
        type = W_SHARE;
      
      // printf ("MIN LEFT: %d, SECTOR LEFT: %d, INODE_LEFT %d, SIZE %d\n", min_left, sector_left, inode_left, size);

      struct cache_entry *cache_entry = cache_get_entry (sector_id, type);
      // if (sector_id >= 382)
      //   {
      //     printf ("Sector Id %u, chunk size %d at ofset  %d\n", sector_id, chunk_size, sector_ofs);
      //     hex_dump (offset, cache_entry->data + sector_ofs, chunk_size, true);
      //   }
      memcpy (cache_entry->data + sector_ofs, buffer + bytes_written,
              chunk_size);
      cache_release_entry (cache_entry, type);

      // if (sector_id >= 382)
      //   {
      //     hex_dump (offset, cache_entry->data + sector_ofs, chunk_size, true);
      //   }


      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

    /* Update file length once write is complete. */
    inode_update_length (inode, offset);

    /* Signal any process waiting to deny file write. */
    lock_acquire (&inode->lock);
    if (--inode->write_cnt == 0)
      cond_signal (&inode->no_writers, &inode->lock);
    lock_release (&inode->lock);
  

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  lock_acquire (&inode->lock);
  while (inode->write_cnt != 0)
    cond_wait (&inode->no_writers, &inode->lock);
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  lock_release (&inode->lock);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  lock_acquire (&inode->lock);
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
  lock_release (&inode->lock);
}

/* Whenever accessing the inode length we need to update it on disk as well
   becaue don't want disk to not have up to date inode length. Might as well
   keep inode length as something stored on disk. */
off_t
inode_length (const struct inode *inode)
{
  off_t inode_length;
  struct cache_entry * cache_entry = cache_get_entry (inode->sector, R_SHARE);
  struct inode_disk *data = (struct inode_disk *) cache_entry->data;
  inode_length = data->length;
  cache_release_entry (cache_entry, R_SHARE);
  return inode_length;
}

/* Update the inode INODE's length WRITE_END is larger than current length. */
static void
inode_update_length (struct inode *inode, off_t write_end)
{
    struct cache_entry * inode_entry = cache_get_entry (inode->sector, W_EXCL);
    struct inode_disk *data = (struct inode_disk *) inode_entry->data;
    if (write_end > data->length)
      data->length = write_end;
    cache_release_entry (inode_entry, W_EXCL);
}

/* Calculates the indexes at each block level to  */
static off_t
ofs_to_indicies (off_t ofs, off_t *indicies)
{
  off_t logical_idx = direct_idx (ofs);
  if (logical_idx < NUM_DIRECT_POINTERS)
    {
      indicies[0] = logical_idx;
      return 1;
    }
  logical_idx -= NUM_DIRECT_POINTERS;

  if (logical_idx < NUM_BLOCK_POINTERS)
    {
      indicies[0] = SINGLE_INDIRECT_INDEX;
      indicies[1] = logical_idx;
      return 2;
    }
  else
    {
      logical_idx -= NUM_DIRECT_POINTERS;
      indicies[0] = DOUBLE_INDIRECT_INDEX;
      indicies[1] = doubly_indirect_idx (logical_idx);
      indicies[2] = singly_indirect_idx (logical_idx);
      return 3;
      
    }
}

/* Set the block pointer at offset BLOCK_OFS in block sector BLOCK to
   SECTOR. */
static block_sector_t
get_block_ptr (void *block, off_t block_ofs, bool inode)
{
  if (inode)
    {
      struct inode_disk *disk_inode = (struct inode_disk *) block;
      return disk_inode->blocks[block_ofs];
    }
  else
    {
      block_sector_t *blocks = (block_sector_t *) block;
      return blocks[block_ofs];
    }
}

/* Set the block pointer at offset BLOCK_OFS in block sector BLOCK to
   SECTOR. */
static void
set_block_ptr (void *block, off_t block_ofs, block_sector_t sector,
               bool inode)
{
  if (inode)
    {
      struct inode_disk *disk_inode = (struct inode_disk *) block;
      disk_inode->blocks[block_ofs] = sector;
    }
  else
    {
      block_sector_t *blocks = (block_sector_t *) block;
      blocks[block_ofs] = sector;
    }
}

/* Return the block number of the block holds the byte at offset OFFSET
   in the data represented by the inode that lives at sector INODE_SECTOR.
   Returns 0 if byte's block hasn't been written yet. */
static block_sector_t
get_read_block (block_sector_t inode_sector, off_t offset)
{
  off_t indicies[MAX_INDICIES];
  off_t num_indicies = ofs_to_indicies (offset, indicies);

  int cur_depth = 0;
  struct cache_entry *cur = cache_get_entry (inode_sector,
                                             R_SHARE);
  block_sector_t child_sector = get_block_ptr (cur->data, indicies[cur_depth],
                                               true);
  cache_release_entry (cur, R_SHARE);

  while (cur_depth < num_indicies - 1 && child_sector != 0)
    {
      cur = cache_get_entry (child_sector, R_SHARE);

      /* Advance. */
      cur_depth++;
      child_sector = get_block_ptr (cur->data, indicies[cur_depth], false);
      cache_release_entry (cur, R_SHARE);
    }
  return child_sector;
}

/* Return the block number of the block holds the byte at offset OFFSET
   in the data represented by the inode that lives at sector INODE_SECTOR.
   Allocates each block needed if not yet allocated.
   
   If block was allocated, sets the cache use type to W_EXCL. */
static block_sector_t
get_write_block (block_sector_t inode_sector, off_t offset)
{
  off_t indicies[MAX_INDICIES];
  off_t num_indicies = ofs_to_indicies (offset, indicies);

  int cur_depth = 0;
  block_sector_t cur_sector = inode_sector;
  struct cache_entry *cur = cache_get_entry (cur_sector,
                                             R_SHARE);

  block_sector_t child_sector = get_block_ptr (cur->data, indicies[cur_depth],
                                               true);
  cache_release_entry (cur, R_SHARE);

  /* Iterate through the indicies to get the data block number. */
  while (cur_depth < num_indicies - 1 && child_sector != 0)
    {
      cur = cache_get_entry (child_sector, R_SHARE);
      cur_sector = child_sector;
      // printf ("GORDON %u\n", cur_sector);

      cur_depth++;
      child_sector = get_block_ptr (cur->data, indicies[cur_depth], false);
      cache_release_entry (cur, R_SHARE);
    }

  /* One of the indirect blocks or the data block was not present.
     Allocate new blocks and updates the parent indexes for each allocated
     block. */
  if (child_sector == 0)
    {
      /* Get the necessary free blocks from free map. */
      int num_to_create = num_indicies - cur_depth;
      block_sector_t new_sectors[num_to_create];
      if (!free_map_allocate (num_to_create, new_sectors))
          return 0;
        

      /* First allocated block given to data. */
      int num_created = 0;
      block_sector_t write_sector = new_sectors[num_created];
      struct cache_entry *data_entry;
      data_entry = cache_add_sector (write_sector,
                                     true);
      lock_release (&data_entry->lock);
      num_created++;

      /* Create parent block and write in the appropriate entry.
         Start from lowest depth to so satisfy the recoverability criteria. */
      int parent_depth = num_indicies - num_created;
      struct cache_entry *parent_entry;
      while (parent_depth > cur_depth)
        {
          block_sector_t parent_sector = new_sectors[num_created];
          parent_entry = cache_add_sector (parent_sector,
                                           true);
          set_block_ptr (parent_entry->data, indicies[parent_depth],
                          new_sectors[num_created - 1], false);
          // printf ("NEW INDIRECT %u points to BLOCK %u at index %d\n", parent_sector, new_sectors[num_created - 1], indicies[parent_depth]);

          lock_release (&parent_entry->lock);
          parent_depth--;
          num_created++;
        }

      /* Last case. If parent_depth == 0, inode, different logic to set ptr.
         Exclusive lock needed because the block exists already. */
      bool cur_is_inode = parent_depth == 0;
      // printf ("FINAL: %s %u points to BLOCK %u at index %d\n",
      //         cur_is_inode ? "INODE" : "INDIRECT", cur_sector, new_sectors[num_created - 1], indicies[parent_depth]);
      parent_entry = cache_get_entry (cur_sector, W_EXCL);
      set_block_ptr (parent_entry->data, indicies[parent_depth],
                     new_sectors[num_created - 1],
                     cur_is_inode);
      cache_release_entry (parent_entry, W_EXCL);
        
      return write_sector;
    }
  return child_sector;
}

