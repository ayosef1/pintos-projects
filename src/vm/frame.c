#include <hash.h>
#include <stdio.h>
#include <string.h>
#include "threads/synch.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "filesys/off_t.h"
#include "vm/frame.h"

/* Number of frames in frame table. */
static size_t num_frames;
/* Frame table Base Pointer */
static struct fte *frame_table_base;
/* Frame table Base Pointer */
static struct fte *frame_table_end;
/* Base address used to index into the frame table.*/
static uint8_t *user_kpage_base;
/* Lock to Synchronize Eviction. */
struct lock eviction_lock; 
/* Clock Hand. */
struct fte *clock_hand;

static void *evict (bool zeroed);
static void tick_clock_hand (void);
static void clear_frame (void * vaddr, bool lock_held);
static struct fte *frame_lookup (uint8_t *kpage);

/* Initializes Frame Table to have NUM_USER_PAGES frames allocated from the 
    USER_POOL_BASE to allow for paging. */
void
frame_table_init (uint8_t *user_pool_base, size_t num_user_pages)
{
    num_frames = num_user_pages;
    user_kpage_base = user_pool_base;
}

/* Creates Frame Table to allow for paging. Sets the frame to pinned to 
    avoid eviction. */
void
frame_table_create (void)
{
    size_t frame_table_size = sizeof (struct fte) * num_frames;
    frame_table_base = malloc (frame_table_size);
    memset (frame_table_base, 0, frame_table_size);
    if (frame_table_base == NULL)
        PANIC ("Can't allocate for frame table.");

    /* Initialize each lock. */
    struct fte * fte;
    for (fte = frame_table_base; fte < frame_table_base + num_frames; fte++)
        {
            lock_init (&fte->lock);
            fte->pinned = true;
        }
    
    /* Eviction initialization. */
    lock_init (&eviction_lock);
    clock_hand = frame_table_base;
    frame_table_end = frame_table_base + num_frames;
}

/* Destroys frame table by freeing memory associated with hash table. */
void frame_table_destroy (void)
{
    free (frame_table_base);
}

/* Returns a new frame for the user process. It first tries to acquire a
   new frame from the user pool. If there are none left it acquires it
   by evicting using the clock algorithm. Returns an address to a physical 
   frame containing ZEROED? bytes. */
void *
frame_get_page (bool zeroed)
{
    void *kpage;

    /* Palloc atomically allocates pages. */
    kpage = palloc_get_page (PAL_USER | (zeroed ? PAL_ZERO : 0));
    if (kpage == NULL)
        {   
            kpage = evict (zeroed);
        }
    return kpage;
}

/* Returns the spte currently mapped to the frame KPAGE. Maintains the lock 
    in the frame struck if specified to do so by HOLD_LOCK. Returns null if
    no such spte exists. */
struct spte *
frame_get_spte (void *kpage, const void *upage, bool hold_lock)
{
    struct spte * spte;
    struct fte *fte = frame_lookup (kpage);
    if (kpage == NULL)
        return NULL;

    lock_acquire (&fte->lock);
    spte = fte->spte;
    if (spte->upage != upage)
    {
        lock_release (&fte->lock);
        return pagedir_get_spte (thread_current ()->pagedir, upage, hold_lock);
    }
    if (!hold_lock)
        lock_release (&fte->lock);
    
    return spte;
}

/* Implements the clock algorithm that approximates LRU for  eviction of frames. 
    Gives each unpinned, mapped frame a second chance before being evicted. 
    Returns pointer to physical frame with the bytes ZEROED? */
