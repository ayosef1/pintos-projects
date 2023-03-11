#include "devices/block.h"
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

static struct cache_entry *cache_get_sector (block_sector_t sector, bool write);
static struct cache_entry *cache_get_new (block_sector_t sector, bool write);
static struct cache_entry *cache_alloc (void);
static struct cache_entry *cache_evict (void);
static void tick_clock_hand (void);

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
    /* thread_create ('write_back_thread', PRI_DEFAULT, write_back_fn, NULL); */
}

/* Returns a cache entry corresponding to SECTOR with up to date metadata.
   Increments the cache entry's write_cnt if WRITE flag is true. */
static struct cache_entry *
cache_get_sector (block_sector_t sector, bool write)
{
    struct cache_entry *cur;
    for (cur = cache_begin; cur < cache_end; cur++)
        {
            lock_acquire (&cur->lock);
            /* data NULL means not in use. */
            if (cur->data != NULL && cur->sector == sector)
                {
                    /* Maybe don't want to. */
                    cur->total_refs++;
                    if (write)
                        cur->write_refs++;
                    /* Later might want to keep hold of this so can set
                       all other metadata while lock held. */
                    lock_release (&cur->lock);
                    return cur;
                }
        }
    return cache_get_new (sector, write);
}

/* Gets a new cache entry for the sector SECTOR either via allocation or
   eviction if not free blocks. It then intializes the cache entry metadata.
   If flage WRITE is true, sets write_refs to 1, otherwise 0. Returns the a
   fully populated cache entry for sector SECTOR. */
static struct cache_entry *
cache_get_new (block_sector_t sector, bool write)
{
    lock_acquire (&get_new_lock);
    /* Allocate new cache entry. Will have lock to entry  */
    struct cache_entry *new_cache;
    if (cached_count < MAX_CACHE_ENTRIES)
        new_cache = cache_alloc ();
    /* Evict an entry. */
    else
        new_cache = cache_evict ();
    
    if (new_cache == NULL)
        PANIC ("Issue with getting new frame, should never retrun NULL");
    
    ASSERT (lock_held_by_current_thread (&new_cache->lock));
    new_cache->sector = sector;

    /* Setting correct ref counts. */
    ASSERT (new_cache->total_refs == 0);
    new_cache->total_refs = 1;
    new_cache->write_refs = write ? 1 : 0;

    new_cache->accessed = false;
    new_cache->dirty = false;

    block_read (fs_device, sector, new_cache->data);
    
    /* Should always have the lock here. */
    lock_release (&new_cache->lock);
    lock_release (&get_new_lock);
    return  new_cache;
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
            if (candidate->sector == 0)
                {
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
cache_evict (void)
{
    int loop_cnt = 0;
    struct cache_entry *clock_start = clock_hand;
    while (loop_cnt < MAX_CLOCK_LOOPS)
        {
            lock_acquire (&clock_hand->lock);
            if (clock_hand->sector != 0 && clock_hand->total_refs == 0)
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
                            
                            tick_clock_hand ();
                            return clock_hand - 1;
                        }
                }
            lock_release (&clock_hand->lock);
            tick_clock_hand ();
            if (clock_hand == clock_start)
                loop_cnt++;
        }
    PANIC ("Unable to evict cache entry");
    return NULL;
}

/* Moves the clock hand forward once in the ring buffer. */
static void
tick_clock_hand (void)
{
    if (++clock_hand == cache_end)
        clock_hand = cache_begin;
}