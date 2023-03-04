#ifndef VM_SWAP_H
#define VM_SWAP_H

#include "threads/vaddr.h"
#include "devices/block.h"

#define SECTORS_PER_SLOT PGSIZE / BLOCK_SECTOR_SIZE

void swap_init (void);
bool swap_try_read (size_t swap_id, uint8_t *kpage);
size_t swap_write (uint8_t *kpage);
void swap_free (size_t swap_id);

#endif /* vm/swap.h */