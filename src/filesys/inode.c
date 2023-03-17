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

/* All used for the multilevel index inode. */
#define NUM_DIRECT_POINTERS 122
/* Index of singly indirect block in inode. */
#define SINGLE_INDIRECT_INDEX NUM_DIRECT_POINTERS
/* Index of doublly indirect block in inode. */
#define DOUBLE_INDIRECT_INDEX NUM_DIRECT_POINTERS + 1
/* Number of block pointers in an inode. */
#define NUM_BLOCK_POINTERS 124
/* Number of block pointers in an indirect block. */
#define POINTERS_PER_BLOCK  128
/* Max number of indicies needed to get data block in inode. */
#define MAX_INDICIES 3
/* Max number of bytes a file can be. */
#define MAX_FILE_BYTES (NUM_DIRECT_POINTERS + NUM_BLOCK_POINTERS + \
                        NUM_BLOCK_POINTERS * NUM_BLOCK_POINTERS)   \
                        * BLOCK_SECTOR_SIZE    


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
static bool inode_check_extension (struct inode *inode, off_t write_end);
static void free_inode_blocks (struct inode *inode);
static void free_block (block_sector_t sector, off_t height);
static off_t ofs_to_indicies (off_t ofs, off_t *indicies);
static block_sector_t get_data_sector (block_sector_t inode_sector,
                                       off_t offset, bool read);

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
    
    struct lock deny_write_lock;        /* Sync for deny_write_cnt. */
    struct lock extension_lock;         /* Sync for file extension. */
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
  struct cache_entry *inode_entry;
  struct inode_disk *disk_inode;

  ASSERT (length >= 0);
  inode_entry = cache_get_entry (sector, EXCL, true);
  disk_inode = (struct inode_disk  *) inode_entry->data;
  disk_inode->length = length;
  disk_inode->magic = INODE_MAGIC;
  disk_inode->is_file = is_file;
  cache_release_entry (inode_entry, EXCL, true);
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
  lock_init (&inode->deny_write_lock);
  lock_init (&inode->extension_lock);
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
            free_inode_blocks (inode);
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
      
      block_sector_t sector_idx = get_data_sector (inode->sector, offset, true);
      if (sector_idx == 0)
          memset(buffer + bytes_read, 0, chunk_size);
      else
        {
          struct cache_entry *to_read = cache_get_entry (sector_idx, SHARE,
                                                         false);
          memcpy (buffer + bytes_read, to_read->data + sector_ofs, chunk_size);
          cache_release_entry (to_read, SHARE, false);
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

  lock_acquire (&inode->deny_write_lock);
  if (inode->deny_write_cnt)
    {
      lock_release (&inode->deny_write_lock);
      return 0;
    }
  inode->write_cnt++;
  lock_release (&inode->deny_write_lock);

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

      /* Does checking of extension need to be synchronized. */
      bool extension = inode_check_extension (inode, offset + chunk_size);
      
      block_sector_t sector_id = get_data_sector (inode->sector, offset, false);
      if (sector_id == 0)
        {
          if (extension)
            lock_release (&inode->extension_lock);
          break;
        }
      
      /* Always a shared write because other extension lock acquired and
         other readers won't have access because length isn't updated. */
      struct cache_entry *entry_to_write = cache_get_entry (sector_id, SHARE,
                                                            false);
      memcpy (entry_to_write->data + sector_ofs, buffer + bytes_written,
              chunk_size);
      cache_release_entry (entry_to_write, SHARE, true);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
      if (extension)
        inode_update_length (inode, offset);
    }

    /* Signal any process waiting to deny file write. */
    lock_acquire (&inode->deny_write_lock);
    if (--inode->write_cnt == 0)
      cond_signal (&inode->no_writers, &inode->deny_write_lock);
    lock_release (&inode->deny_write_lock);
  

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  lock_acquire (&inode->deny_write_lock);
  while (inode->write_cnt != 0)
    cond_wait (&inode->no_writers, &inode->deny_write_lock);
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  lock_release (&inode->deny_write_lock);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  lock_acquire (&inode->deny_write_lock);
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
  lock_release (&inode->deny_write_lock);
}

