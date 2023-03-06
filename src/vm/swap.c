#include <bitmap.h>
#include "devices/block.h"
#include "filesys/off_t.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "vm/swap.h"
#include <stdio.h>

/* Mutual exclusion of table and block_sector. */
struct lock swap_lock;
/* Bitmap of free slots in swap device. */  
struct bitmap *used_map;
/* Block device interface. */
struct block * swap_block;

/* Initializes the swap tables bit map to be the size of the number
   of sectors in swap. */
void
swap_init (void)
{
    swap_block = block_get_role (BLOCK_SWAP);
    lock_init (&swap_lock);
    used_map = bitmap_create (block_size (swap_block));
}

/* Reads a page from swap starting from sector with id START_ID into
   the frame with kernel virtual address KPAGE. If there is not enough
   room in swap, returns false, otherwise returns true. */
bool
swap_try_read (size_t start_id, uint8_t *kpage)
{
    bool ret;
    lock_acquire (&swap_lock);
    ret = bitmap_all (used_map, start_id, SECTORS_PER_SLOT);
    if (ret)
    {
        for (off_t ofs = 0; ofs < SECTORS_PER_SLOT; ofs++)
            {
                block_read (swap_block, start_id + ofs, kpage + ofs * 
                            BLOCK_SECTOR_SIZE);
                bitmap_reset (used_map, start_id + ofs);
            }        
    }
    lock_release (&swap_lock);
    return ret;
}

/* Writes the contents of frame at kernel virtual address KPAGE into the next
   available contiguous page length region of swap. Returns the id of the
   first sector used to store the page. */
size_t
swap_write (uint8_t *kpage)
{
    lock_acquire (&swap_lock);
    size_t start_id = bitmap_scan_and_flip (used_map, 0, SECTORS_PER_SLOT,
                                            false);
    if (start_id == BITMAP_ERROR)
        PANIC ("Swap is full");
    for (off_t ofs = 0; ofs < SECTORS_PER_SLOT; ofs++)
        block_write (swap_block, start_id + ofs, kpage + ofs * 
                        BLOCK_SECTOR_SIZE);
    lock_release (&swap_lock);
    return start_id;
}

/* Frees the sectors in swap space previously allocated
    for the given START_ID and the consecutive SECTORS_PER_SLOT sectors. */
void 
swap_free (size_t start_id)
{
    lock_acquire (&swap_lock);
    bitmap_set_multiple (used_map, start_id, SECTORS_PER_SLOT, false) ;
    lock_release (&swap_lock);
}

