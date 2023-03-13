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

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define INODE_DIR_BLOCKS 122
#define DOUBLY_DIR_IDX 122

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t blocks[INODE_DIR_BLOCKS + 1];   /* Blocks used for payload data. */
    off_t length;                       /* File size in bytes. */
    bool directory;                     /* Indicates if inode for directory. */
    unsigned magic;                     /* Magic number. */
    uint32_t unused[2];               /* Not used. */
  };

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
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    block_sector_t *blocks;             /* Location of payload data. */
    off_t length;                       /* File size in bytes. */
  };


// TODO: MOVE TO TOP
#define NUMS_PER_SECTOR BLOCK_SECTOR_SIZE / sizeof (block_sector_t)

/* Given the logical index of a sector, returns the physical sector number on 
   disk where that block is stored. Starts search from doubly indirect block
   located at sector DOUBLY_INDIR. */
static block_sector_t
index_lookup_sector (const block_sector_t *blocks, int logical_idx)
{
  /* Dealing with a direct block. */
  if (logical_idx < INODE_DIR_BLOCKS)
    return blocks[logical_idx];

  /* Desired sector must be accessed via doubly indrect block */
  block_sector_t doubly_indir_block[NUMS_PER_SECTOR];
  int singly_indir_block_idx;
  int singly_indir_block_num;
  block_sector_t singly_indir_block[NUMS_PER_SECTOR];

  /* Read the singly indirect block numbers into doubly_indir_block */
  cache_read (blocks[DOUBLY_DIR_IDX], doubly_indir_block,
              BLOCK_SECTOR_SIZE, 0);
  
  singly_indir_block_idx = (logical_idx - INODE_DIR_BLOCKS) / NUMS_PER_SECTOR;
  singly_indir_block_num = doubly_indir_block[singly_indir_block_idx];

  /* Read the direct block numbers into singly_indir_block */
  cache_read (singly_indir_block_num, singly_indir_block, BLOCK_SECTOR_SIZE, 0);

  return singly_indir_block[(logical_idx - INODE_DIR_BLOCKS) % NUMS_PER_SECTOR];
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  if (pos < inode->length)
    return index_lookup_sector (inode->blocks, pos / BLOCK_SECTOR_SIZE);
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  printf ("Trying to allocate sectors for file for inode %u\n", sector);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;

      bool large_file = false;
      size_t num_doubly = 0;
      size_t num_singly = 0;
      if (sectors > INODE_DIR_BLOCKS)
        {
          num_doubly = 1;
          num_singly = DIV_ROUND_UP (sectors - INODE_DIR_BLOCKS,
                                     NUMS_PER_SECTOR);
          large_file = true;
        }
      /* NUMBER OF BLOCKS NEEDED = INODE_DIR_BLOCKS + num_doubly + num_singly + (sectors - INODE_DIR_BLOCKS) */
      int total_sectors = sectors + num_doubly + num_singly;

      block_sector_t sector_nums[total_sectors];
      if (free_map_allocate_non_consec (total_sectors, sector_nums))
        {
          /* The first INODE_DIR_BLOCKS in sector_nums are the sector ids of 
             direct blocks whose numbers are stored on the inode. The next 
             block is the doubly indirect block stored on the inode. The next
             128 sector ids belong to the singly indirect blocks stored in the
             doubly indirect block. Finally, the sectors - INODE_DIR_BLOCKS 
             direct blocks which we access through indirect addressing make up
             the remainder of our array.
           */
          block_sector_t *direct_on_inode;
          block_sector_t *doubly_indirect;
          block_sector_t *singly_indirect_blocks;
          block_sector_t *direct_off_inode;

          direct_on_inode = sector_nums;
          doubly_indirect = sector_nums + DOUBLY_DIR_IDX;
          singly_indirect_blocks = doubly_indirect + 1;
          direct_off_inode = singly_indirect_blocks + num_singly;

          /* Update the direct blocks on the inode. */
          size_t i;
          for (i = 0; i < INODE_DIR_BLOCKS; i++)
            disk_inode->blocks[i] = direct_on_inode[i];
          
          /* Update the doubly indirect block. */
          if (large_file)
            disk_inode->blocks[DOUBLY_DIR_IDX] = *doubly_indirect;

          /* Write the actual inode itself to its sector. */
          PANIC ("successfully allocated.");
          cache_write (sector, disk_inode, BLOCK_SECTOR_SIZE, 0);

          if (large_file)
            {
              /* Write singly indirect block numbers to doubly indirect block. */
              cache_write (*doubly_indirect, singly_indirect_blocks, 
                           num_singly * sizeof (block_sector_t), 0);
              /* Populate singly indirect blocks with direct block numbers. */
              block_sector_t *cur_dir_chunk = direct_off_inode;
              block_sector_t *cur_sindir = singly_indirect_blocks;
              size_t num_dir_blocks_written = sectors - INODE_DIR_BLOCKS;

              while (num_dir_blocks_written > 0)
                {
                  int to_write = num_dir_blocks_written < NUMS_PER_SECTOR ? 
                    num_dir_blocks_written : NUMS_PER_SECTOR;
                  cache_write (*cur_sindir, cur_dir_chunk, to_write, 0);
                  cur_sindir++;
                  cur_dir_chunk += to_write;
                  num_dir_blocks_written -= to_write;
                }
            }
          if (sectors > 0)
            {
                static char zeros[BLOCK_SECTOR_SIZE];
                size_t i;
                
                
                for (i = 0; i < sectors; i++)
                  {
                    size_t sect_num;
                    
                    sect_num = index_lookup_sector (disk_inode->blocks, i);
                    cache_write (sect_num, zeros, BLOCK_SECTOR_SIZE, 0);
                  }
            }
        }
        success = true; 
    }
      free (disk_inode);
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;
  struct inode_disk data;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;

  cache_read (inode->sector, &data, BLOCK_SECTOR_SIZE, 0);
  inode->blocks = data.blocks;
  // inode->file_start = data.start;
  inode->length = data.length;
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  /* TODO: do we need a lock?*/
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  /* TODO: do we need a lock?*/
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
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* TODO: free sectors keeping indirect addressing in mind. */
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          size_t i;
  
          size_t sectors = bytes_to_sectors (inode->length);

          if (sectors > 0)
            {
              /* First, release the payload blocks. */
              for (i = 0; i < sectors; i++)
                {
                  size_t sector_num;
                  
                  sector_num = index_lookup_sector (inode->blocks, i);
                  free_map_release (sector_num, 1);
                }
            }
          if (sectors > INODE_DIR_BLOCKS)
            {
              block_sector_t doubly_indir_block[NUMS_PER_SECTOR];
              
              /* Next, release the singly indirect blocks. */
              size_t num_singly = DIV_ROUND_UP (sectors - INODE_DIR_BLOCKS,
                                                NUMS_PER_SECTOR);
              cache_read (inode->blocks[DOUBLY_DIR_IDX], doubly_indir_block,
                          BLOCK_SECTOR_SIZE, 0);
              for (i = 0; i < num_singly; i++)
                  free_map_release (doubly_indir_block[i], 1);

              /* And then the doubly indirect block. */
              free_map_release (inode->blocks[DOUBLY_DIR_IDX], 1);
            }
          /* Finally, release the sector containing the inode. */
          free_map_release (inode->sector, 1);
        }
      // else
      //   {
      //     /* Write INODE to disk*/
      //     /* TODO: can cache flush INODE to disk for us. */
      //     /* Can we assume it's in the cache? */
      //     /* If not, doesn't that mean it's already been flushed to disk? */
      //     /* Do we have to block_write ourselves? */
      //     /* Do we write back to disk if we're removing? */
      //         /* According to chatgpt - we should write back the inode
      //               ensures file recoverability*
      //            But do we also write back the data sectors? /

      //     /* Flush to disk*/
      //   }
      free (inode); 
    }
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
      /* TODO: read the correct sector now that we indirectly address */
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;
      
      cache_read (sector_idx, buffer + bytes_read, chunk_size, sector_ofs);
      
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
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt)
    return 0;

  while (size > 0) 
    {
      /* TODO: write to the correct sector now that we indirectly address */
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      cache_write (sector_idx, buffer + bytes_written, chunk_size, sector_ofs);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  /* TODO: do we need a lock?*/
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  /* TODO: do we need a lock?*/
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  /* TODO: do we need a lock?*/
  return inode->length;
}