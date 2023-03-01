#include <hash.h>
#include "threads/synch.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "vm/frame.h"
#include "vm/page.h"
#include "userprog/pagedir.h"

/* Frame table */
static struct hash frame_table;
/* Sync for frame table */
static struct lock frame_lock;
/* Sync for clock algorithm list */
static struct lock clock_lock;
/* Clock Algorithm List*/
static struct list clock_algorithm_list;
/* Clock Algorithm Hand */
static struct list_elem *hand; 

static void insert_frame (struct fte * fte, void * kpage);
static bool delete_frame (void * vaddr);
static unsigned frame_hash (const struct hash_elem *fte_, void *aux UNUSED);
static bool frame_less (const struct hash_elem *a_, const struct hash_elem *b_,
                        void *aux UNUSED);
static struct fte *frame_lookup (void *kpage);

/* Initializes Frame Table to allow for paging. */
void
frame_table_init (void)
{
    hash_init (&frame_table, frame_hash, frame_less, NULL);
    list_init (&clock_algorithm_list);
    lock_init(&frame_lock);
    lock_init(&clock_lock);
    hand = list_begin(&clock_algorithm_list);
}

/* Destroys frame table by freeing memory associated with hash table. */
void frame_table_destroy (void)
{
    while (!list_empty(&clock_algorithm_list)) {
        list_pop_front(&clock_algorithm_list);
    }
    hash_destroy (&frame_table, NULL);
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
        /* Implement eviction here that will 
            a) Use clock to find correct page to evict
            b) Unload using an spte function
                i) Unload data to correct location in memory 
                ii) Set pagedir to have PTE_P as 0*/
        kpage = frame_evict ();
        if (kpage == NULL)
            PANIC ("Eviction Incurred an Error");
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

    lock_acquire (&frame_lock);
    hash_insert (&frame_table, &fte->hash_elem);
    lock_release (&frame_lock);
    lock_acquire (&clock_lock);
    list_push_back (&clock_algorithm_list, &fte->clock_elem);
    lock_release (&clock_lock);
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
    hash_delete (&frame_table, &fte->hash_elem);
    lock_release (&frame_lock);
    lock_acquire (&clock_lock);
    list_remove (&fte->clock_elem);
    lock_release (&clock_lock);
    free (fte);
    return true;
}

/* Returns the frame table entry that corresponds to the physical frame
   associated with the kernel virtual page KPAGE. If no such entry exists,
   returns NULL. */
struct fte *
frame_lookup (void *kpage)
{
  struct fte f;
  struct hash_elem *e;

  f.kpage = kpage;
  e = hash_find (&frame_table, &f.hash_elem);
  return e != NULL ? hash_entry (e, struct fte, hash_elem) : NULL;
}

/* Returns a hash value for a frame f. */
static unsigned
frame_hash (const struct hash_elem *fte_, void *aux UNUSED)
{
  const struct fte *fte = hash_entry (fte_, struct fte, hash_elem);
  return hash_int ((unsigned)fte->kpage);
}

/* Returns true if a frame a preceds frame b. */
static bool
frame_less (const struct hash_elem *a_, const struct hash_elem *b_,
            void *aux UNUSED)
{
  const struct fte *a = hash_entry (a_, struct fte, hash_elem);
  const struct fte *b = hash_entry (b_, struct fte, hash_elem);
  
  return a->kpage < b->kpage;
}


void move_clock_hand (void) {
    if (list_tail(&clock_algorithm_list) == hand) 
        hand = list_begin (&clock_algorithm_list);
    else
        hand = list_next(hand);
    return;
}

void *frame_evict (void) 
{
    while (true) {
        if (list_empty(&clock_algorithm_list))
            continue;
        struct fte *entry = list_entry(hand, struct fte, clock_elem);
        if (entry == NULL)
            PANIC("entry was null");
        if (entry->kpage == NULL)
            PANIC("kpage was null");
        /* If the hand is pinned, skip it. */
        if (!entry->pinned) {
            move_clock_hand();
        } else {
            /* Check if the page is accessed. */
            if (pagedir_is_accessed(entry->pd, entry->upage)) {
                /* If it is accessed, clear the accessed bit and move the hand 
                 forward. */
                pagedir_set_accessed(entry->pd, entry->upage, false);
                move_clock_hand();
            } else {
                /* Evict the frame and return the kernel virtual address of the
                 evicted page. */
                spt_evict_upage (entry->upage);
                //entry->pd.PTE_P = 0;
                move_clock_hand();
                return entry->kpage;
            }
        }
    }
}