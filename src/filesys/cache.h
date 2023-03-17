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

/* Different use types that determine how to synchronize access to
   the cache entry on a call to cache_get_entry. */
enum cache_use_type
    {
        EXCL,                         /* Exclusive write. */
        SHARE,                        /* Non exclusive write. */
        R_AHEAD,                      /* Read ahead. */
    };

/* An entry in the buffer cache. */
struct cache_entry
    {
        block_sector_t sector;          /* Block sector represented. */
        int shared_refs;                /* Number of shared references. */
        int shared_waiters;             /* Number of processes waiting for
                                           shared access. */
        int excl_waiters;               /* Number processes waiting for
                                           exclusive access. */
        bool accessed;                  /* Accessed bit for eviction. */
        bool dirty;                     /* Dirty bit for eviction and write
                                           back. */
        bool allocated;                 /* Whether cache_entry has been 
                                           allocated. */
        struct lock lock;               /* Lock to synchronize access to cache
                                           entry metadata. */
        struct condition excl_done;     /* Signal waiting. */
        struct condition no_refs;       /* For eviction, when on. */
        uint8_t *data;                  /* Actual cached sector. */
    };


void cache_init (void);
struct cache_entry *cache_get_entry (block_sector_t sector,
                                     enum cache_use_type type,
                                     bool new);
void cache_release_entry (struct cache_entry *e, enum cache_use_type type);
void cache_write_to_disk (bool filesys_done);

#endif /* filesys/directory.h */