#include <string.h>
#include <stdio.h>
#include "filesys/cache.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"

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
        struct condition no_writers;    /* For write back, signal when done 
                                           write_refs is zero. */
        struct lock lock;               /* Lock held while evicting or writing
                                           back. */  
        uint8_t *data;                  /* Actual cached sector, proxy for
                                           whether in use. */
    };

/* Lock for finding a new cache entry to load sector into. */
static struct lock get_new_lock;
/* Block Cache Begin. */
static struct cache_entry *cache_begin;
/* Final cache entry. */
static struct cache_entry *cache_end;
/* Clock hand for eviction. */
static struct cache_entry *clock_hand;
/* Count of number in use. */
static int cached_count;
/* Filesystem device. */
extern struct block *fs_device;

static struct cache_entry *cache_get_sector (block_sector_t sector, bool write,
                                             bool read_ahead);
static struct cache_entry *cache_add_sector (block_sector_t sector, bool write,
                                             bool read_ahead);
static struct cache_entry *cache_alloc (void);
static struct cache_entry *evict_cache_entry (void);
static void tick_clock_hand (void);
static void read_ahead_fn (void *aux);
static void write_back_fn (void *aux UNUSED);

void
cache_init (void)
{
    struct cache_entry * cur;

    cache_begin = calloc (MAX_CACHE_ENTRIES, sizeof (struct cache_entry));
    if (cache_begin == NULL)
        PANIC ("Unable to allocate filesys cache");
    
    /* Initializing statics. */
    clock_hand = cache_begin;
    cache_end = cache_begin + MAX_CACHE_ENTRIES;
    lock_init (&get_new_lock);
    cached_count = 0;

    for (cur = cache_begin; cur < cache_end; cur++)
        {
            lock_init (&cur->lock);
            cond_init (&cur->no_writers);
            cur->data = malloc (BLOCK_SECTOR_SIZE);
            if (cur->data == NULL)
                PANIC ("Unable to allocate cache block.");
            /* All other members initialized to 0. */
        }

    /* Create the cleanup thread. */
    thread_create ("write_back", PRI_DEFAULT, write_back_fn, NULL);
}

void
cache_read (block_sector_t sector, void *buffer, off_t size, off_t ofs)
{
    struct cache_entry *e = cache_get_sector (sector, false, false);
    memcpy (buffer, e->data + ofs, size);
    
    /* Update metadata on completion of read. */
    lock_acquire (&e->lock);
    ASSERT (e->total_refs > 0);
    e->total_refs--;
    lock_release (&e->lock);
}

void
cache_write (block_sector_t sector, const void *buffer, off_t size, off_t ofs)
{
    struct cache_entry *e = cache_get_sector (sector, true, false);
    memcpy (e->data + ofs, buffer, size);
    
    /* Update metadata on completion of read. */
    lock_acquire (&e->lock);
    e->dirty = true;
    ASSERT (e->total_refs > 0);
    e->total_refs--;
    if (--e->write_refs == 0)
        cond_signal (&e->no_writers, &e->lock);
    lock_release (&e->lock);
}



/* Returns a cache entry corresponding to SECTOR with up to date metadata.
   Increments the cache entry's write_cnt if WRITE flag is true. */
static struct cache_entry *
cache_get_sector (block_sector_t sector, bool write, bool read_ahead)
{
    struct cache_entry *cur;
    for (cur = cache_begin; cur < cache_end; cur++)
        {
            lock_acquire (&cur->lock);
            /* data NULL means not in use. */
            if (cur->allocated && cur->sector == sector)
                {
                    /* Only update use information if guarantee use. */
                    if (!read_ahead)
                        {
                            cur->total_refs++;
                            if (write)
                                cur->write_refs++;
                            
                            cur->accessed = true;
                        }
                    /* Later might want to keep hold of this so can set
                       all other metadata while lock held. */
                    lock_release (&cur->lock);
                    return cur;
                }
            lock_release (&cur->lock);
        }
    return cache_add_sector (sector, write, read_ahead);
}

/* Gets a new cache entry for the sector SECTOR either via allocation or
   eviction if not free blocks. It then intializes the cache entry metadata.
   If flage WRITE is true, sets write_refs to 1, otherwise 0. Returns the a
   fully populated cache entry for sector SECTOR. */