/* Whenever accessing the inode length we need to update it on disk as well
   becaue don't want disk to not have up to date inode length. Might as well
   keep inode length as something stored on disk. */
off_t
inode_length (const struct inode *inode)
{
  off_t inode_length;
  struct cache_entry * cache_entry = cache_get_entry (inode->sector, SHARE,
                                                      false);
  struct inode_disk *data = (struct inode_disk *) cache_entry->data;
  inode_length = data->length;
  cache_release_entry (cache_entry, SHARE, false);
  return inode_length;
}

/* Atomically checks inode INODE's length and acquires the extension lock
   if the write about to happen is longer than WRITE_END. */
static bool
inode_check_extension (struct inode *inode, off_t write_end)
{
  bool extension = false;
  struct cache_entry * inode_entry = cache_get_entry (inode->sector, EXCL,
                                                      false);
  struct inode_disk *data = (struct inode_disk *) inode_entry->data;
  if (write_end > data->length)
    {
      lock_acquire (&inode->extension_lock);
      extension = true;
    }
  cache_release_entry (inode_entry, EXCL, false);
  return extension;
}

/* Update the inode INODE's length to WRITE_END and releases the extension
   lock. */
static void
inode_update_length (struct inode *inode, off_t write_end)
{
    struct cache_entry * inode_entry = cache_get_entry (inode->sector, EXCL,
                                                        false);
    struct inode_disk *data = (struct inode_disk *) inode_entry->data;
    data->length = write_end;
    cache_release_entry (inode_entry, EXCL, true);
    lock_release (&inode->extension_lock);
}

/* Calculates the indicies required to find the level indicies of the
   block that byte OFS resides in. 
   
   Returns the number of indicies needed to access the data block.
   1 means a OFS is accessed via a directly indexed block in the inode.
   2 means a OFS is accessed via singly indirect indexed block in the inode.
   3 means a OFS is accessed via doubly indirect indexed block in the inode. */
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

/* Allocate and initalize new sectors with sector numbers in NEW_SECTORS.
   Start with the data block at START_DEPTH and move upwards until the parent
   of the newly allocated block is at STOP DEPTH.  */
static void
allocate_new_blocks (block_sector_t *new_sectors, off_t *indicies,
                     off_t start_depth, off_t stop_depth)
{
  struct cache_entry *parent_entry;
  block_sector_t *parent_sector;
  block_sector_t *child_sector = new_sectors;
  int parent_depth = start_depth;

  bool new = true;
  bool dirty = false;

  struct cache_entry *data_entry = cache_get_entry (*new_sectors, EXCL, new);
  cache_release_entry (data_entry, EXCL, dirty);
  parent_depth--;
  new_sectors++;

  /* Create parent block and write in the appropriate entry.
      Start from lowest depth to so satisfy the recoverability criteria.
      Dirty true because setting block pointer. */
  dirty = true;
  while (parent_depth > stop_depth)
    {
      parent_sector = new_sectors;
      parent_entry = cache_get_entry (*parent_sector, EXCL, new);
      set_block_ptr (parent_entry->data, indicies[parent_depth],
                     *child_sector, false);
      cache_release_entry (parent_entry, EXCL, dirty);
      child_sector = parent_sector;
      parent_depth--;
      new_sectors++;
    }
}

/* Recursively frees block sector SECTOR that is at height HEIGHT from
   free map.

   Height 0 corresponds to a data block. Height > 0 is the level of
   indirection of the block. */
static void
free_block (block_sector_t sector, off_t height)
{
  if (sector == 0)
    return;
  
  if (height > 0)
    {
      struct cache_entry *indirect_entry = cache_get_entry (sector, SHARE,
                                                            false);
      uint8_t *block = indirect_entry->data;
      height--;
      block_sector_t cur_sector;
      for (off_t i = 0; i < NUM_BLOCK_POINTERS; i++)
        {
          cur_sector = get_block_ptr (block, i, false);
          free_block (cur_sector, height);
        }
      cache_release_entry (indirect_entry, false, false);
    }
  free_map_release (sector);
}

