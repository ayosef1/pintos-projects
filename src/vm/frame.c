#include "threads/synch.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "vm/frame.h"
#include "vm/page.h"
#include "vm/swap.h"
#include "userprog/pagedir.h"

/* Frame table */
static struct list frame_table;
/* Sync for frame table */
static struct lock frame_lock;
/* Clock Algorithm Hand */
static struct list_elem *hand; 

static void insert_frame (struct fte * fte, void * kpage);
static bool delete_frame (void * vaddr);
static struct fte *frame_lookup (void *kpage);

/* Initializes Frame Table to allow for paging. */
void
frame_table_init (void)
{
    list_init (&frame_table);
    lock_init (&frame_lock);
    hand = list_begin(&frame_table);
}

/* Destroys frame table by freeing memory associated with list. */
void frame_table_destroy (void)
{
    while (!list_empty(&frame_table)) {
        struct list_elem *e = list_pop_front(&frame_table);
        struct fte *fte = list_entry(e, struct fte, elem);
        free(fte);
    }
}

/* Returns a new frame for the user process. It first tries to acquire a
   new frame from the user pool. If there are none left it acquires it
   by evicting using the clock algorithm */
void *
frame_get_page (enum palloc_flags flags)
{
    void *kpage;
    struct fte *fte;

    kpage = palloc_get_page (flags);
    if (kpage == NULL)
    {
        /* Use clock to find correct page to evict */
        lock_acquire(&frame_lock);
        kpage = frame_evict ();
        lock_release (&frame_lock);
        if (kpage == NULL)
            PANIC ("Eviction Didn't Find Page");
        /* Unload using an spte function
            i) Unload data to correct location in memory 
            ii) Set pagedir to have PTE_P as 0 */
        //PANIC("Yet to implement the unloading");
        delete_frame (kpage);
    }
    fte = malloc (sizeof (struct fte));
    insert_frame (fte, kpage);

    return kpage;
}

/* Frees the physical frame associated with kernel virtual page KPAGE
   and all associated memory. */
void
frame_free_page (void *kpage)
{
    if (!delete_frame (kpage))
        return;

    palloc_free_page (kpage);
}

/* Sets the user virtual addres of frame, whose kernel virtual address is
   KPAGE's, to UPAGE */
void 
frame_set_udata (void *kpage, void *upage, uint32_t *pd, struct spte *spte)
{
    struct fte *fte = frame_lookup (kpage);
    if (fte == NULL)
        return;
    fte->upage = upage;
    fte->pd = pd;
    fte->spte = spte;
    fte->pinned = false;
    /* TODO: Maybe also unpin here when do eviction */
}

/* Inserts the frame table entry FTE into the frame table with key KPAGE 
   corresponding to its physical address. */
void
insert_frame (struct fte * fte, void * kpage)
{
    /* Probably want to make sure isn't evicted until info loaded from
       supplementary page table */
    fte->pinned = true;
    fte->upage = NULL;
    fte->kpage = kpage;

    /* Possibly add the new frame right before the clock hand so that it
        gets a long shot */
    lock_acquire (&frame_lock);
    list_push_back (&frame_table, &fte->elem);
    lock_release (&frame_lock);
}

/* Removes the frame table entry that corresponds whose physical address 
   corresponds to the kernel virtual page KPAGE from the frame table and
   frees the associated memory. Returns true if the entry was removed and
   false if the entry could not be found in the table. */
static bool
delete_frame (void * kpage)
{
    /* TODO: Very coarse grain locking here, can it be simpler? */
    lock_acquire (&frame_lock);
    struct fte *fte = frame_lookup (kpage);
    if (fte == NULL)
        {
            lock_release (&frame_lock);
            return false;
        }
    list_remove (&fte->elem);
    free (fte);
    lock_release (&frame_lock);
    
    return true;
}

/* Returns the frame table entry that corresponds to the physical frame
   associated with the kernel virtual page KPAGE. If no such entry exists,
   returns NULL. */
struct fte *
frame_lookup (void *kpage)
{
    struct fte *f;
    struct list_elem *e;

    e = list_head (&frame_table);
    while ((e = list_next (e)) != list_end (&frame_table)) 
    {
        f = list_entry(e, struct fte, elem);
        if (f->kpage == kpage)
            return f;
    }
    return NULL;
}


void move_clock_hand (void) {
    if (list_tail(&frame_table) == hand) 
        hand = list_begin (&frame_table);
    else
        hand = list_next(hand);
    return;
}

void *frame_evict (void) 
{
    while (true) {
        if (list_empty(&frame_table))
            continue;
        struct fte *entry = list_entry(hand, struct fte, elem);
        /* If the hand is pinned, skip it. We must account for frame table 
        entries that aren't completely set up */
        if (entry->pinned) {
            move_clock_hand();
        } else {
            if (pagedir_get_page(entry->pd, entry->upage) == NULL)
                PANIC("not in pagedir");
            /* Check if the page is accessed. */
            if (pagedir_is_accessed(entry->pd, entry->upage)) {
                /* If it is accessed, clear the accessed bit and move the hand 
                 forward. */
                pagedir_set_accessed(entry->pd, entry->upage, false);
                move_clock_hand();
            } else {
                /* Evict the frame and return the kernel virtual address of the
                 evicted page. */
                spt_evict_upage (entry->upage, entry->pd);
                move_clock_hand();
                return entry->kpage;
            }
        }
    }
}