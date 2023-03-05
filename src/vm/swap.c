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

/* Initializes Swap Table to allow for swapping. */
void
swap_init (void)
{
    swap_block = block_get_role (BLOCK_SWAP);
    lock_init (&swap_lock);
    used_map = bitmap_create (block_size (swap_block));
}

/* Tries to read a swap slot from the swap block. */
bool
swap_try_read (size_t start_id, uint8_t *kpage)
{
    bool ret = true;
    lock_acquire (&swap_lock);
    for (off_t ofs = 0; ofs < SECTORS_PER_SLOT; ofs++)
        {
            if (!bitmap_test (used_map, start_id + ofs))
                {
                    ret = false;
                    break;
                }
            block_read (swap_block, start_id + ofs, kpage + ofs * BLOCK_SECTOR_SIZE);
            bitmap_reset (used_map, start_id + ofs);
        }
    lock_release (&swap_lock);
    return ret;
}

/* Writes the contents of the given kernel page to swap block and returns
    the index of the first sector in the swap block that was written to. */
size_t
swap_write (uint8_t *kpage)
{
    lock_acquire (&swap_lock);
    size_t start_id = bitmap_scan_and_flip (used_map, 0, SECTORS_PER_SLOT,
                                            false);
    if (start_id == BITMAP_ERROR)
        PANIC ("Swap is full");

    for (off_t ofs = 0; ofs < SECTORS_PER_SLOT; ofs++)
        {
            block_write (swap_block, start_id + ofs, kpage + ofs * BLOCK_SECTOR_SIZE);
        }
    lock_release (&swap_lock);
    return start_id;
}

/* Frees the sectors in swap space previously allocated
    for the given start_id and the consecutive SECTORS_PER_SLOT sectors. */
void 
swap_free (size_t start_id)
{
    lock_acquire (&swap_lock);
    bitmap_set_multiple (used_map, start_id, SECTORS_PER_SLOT - 1, false) ;
    lock_release (&swap_lock);
}