#include <hash.h>
#include "threads/synch.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"

/* Frame table */
static struct hash frame_table;
/* Sync for frame table */
static struct lock frame_lock;

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
    lock_init(&frame_lock);
}

/* Destroys frame table by freeing memory associated with hash table. */
void frame_table_destroy (void)
{
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
        /* Implement eviction here that will 
            a) Use clock to find correct page to evict
            b) Unload using an spte function
                i) Unload data to correct location in memory 
                ii) Set pagedir to have PTE_P as 0*/
        PANIC ("Eviction not implemented yet");
    
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
    /* TODO: Add to a free list perhaps? */
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

bool
frame_unpin (void *uaddr)
{   
    void *kpage;
    struct fte *fte;
    struct thread *cur;

    cur = thread_current ();
    kpage = pg_round_down (pagedir_get_page (cur->pagedir, uaddr));

    lock_acquire (&frame_lock);
    fte = frame_lookup (kpage);
    if (fte == NULL)
    {
        lock_release (&frame_lock);
        return false;
    }
    /* TODO: does it matter that we unpin an unpinned page??*/
    if (!fte->pinned)
    {
        lock_release (&frame_lock);
        return false;
    }
    fte->pinned = false;
    lock_release (&frame_lock);
    return true;
}