static void *
evict (bool zeroed)
{   
    int i = 0;
    int max_iterations = 2 * num_frames;

    lock_acquire (&eviction_lock);
    for (i = 0; i < max_iterations; i++)
        {
            if (!lock_try_acquire (&clock_hand->lock))
            {
                tick_clock_hand ();
            }
            else if (!clock_hand->pinned)
                {
                    /* If the page directory entry for the clock hand has been 
                        accessed, reset the accessed bit and continue iterating 
                        through the clock algorithm. This implements the second
                        chance replacement. */
                    if (pagedir_is_accessed (clock_hand->pd, clock_hand->upage))
                    {
                        pagedir_set_accessed (clock_hand->pd, clock_hand->upage,
                                              0);
                        lock_release (&clock_hand->lock);
                        tick_clock_hand ();
                        continue;
                    }

                    /* Pagedir entry has not been accessed, evict and return 
                        the newly evicted frame. */
                    void *kpage = user_kpage_base + 
                                  ((clock_hand - frame_table_base) * PGSIZE);
                    spt_evict_kpage (kpage, clock_hand->pd, clock_hand->spte);
                    clock_hand->pinned = true;
                    lock_release (&clock_hand->lock);
                    tick_clock_hand ();
                    lock_release (&eviction_lock);

                    if (zeroed)
                        memset (kpage, 0, PGSIZE);
                    
                    return kpage;
                }
            lock_release (&clock_hand->lock);
            tick_clock_hand ();
        }
    return NULL;
}

/* Advances the clock hand to the next frame in the frame table.
    If the current clock hand position is the last frame in the frame table,
    the function wraps around to the first frame in the table. */
static void
tick_clock_hand (void)
{
    if (clock_hand + 1 == frame_table_end)
        clock_hand = frame_table_base;
    else
        ++clock_hand;
}

/* Frees the physical frame associated with kernel virtual page KPAGE
   and passes through LOCK_HELD boolean to clear_frame. */
void
frame_free_page (void *kpage, bool lock_held)
{
    clear_frame (kpage, lock_held);
    palloc_free_page (kpage);
}

/* Removes the frame table entry whose physical address 
   corresponds to the kernel virtual page KPAGE from the frame table and
   frees the associated memory. Acquires lock on fte if LOCK_HELD is false. 
   Returns true if the entry was removed and false if the entry could not be 
   found in the table. */
static void
clear_frame (void * kpage, bool lock_held)
{
    struct fte *fte = frame_lookup (kpage);

    if (!lock_held)
        lock_acquire (&fte->lock);

    if (fte->spte != NULL)
        {
            free (fte->spte);
            fte->spte = NULL;
        }
    fte->upage = NULL;
    fte-> pd = NULL;
    fte->pinned = true;
    lock_release (&fte->lock);
}

/* Returns the frame table entry that corresponds to the physical frame
   associated with the kernel virtual page KPAGE. If no such entry exists,
   returns NULL. */
struct fte *
frame_lookup (uint8_t *kpage)
{
    ASSERT (user_kpage_base <= kpage);
    off_t table_ofs = (kpage - user_kpage_base) / PGSIZE;
    return frame_table_base + table_ofs;
}

/* Sets the user virtual addres of frame, whose kernel virtual address is
   KPAGE's, to UPAGE */
void 
frame_set_udata (void *kpage, void *upage, uint32_t *pd, struct spte *spte,
                 bool keep_pinned)
{

    struct fte *fte = frame_lookup (kpage);
    lock_acquire (&fte->lock);
    ASSERT (fte->pinned);
    /* Don't need a lock here because no one accessing this data yet since
       it is pinned. */
    fte->upage = upage;
    fte->pd = pd;
    fte->spte = spte;
    if (!keep_pinned)
        fte->pinned = false;
    lock_release (&fte->lock);
}

/* Set the pinned status to PIN of the frame pointed to by UPAGE. Uses the page 
    directory PD to scan the mapping. Returns if no such page or fte exists. */
void
frame_set_pin (void *upage, uint32_t *pd, bool pin)
{
    void *kpage;
    struct fte *fte;
    
    kpage = pagedir_get_page (pd, upage);
    if (kpage == NULL)
        return;

    fte = frame_lookup (kpage);
    if (kpage == NULL)
        return;
    lock_acquire (&fte->lock);
    fte->pinned = pin;
    lock_release (&fte->lock);
}