static struct cache_entry *
cache_add_sector (block_sector_t sector, bool write, bool read_ahead)
{
    lock_acquire (&get_new_lock);
    /* Allocate new cache entry. Will have lock to entry  */
    struct cache_entry *new_cache;
    bool can_allocate = cached_count < MAX_CACHE_ENTRIES;
    if (can_allocate)
        new_cache = cache_alloc ();
    /* Evict an entry. */
    else
        new_cache = evict_cache_entry ();
    
    if (new_cache == NULL)
        PANIC ("Issue with getting new frame via %s, should never retrun NULL",
               can_allocate ? "ALLOCATION" : "EVICTION");
    
    lock_release (&get_new_lock);
    ASSERT (lock_held_by_current_thread (&new_cache->lock));
    new_cache->sector = sector;

    ASSERT (new_cache->total_refs == 0);
    /* Updating correct entry use metadata. */
    if (!read_ahead)
        {
                ASSERT (new_cache->total_refs == 0);
                new_cache->total_refs = 1;
                ASSERT (new_cache->write_refs == 0);
                new_cache->write_refs = write ? 1 : 0;
                new_cache->accessed = true;
        }
    
    new_cache->dirty = false;

    block_read (fs_device, sector, new_cache->data);
    /* Schedule a thread for read ahead. */

    /* Should always have the lock here. */
    lock_release (&new_cache->lock);
    if (read_ahead)
        thread_create ("read_ahead", PRI_DEFAULT, read_ahead_fn,
                       (void *) (sector + 1));
    
    return new_cache;
}

/* Iterates through cache and returns first free entry. */
static struct cache_entry *
cache_alloc (void)
{
    struct cache_entry *candidate;
    for (candidate = cache_begin; candidate < cache_end; candidate++)
        {
            lock_acquire (&candidate->lock);
            /* data NULL means not in use. */
            if (!candidate->allocated)
                {
                    candidate->allocated = true;
                    cached_count++;
                    return candidate;
                }
            lock_release (&candidate->lock);
        }
    return NULL;
}

/* Runs the clock eviction algorithm on the array to find a frame to evict.
   Writes frame back to the filesys if it is dirty and returns pointer to
   cache_entry. */
static struct cache_entry *
evict_cache_entry (void)
{
    int loop_cnt = 0;
    struct cache_entry *clock_start = clock_hand;
    while (loop_cnt < MAX_CLOCK_LOOPS)
        {
            lock_acquire (&clock_hand->lock);
            /* If evicting, everything should be allocated? */
            if (clock_hand->allocated && clock_hand->total_refs == 0)
                {
                    /* Eviction logic. */
                    if (clock_hand->accessed)
                        {
                            clock_hand->accessed = 0;
                        }
                    else
                        {
                            /* Write back to disk. */
                            if (clock_hand->dirty)
                                block_write (fs_device, clock_hand->sector,
                                             clock_hand->data);
                            
                            struct cache_entry *evicted = clock_hand;
                            tick_clock_hand ();
                            return evicted;
                        }
                }
            lock_release (&clock_hand->lock);

            tick_clock_hand ();
            if (clock_hand == clock_start)
                loop_cnt++;
        }
    return NULL;
}

void
cache_write_to_disk (bool filesys_done)
{
    struct cache_entry *cur;
    for (cur = cache_begin; cur < cache_end; cur++)
        {
            lock_acquire (&cur->lock);
            if (cur->allocated)
                {
                    while (cur->write_refs != 0)
                        cond_wait (&cur->no_writers, &cur->lock);

                    if (cur->dirty)
                        block_write (fs_device, cur->sector, cur->data);
                    
                    if (filesys_done)
                        free (cur->data);
                    else
                        cur->dirty = 0;
                }
            
            lock_release (&cur->lock);

        }
    
    if (filesys_done)
        free (cache_begin);
}

/* Moves the clock hand forward once in the ring buffer. */
static void
tick_clock_hand (void)
{
    if (++clock_hand == cache_end)
        clock_hand = cache_begin;
}

/* Reads sector encoded in AUX into the cache. */
static void
read_ahead_fn (void *aux)
{
    block_sector_t sector = (block_sector_t) aux;
    cache_get_sector (sector, false, true);
}

static void
write_back_fn (void *aux UNUSED)
{
    while (true)
        {
            timer_sleep (WRITE_BACK_PERIOD);
            cache_write_to_disk (false);
        }
}