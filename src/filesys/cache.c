#include <string.h>
#include <stdio.h>
#include "filesys/cache.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/thread.h"


/* Synchronization when loading block sector into cache for race between
   two processes that both can't find block sector in cache. Prevents double
   loading of that block sector. */
static struct lock get_new_lock;
/* Buffer Cache Begin. */
static struct cache_entry *cache_begin;
/* Buffer cache end (for iteration). */
static struct cache_entry *cache_end;
/* Clock hand for eviction. */
static struct cache_entry *clock_hand;
/* Count of number in use. */
static int cached_count;
/* Filesystem device for filesys R/W. */
extern struct block *fs_device;

static struct cache_entry *cache_alloc (void);
static struct cache_entry *evict_cache_entry (void);
static void tick_clock_hand (void);
static void read_ahead_fn (void *aux);
static void write_back_fn (void *aux UNUSED);

void
cache_init (void)
{
    struct cache_entry * cur;

    cache_begin = calloc (CACHE_SIZE, sizeof (struct cache_entry));
    if (cache_begin == NULL)
        PANIC ("Unable to allocate filesys cache");
    
    /* Initializing statics. */
    clock_hand = cache_begin;
    cache_end = cache_begin + CACHE_SIZE;
    lock_init (&get_new_lock);
    cached_count = 0;

    for (cur = cache_begin; cur < cache_end; cur++)
        {
            lock_init (&cur->lock);
            cond_init (&cur->no_writers);
            cond_init (&cur->no_refs);
            cur->data = malloc (BLOCK_SECTOR_SIZE);
            if (cur->data == NULL)
                PANIC ("Unable to allocate cache block.");
            /* All other members initialized to 0. */
        }

    /* Create the cleanup thread. */
    thread_create ("write_back", PRI_DEFAULT, write_back_fn, NULL);
}

/* Reads SIZE bytes from cache entry for block sector SECTOR into BUFFER, 
   starting at position OFS. Updates corresponding cache entry metadata,
   including loading sector into cache if not present. */
void
cache_read (block_sector_t sector, void *buffer, off_t size, off_t ofs)
{
    struct cache_entry *e = cache_get_entry (sector, R_SHARE);
    memcpy (buffer, e->data + ofs, size);
    
    /* Update metadata on completion of read. */
    lock_acquire (&e->lock);
    ASSERT (e->total_refs > 0);
    if (--e->total_refs == 0)
        cond_broadcast (&e->no_refs, &e->lock);
    lock_release (&e->lock);
}

/* Writes SIZE bytes from BUFFER into cached block sector SECTOR, starting at 
   position OFS. Updates corresponding cache entry metadata, including
   loading sector into cache if not present. */
void
cache_write (block_sector_t sector, const void *buffer, off_t size, off_t ofs)
{
    struct cache_entry *e = cache_get_entry (sector, W_SHARE);
    memcpy (e->data + ofs, buffer, size);
    
    /* Update metadata on completion of read. */
    lock_acquire (&e->lock);
    e->dirty = true;
    /* Signals to write-back thread that can start writing back. */
    if (--e->write_refs == 0)
        cond_signal (&e->no_writers, &e->lock);
    if (--e->total_refs == 0)
        cond_signal (&e->no_refs, &e->lock);
    lock_release (&e->lock);
}



/* Returns a cache entry corresponding to SECTOR loading from disk if 
   not already present. Increments the cache entry's write_cnt if WRITE flag 
   is true. If READ_AHEAD flag set, asynchronously reads next block sector if 
   block sector SECTOR is not yet cached.*/
struct cache_entry *
cache_get_entry (block_sector_t sector, enum cache_use_type type)
{
    struct cache_entry *entry;
    bool present = false;
    for (entry = cache_begin; entry < cache_end; entry++)
        {
            lock_acquire (&entry->lock);
            if (entry->allocated && entry->sector == sector)
                {
                    present = true;
                    break;
                }
            lock_release (&entry->lock);
        }
    
    if (!present)
        entry = cache_add_sector (sector, false);
    
    switch (type)
        {
            case (W_EXCL):
                while (entry->total_refs != 0)
                    cond_wait (&entry->no_refs, &entry->lock);
            case (W_SHARE):
                entry->total_refs++;
                entry->write_refs++;
                entry->accessed = true;
                break;
            case (R_SHARE):
                entry->total_refs++;
                entry->accessed = true;
                break;
            default:
                if (!present)
                    thread_create ("read_ahead", PRI_DEFAULT, read_ahead_fn,
                                   (void *) (sector + 1));
            /* Do nothing for READ AHEAD*/
        }
    
    if (type != W_EXCL)
        lock_release (&entry->lock);
    
    return entry;
}

/* Gets a new cache entry for the sector SECTOR either via allocation or
   eviction if no free cache entries and initializes metadata. */
struct cache_entry *
cache_add_sector (block_sector_t sector, bool new)
{
    lock_acquire (&get_new_lock);

    /* Second check to account for race between two threads loading same
       block sector into buffer cache. */
    struct cache_entry *new_entry;
    for (new_entry = cache_begin; new_entry < cache_end; new_entry++)
        {

            /* No fine grain lock required here becuase must have the
               get_new_lock lock to change these members. */
            if (new_entry->allocated && new_entry->sector == sector)
                {
                    /* Ordering very important. Acquire the the entry lock
                       before releasing get_new_lock so entry isn't evicted
                       between these two steps. */
                    lock_acquire (&new_entry->lock);
                    goto done;
                }
        }

    bool can_allocate = cached_count < CACHE_SIZE;
    if (can_allocate)
        new_entry = cache_alloc ();
    /* Evict an entry. */
    else
        new_entry = evict_cache_entry ();
    
    if (new_entry == NULL)
        PANIC ("Issue with getting new frame via %s, should never retrun NULL",
               can_allocate ? "ALLOCATION" : "EVICTION");
    
    ASSERT (lock_held_by_current_thread (&new_entry->lock));
    ASSERT (new_entry->write_refs == 0);
    ASSERT (new_entry->total_refs == 0);
    new_entry->sector = sector;

    if (new)
        {
            new_entry->accessed = true;
            memset (new_entry->data, 0, BLOCK_SECTOR_SIZE);
        }
    else
        block_read (fs_device, sector, new_entry->data);

    done:
        lock_release (&get_new_lock);
        return new_entry;
}

/* Iterates through cache and returns first free entry. Returns NULL if
   no free entries. */
static struct cache_entry *
cache_alloc (void)
{
    struct cache_entry *candidate;
    for (candidate = cache_begin; candidate < cache_end; candidate++)
        {
            lock_acquire (&candidate->lock);
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
            if (clock_hand->allocated)
                {
                    while (clock_hand->total_refs != 0)
                        cond_wait (&clock_hand->no_refs, &clock_hand->lock);
                    
                    if (clock_hand->accessed)
                        {
                            clock_hand->accessed = 0;
                        }
                    else
                        {
                            /* Write back to disk only when dirty. */
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

/* Writes all dirty buffer cache entries to disk. If FILESYS_DONE flag set,
   frees memory associated with cached block after written back and then
   frees cache before returning. */
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

/* Reads block sector encoded in AUX into the cache. */
static void
read_ahead_fn (void *aux)
{
    block_sector_t sector = (block_sector_t) aux;
    cache_get_entry (sector, R_AHEAD);
}

/* Periodically writes all dirty buffer cache entries to disk. */
static void
write_back_fn (void *aux UNUSED)
{
    while (true)
        {
            timer_sleep (WRITE_BACK_PERIOD);
            cache_write_to_disk (false);
        }
}