#include <string.h>
#include <stdio.h>
#include "filesys/cache.h"
#include "filesys/inode.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/thread.h"

#define CACHE_SIZE 64                       /* Size of buffer cache. */                             
#define MAX_CLOCK_LOOPS 2                   /* Max number of iterations 
                                               when in eviction. */     
#define WRITE_BACK_PERIOD TIMER_FREQ * 30   /* Flush cache back to disk
                                               every 30 seconds. */

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

/* Queue of sectors to be read ahead by the read ahead thread. */
struct list read_ahead_queue;
/* Sync for read_ahead_queue. */
struct lock read_ahead_lock;
/* CV to put read ahead thread to sleep if no sectors to read. */
struct condition read_ahead_cv;

static struct cache_entry *cache_add_sector (block_sector_t sector, bool new);
static struct cache_entry *cache_alloc (void);
static struct cache_entry *evict_cache_entry (void);
static void get_entry_sync (struct cache_entry *entry,
                            enum cache_use_type type,
                            bool write_back);
static void tick_clock_hand (void);
static void push_read_ahead_queue (struct r_ahead_data *r_ahead_data);
static void read_ahead_fn (void *aux UNUSED);
static void write_back_fn (void *aux UNUSED);

/* Entry in read ahead queue. */
struct r_ahead_entry 
    {
        struct r_ahead_data data;               /* Data to do read ahead. */
        struct list_elem list_elem;             /* Read ahead queue elem. */
    };

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
            cond_init (&cur->excl_done);
            cond_init (&cur->no_refs);
            cur->data = malloc (BLOCK_SECTOR_SIZE);
            if (cur->data == NULL)
                PANIC ("Unable to allocate cache block.");
            /* All other members initialized to 0. */
        }

    list_init (&read_ahead_queue);
    lock_init (&read_ahead_lock);
    cond_init (&read_ahead_cv);

    thread_create ("write_back", PRI_DEFAULT, write_back_fn, NULL);
    thread_create ("read_ahead", PRI_DEFAULT, read_ahead_fn, NULL);
}


/* Returns a cache entry corresponding to SECTOR loading from disk if 
   not already present. Applies appropriate synchronization and updates
   to the entry's metadata based on use type TYPE.
   If flag NEW is set, doesn't do initial search of cache to see if entry
   there.

   If R_AHEAD_DATA is non-null and the current sector is being read ahead
   itself, adds the R_AHEAD_DATA to the read ahead queue. */
struct cache_entry *
cache_get_entry (block_sector_t sector, enum cache_use_type type, bool new,
                 struct r_ahead_data *r_ahead_data)
{
    /* Return a new entry. */
    bool present = false;
    struct cache_entry *entry;
    /* Seach cache if present. */
    if (!new)
        {
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
        }
    
    if (!present)
        entry = cache_add_sector (sector, new);
    
    get_entry_sync (entry, type, false);
    
    /* Read ahead if it isn't present and not already reading ahead. */
    if (type != R_AHEAD && r_ahead_data != NULL)
        push_read_ahead_queue (r_ahead_data);
    
    if (type != EXCL)
        lock_release (&entry->lock);
        
    return entry;
}

/* Does the cleanup once a process has completed its use of cached block ENTRY
   with type TYPE. Specifically, it signals updates the entry's metadata and
   signals any waiting processes that can now access the cache entry.
   
   If the DIRTY flag is set, the cache entry is marked as dirty. */
void
cache_release_entry (struct cache_entry *entry, enum cache_use_type type,
                     bool dirty)
{
    switch(type)
        {
            case(EXCL):
                if (entry->shared_waiters != 0)
                    /* Wake up every shared waiting thread. */
                    cond_broadcast (&entry->excl_done, &entry->lock);
                break;
            case (SHARE):
                lock_acquire (&entry->lock);
                entry->shared_refs--;
                /* Signal one excl waiter at a time. */
                if (entry->shared_refs == 0 && entry->excl_waiters != 0)
                    cond_signal (&entry->no_refs, &entry->lock);
                break;
            default:
                break;
        }

    if (dirty)
        entry->dirty = true;
    
    lock_release (&entry->lock);
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
                    get_entry_sync (cur, EXCL, false);
                    if (cur->dirty)
                        {
                            block_write (fs_device, cur->sector, cur->data);
                            cur->dirty = false;
                        }
                    else if (filesys_done)
                        {
                            cur->allocated = false;
                            free (cur->data);
                        }

                    cache_release_entry (cur, EXCL, false);
                }
            else
                lock_release (&cur->lock);

        }
    
    if (filesys_done)
        free (cache_begin);
}

/* Gets a new cache entry for the sector SECTOR either via allocation or
   eviction if no free cache entries and initializes metadata. If flag NEW
   is set no read from disk and just zeroes out acquired cache entry. */
