#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"
#include "devices/timer.h"
#include "filesys/off_t.h"
#include "threads/synch.h"

/* Size of buffer cache. */
#define CACHE_SIZE 64
/* Maximum number of iterations over buffer cache for eviction. */                            
#define MAX_CLOCK_LOOPS 2
/* Number of ticks between periodic buffer cache write back. */
#define WRITE_BACK_PERIOD TIMER_FREQ * 10

enum cache_use_type
    {
        W_EXCL,                         /* Exclusive write. */
        W_SHARE,                        /* Non exclusive write. */
        R_SHARE,                        /* Read of data. */
        R_AHEAD                         /* Read ahead. */
    };

/* An entry in the buffer cache. */
struct cache_entry
    {
        block_sector_t sector;          /* Block sector represented. */
        int write_refs;                 /* Total number of writer references. */
        int total_refs;                 /* Total number of references. */
        bool accessed;                  /* Accessed bit for eviction. */
        bool dirty;                     /* Dirty bit for eviction and write
                                           back. */
        bool allocated;                 /* Whether cache_entry has been 
                                           allocated. */
        struct condition no_writers;    /* For write back, signal when 
                                           write_refs decremented to zero. */
        struct condition no_refs;       /* For eviction, when on*/
        struct lock lock;               /* Lock to synchronize access to cache
                                           entry metadata. */  
        uint8_t *data;                  /* Actual cached sector. */
    };

struct cache_entry;


void cache_init (void);
struct cache_entry *cache_get_entry (block_sector_t sector,
                                     enum cache_use_type type);
struct cache_entry *cache_add_sector (block_sector_t sector, bool new);
void cache_read (block_sector_t sector, void *buffer, off_t size, off_t ofs);
void cache_write (block_sector_t sector, const void *buffer, off_t size,
                  off_t ofs);
void cache_release_entry (struct cache_entry *e, enum cache_use_type type);
void cache_write_to_disk (bool filesys_done);

#endif /* filesys/directory.h */