/* Frees all blocks assocaited with inode INODE from the free map as well
   as inode itself. */
static void
free_inode_blocks (struct inode *inode)
{
  /* Do SHARE so no deadlock when getting entries at level below.
     Little bit hacky because should be exclusive access but no one
     else can access this block when this call is made so OK. */
  struct cache_entry *inode_entry = cache_get_entry (inode->sector, SHARE,
                                                     false);
  struct inode_disk *inode_disk = (struct inode_disk *) inode_entry->data;

  off_t last_direct_idx = direct_idx (inode_disk->length);
  off_t inode_stop_idx;

  if (last_direct_idx < NUM_DIRECT_POINTERS)
    inode_stop_idx = last_direct_idx;
  else if (last_direct_idx < NUM_DIRECT_POINTERS + NUM_BLOCK_POINTERS)
    inode_stop_idx = SINGLE_INDIRECT_INDEX;
  else
    inode_stop_idx = DOUBLE_INDIRECT_INDEX;

  block_sector_t cur_sector;
  off_t height = 0;
  off_t cur_idx = 0;
  while (cur_idx <= inode_stop_idx)
    {
      if (cur_idx == SINGLE_INDIRECT_INDEX)
        height = 1;
      else if (cur_idx == DOUBLE_INDIRECT_INDEX)
        height = 2;

      cur_sector = get_block_ptr (inode_disk, cur_idx, true);
      free_block (cur_sector, height);
      cur_idx++;
    }

  free_map_release (inode->sector);
  cache_release_entry (inode_entry, SHARE, false);
}



/* Return the block number of the block that holds the byte at offset OFFSET
   in the data represented by the inode that lives at sector INODE_SECTOR.
   
   Flag READ determines what to do when a block is found to be unallocated.
   If READ is true, then 0 is returned.
   If READ is false, the data block and the relevant indirect blocks not
   yet allocated are allocated. */
static block_sector_t
get_data_sector (block_sector_t inode_sector, off_t offset, bool read)
{
  off_t indicies[MAX_INDICIES];
  off_t num_indicies = ofs_to_indicies (offset, indicies);

  int cur_depth = 0;
  block_sector_t cur_sector = inode_sector;
  struct cache_entry *cur = cache_get_entry (cur_sector, SHARE, false);

  block_sector_t child_sector = get_block_ptr (cur->data, indicies[cur_depth],
                                               true);
  cache_release_entry (cur, SHARE, false);

  /* Iterate through the indicies to get the data block number. */
  while (cur_depth < num_indicies - 1 && child_sector != 0)
    {
      cur = cache_get_entry (child_sector, SHARE, false);
      cur_sector = child_sector;

      cur_depth++;
      child_sector = get_block_ptr (cur->data, indicies[cur_depth], false);
      cache_release_entry (cur, SHARE, false);
    }
    
  if (read)
    return child_sector;

  /* If all blocks allocated, return the found sector. */
  if (child_sector != 0)
    return child_sector;

  /* Get the necessary free blocks from free map. */
  int num_to_create = num_indicies - cur_depth;
  block_sector_t new_sectors[num_to_create];
  if (!free_map_allocate (num_to_create, new_sectors))
      return 0;

  /* First allocated block given to data. */
  block_sector_t data_sector = new_sectors[0];

  allocate_new_blocks (new_sectors, indicies, num_indicies, cur_depth);

  /* Last case. If parent_depth == 0, inode, different logic to set ptr.
      Exclusive lock needed because the block exists already. */
  bool cur_is_inode = cur_depth == 0;

  cur = cache_get_entry (cur_sector, EXCL, false);
  set_block_ptr (cur->data, indicies[cur_depth],
                  new_sectors[num_to_create - 1],
                  cur_is_inode);
  cache_release_entry (cur, EXCL, true);
    
  return data_sector;
}

