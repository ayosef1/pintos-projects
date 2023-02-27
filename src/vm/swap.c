#include <bitmap.h>
#include "devices/block.h"
#include "threads/synch.h"
#include "vm/swap.h"

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
swap_try_read (size_t start_id, void *upage)
{
    bool ret = true;
    lock_acquire (&swap_lock);
    for (size_t id = start_id; id < SECTORS_PER_SLOT; id++)
        {
            if (!bitmap_test (used_map, start_id))
                {
                    ret = false;
                    break;
                }
            block_read (swap_block, id, upage + id * SECTORS_PER_SLOT);
            bitmap_reset (used_map, id);
        }
    lock_release (&swap_lock);
    return ret;
}

size_t
swap_write (void *upage)
{
    lock_acquire (&swap_lock);

    size_t start_id = bitmap_scan_and_flip (used_map, 0, 8, true);
    if (start_id == BITMAP_ERROR)
        PANIC ("Swap is full");
    
    for (block_sector_t id = start_id; id < SECTORS_PER_SLOT; id++)
        {
            block_write (swap_block, id, upage + id * SECTORS_PER_SLOT);
        }
    lock_release (&swap_lock);
    return start_id;
}

void 
swap_free (size_t start_id)
{
    lock_acquire (&swap_lock);
    for (size_t id = start_id; id < SECTORS_PER_SLOT; id++)
        {
            bitmap_reset (used_map, id);
        }
    lock_release (&swap_lock);
}