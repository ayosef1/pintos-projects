#include <hash.h>
#include "threads/synch.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
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
static struct fte *frame_lookup (void *kaddr);

/* Initializes Frame Table to allow for paging */
void
frame_init()
{
    hash_init (&frame_table, frame_hash, frame_less, NULL);
    lock_init(&frame_lock);
}

/* Returns a new frame for the user process. It first tries to acquire a
   new frame from the user pool. If there are none left it acquires it
   evicts using LRU */
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

void
frame_free_page (void *kpage)
{
    if (!delete_frame (kpage))
        return;

    palloc_free_page (kpage);
}

void
insert_frame (struct fte * fte, void * kpage)
{
    /* Probably want to make sure isn't evicted until info loaded from
       supplementary page table */
    fte->pinned = true;
    fte->uaddr = NULL;
    fte->kaddr = kpage;
    fte->tid = thread_current ()->tid;

    lock_acquire (&frame_lock);
    hash_insert (&frame_table, &fte->hash_elem);
    lock_release (&frame_lock);
}

/* This function might be redundant right now, meant to completely delete
   a frame associate with a virtual kernal address. */
static bool
delete_frame (void * kaddr)
{
    /* TODO: Very coarse grain locking here, can it be simpler? */
    lock_acquire (&frame_lock);
    struct fte *fte = frame_lookup (kaddr);
    if (fte == NULL)
        {
            lock_release (&frame_lock);
            return false;
        }
    lock_acquire (&frame_lock);
    hash_delete (&frame_table, &fte->hash_elem);
    /* TODO: Add to a free list perhaps? */
    free (fte);
    lock_release (&frame_lock);
    return true;
}

/* Returns the frame containing the given virtual address,
   or a null pointer if no such frame exists. */
struct fte *
frame_lookup (void *kaddr)
{
  struct fte f;
  struct hash_elem *e;

  f.kaddr = kaddr;
  e = hash_find (&frame_table, &f.hash_elem);
  return e != NULL ? hash_entry (e, struct fte, hash_elem) : NULL;
}

/* Returns a hash value for a frame f. */
static unsigned
frame_hash (const struct hash_elem *fte_, void *aux UNUSED)
{
  const struct fte *fte = hash_entry (fte_, struct fte, hash_elem);
  return hash_int ((unsigned)fte->kaddr);
}

/* Returns true if a frame a preceds frame b. */
static bool
frame_less (const struct hash_elem *a_, const struct hash_elem *b_,
               void *aux UNUSED)
{
  const struct fte *a = hash_entry (a_, struct fte, hash_elem);
  const struct fte *b = hash_entry (b_, struct fte, hash_elem);
  
  return a->kaddr < b->kaddr;
}
