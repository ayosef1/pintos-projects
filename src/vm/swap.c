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

void
swap_init (void)
{
    swap_block = block_get_role (BLOCK_SWAP);
    lock_init (&swap_lock);
    used_map = bitmap_create (block_size (swap_block));
}

bool
swap_try_read (size_t start_id, uint8_t *kpage)
{
    bool ret = true;
    // printf("SWAP READ ACQUIRED\n");
    lock_acquire (&swap_lock);
    for (off_t ofs = 0; ofs < SECTORS_PER_SLOT; ofs++)
        {
            if (!bitmap_test (used_map, start_id + ofs))
                {
                    ret = false;
                    break;
                }
            block_read (swap_block, start_id, kpage + ofs * BLOCK_SECTOR_SIZE);
            bitmap_reset (used_map, start_id + ofs);
        }
    // printf("SWAP READ RELEASED %d\n", ret);
    lock_release (&swap_lock);
    return ret;
}

size_t
swap_write (uint8_t *kpage)
{
    // printf("SWAP WRITE ACQUIRED\n");
    lock_acquire (&swap_lock);
    size_t start_id = bitmap_scan_and_flip (used_map, 0, SECTORS_PER_SLOT,
                                            false);
    if (start_id == BITMAP_ERROR)
        PANIC ("Swap is full");

    for (off_t ofs = 0; ofs < SECTORS_PER_SLOT; ofs++)
        {
            block_write (swap_block, start_id, kpage + ofs * BLOCK_SECTOR_SIZE);
        }
    lock_release (&swap_lock);
    // printf("SWAP WRITE RELEASE\n");
    return start_id;
}

void 
swap_free (size_t start_id)
{
    // printf("SWAP LOCK ACQUIRED\n");
    lock_acquire (&swap_lock);
    for (size_t id = start_id; id < start_id + SECTORS_PER_SLOT; id++)
        {
            bitmap_reset (used_map, id);
        }
    lock_release (&swap_lock);
    // printf("SWAP LOCK RELEASE\n");
}