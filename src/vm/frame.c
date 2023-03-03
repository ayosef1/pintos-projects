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
void tick_clock_hand (void);
static void clear_frame (void * vaddr, bool lock_held);
static struct fte *frame_lookup (uint8_t *kpage);

void 
frame_table_init (uint8_t *user_pool_base, size_t num_user_pages)
{
    num_frames = num_user_pages;
    user_kpage_base = user_pool_base;
}

/* Initializes Frame Table to allow for paging. */
void
frame_table_create (void)
{
    size_t frame_table_size = sizeof (struct fte) * num_frames;
    frame_table_base = malloc (frame_table_size);
    memset (frame_table_base, 0, frame_table_size);
    if (frame_table_base == NULL)
        PANIC ("Can't allocate for frame table.");
    printf ("%zu frames available in frame table of size %zu.\n", num_frames,
            frame_table_size);
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
   by evicting using the clock algorithm */
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

struct spte *
frame_get_spte (void * kpage, bool hold_lock)
{
    struct spte * spte;
    struct fte *fte = frame_lookup (kpage);
    if (kpage == NULL)
        return NULL;

    lock_acquire (&fte->lock);
    spte = fte->spte;
    if (!hold_lock)
        lock_release (&fte->lock);
    
    return spte;
}

/* Eviction algorithm that implements the */
static void *
evict (bool zeroed)
{   
    lock_acquire (&eviction_lock);
    while (true)
        {
            if (!lock_try_acquire (&clock_hand->lock))
            {
                tick_clock_hand ();
            }
            else if (!clock_hand->pinned)
                {
                    if (pagedir_is_accessed (clock_hand->pd, clock_hand->upage))
                    {
                        pagedir_set_accessed (clock_hand->pd, clock_hand->upage,
                                              0);
                        lock_release (&clock_hand->lock);
                        tick_clock_hand ();
                        continue;
                    }
                    void *kpage = user_kpage_base + 
                                  ((clock_hand - frame_table_base) * PGSIZE);
                    spt_evict_upage (clock_hand->pd, clock_hand->upage,
                                     clock_hand->spte);
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
}

void
tick_clock_hand (void)
{
    if (clock_hand + 1 == frame_table_end)
        clock_hand = frame_table_base;
    else
        ++clock_hand;
}

/* Frees the physical frame associated with kernel virtual page KPAGE
   and all associated memory. */
void
frame_free_page (void *kpage, bool lock_held)
{
    clear_frame (kpage, lock_held);
    palloc_free_page (kpage);
}

/* Removes the frame table entry that corresponds whose physical address 
   corresponds to the kernel virtual page KPAGE from the frame table and
   frees the associated memory. Returns true if the entry was removed and
   false if the entry could not be found in the table. */
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
    /* Coudl change to do cleaning here. I.e. spt_remove_page. */
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
    off_t table_ofs = ((kpage - user_kpage_base) / PGSIZE);
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
    ASSERT (fte->pinned)
    /* Don't need a lock here because no one accessing this data yet since
       it is pinned. */
    fte->upage = upage;
    fte->pd = pd;
    fte->spte = spte;
    if (!keep_pinned)
        fte->pinned = false;
    lock_release (&fte->lock);
}