static struct cache_entry *
cache_add_sector (block_sector_t sector, bool new)
{
    struct cache_entry *new_entry;
    lock_acquire (&get_new_lock);
    /* Second check to account for race between two threads loading same
       block sector into buffer cache. */
    if (!new)
        {
            for (new_entry = cache_begin; new_entry < cache_end; new_entry++)
                {

                    /* No fine grain lock required here becuase must have the
                    get_new_lock lock to change these members. */
                    if (new_entry->allocated && new_entry->sector == sector)
                        {
                            /* Ordering very important. Acquire the the entry 
                            lock before releasing get_new_lock so entry isn't 
                            evicted between these two steps. */
                            lock_acquire (&new_entry->lock);
                            goto done;
                        }
                }
        }

    bool can_allocate = cached_count < CACHE_SIZE;
    if (can_allocate)
        new_entry = cache_alloc ();
    else
        new_entry = evict_cache_entry ();
    
    if (new_entry == NULL)
        PANIC ("Issue with getting new frame via %s, should never retrun NULL",
               can_allocate ? "ALLOCATION" : "EVICTION");
    
    new_entry->sector = sector;

    if (new)
        memset (new_entry->data, 0, BLOCK_SECTOR_SIZE);
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
                    get_entry_sync (clock_hand, EXCL, true);
                    /* Once executing this step it is guaranteed that if the
                       accessed bit is not set that there is no one waiting
                       for this entry. This is because if there are others
                       waiting, they acquired while sleeping in get_entry_sync.
                       If you sleep in get_entry_sync it means it was used
                       before and so accessed will be set. Therefore no races
                       starvation of threads waiting on this entry. */
                    if (clock_hand->accessed)
                        {
                            clock_hand->accessed = false;
                            cache_release_entry (clock_hand, EXCL, false);
                        }
                    else
                        {
                            if (clock_hand->dirty)
                                {
                                    block_write (fs_device, clock_hand->sector,
                                                 clock_hand->data);
                                    clock_hand->dirty = false;
                                }

                            struct cache_entry *evicted = clock_hand;
                            tick_clock_hand ();
                            return evicted;
                        }
                }
            else
                lock_release (&clock_hand->lock);

            tick_clock_hand ();
            if (clock_hand == clock_start)
                loop_cnt++;
        }
    return NULL;
}

/* Synchronizes use of the cache entry ENTRY after the lock has been
   acquired based on the TYPE of use and updates the ENTRY's metadata
   depending on whether process waits or gains access.
   
   If type is EXCL:
    Waits until the number of shared users has dropped to 0.
   If type is SHARE
    Checks that there aren't any processes already waiting exclusive access.
    If there are, waits to be signaled after the first process wanting
    exclusive access has completed.

    Otherwise, no syncrhonization needed.
    
    If the WRITE_BACK flag is set, the accessed bit does not need
    to be set. */
static void
get_entry_sync (struct cache_entry *entry, enum cache_use_type type,
                bool write_back)
{
    switch (type)
        {
            case (EXCL):
                if (entry->shared_refs != 0 || entry->shared_waiters != 0)
                    {
                        entry->excl_waiters++;
                        /* While to deal with supurious wakeup. */
                        do
                            cond_wait (&entry->no_refs, &entry->lock);
                        while (entry->shared_refs != 0);
                        entry->excl_waiters--;
                    }
                break;
            case (SHARE):
                if (entry->excl_waiters != 0)
                    {
                        entry->shared_waiters++;
                        cond_wait (&entry->excl_done, &entry->lock);
                        entry->shared_waiters--;
                    }
                entry->shared_refs++;
                entry->accessed = true;
                break;
            default:
                return;
        }
    if (!write_back)
        entry->accessed = true;
    
}

/* Moves the clock hand forward once in the ring buffer. */
static void
tick_clock_hand (void)
{
    if (++clock_hand == cache_end)
        clock_hand = cache_begin;
}

/* Pushes the information to perform read ahead from R_AHEAD_DATA onto the
   read ahead queue. */
static void
push_read_ahead_queue (struct r_ahead_data *r_ahead_data)
{
    struct r_ahead_entry *e = malloc (sizeof (struct r_ahead_entry));
    if (e != NULL)
        {
            memcpy (&e->data, r_ahead_data, sizeof (struct r_ahead_data));
            lock_acquire (&read_ahead_lock);
            list_push_back (&read_ahead_queue, &e->list_elem);
            cond_signal (&read_ahead_cv, &read_ahead_lock);
            lock_release (&read_ahead_lock);
        }
}

/* Reads entry from the read ahead queue one at a time and reads entry into the
   buffer cache. Specifically gets an inode and an offset and determines the 
   data's corresponding block sector. It is guaranteed that this block is not
   beyond the length of the inode for a write and not beyond the length of the 
   file/directory if a read.

   Reads entries one at a time to prevent starvation of threads trying to add
   to the queue. Frees the entry after using. */
static void
read_ahead_fn (void *aux UNUSED)
{
    while (true)
        {
            lock_acquire (&read_ahead_lock);
            while (list_empty (&read_ahead_queue))
                cond_wait (&read_ahead_cv, &read_ahead_lock);
            
            struct list_elem *e = list_pop_front (&read_ahead_queue);
            lock_release (&read_ahead_lock);
            struct r_ahead_entry *entry = list_entry (e, struct r_ahead_entry,
                                                      list_elem);
                                                      
            block_sector_t sector = inode_get_sector (entry->data.inode_sector,
                                                      entry->data.ofs, true);
            if (sector != 0 && free_map_present (sector))
                cache_get_entry (sector, R_AHEAD, false, NULL);

            free (entry);
        }
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