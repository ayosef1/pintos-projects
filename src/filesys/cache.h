#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"
#include "devices/timer.h"
#include "filesys/off_t.h"

/* Size of buffer cache. */
#define CACHE_SIZE 64
/* Maximum number of iterations over buffer cache for eviction. */                            
#define MAX_CLOCK_LOOPS 2
/* Number of ticks between periodic buffer cache write back. */
#define WRITE_BACK_PERIOD TIMER_FREQ * 10

void cache_init (void);
void cache_read (block_sector_t sector, void *buffer, off_t size, off_t ofs);
void cache_write (block_sector_t sector, const void *buffer, off_t size,
                  off_t ofs);
void cache_write_to_disk (bool filesys_done);

#endif /* filesys/directory.h */