#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"
#include "devices/timer.h"
#include "filesys/off_t.h"

#define MAX_CACHE_ENTRIES 64
#define MAX_CLOCK_LOOPS 2
#define WRITE_BACK_PERIOD TIMER_FREQ * 10

void cache_init (void);
void cache_read (block_sector_t sector, void *buffer, off_t size, off_t ofs);
void cache_write (block_sector_t sector, const void *buffer, off_t size,
                  off_t ofs);
void cache_write_to_disk (bool filesys_done);

#endif /* filesys/directory.h */