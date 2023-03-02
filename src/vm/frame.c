#include <hash.h>
#include <stdio.h>
#include <string.h>
#include "threads/synch.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "filesys/off_t.h"
#include "vm/frame.h"

/* Number of frames in frame table. */
static size_t num_frames;
/* Frame table Base Pointer */
static struct fte *frame_table_base;
/* Base address used to index into the frame table.*/
static uint8_t *user_kpage_base;

static bool delete_frame (void * vaddr);
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
        }
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
frame_get_page (enum palloc_flags flags)
{
    void *kpage;
    struct fte *fte;

    kpage = palloc_get_page (flags);
    if (kpage == NULL)
    {
        //kpage = frame_evict ()
            /* Implement eviction here that will 
            a) Use clock to find correct page to evict
            b) Unload using an spte function
                i) Unload data to correct location in memory 
                ii) Set pagedir to have PTE_P as 0*/
        PANIC ("Eviction not implemented yet");
    }
    else 
        {

            fte = frame_lookup (kpage);
            lock_acquire (&fte->lock);
            fte->pinned = true;
            lock_release (&fte->lock);
        }

    return kpage;
}

// struct spte *
// frame_get_spte (void * kpage)
// {
//     struct spte * spte;
//     struct fte *fte = frame_lookup (kpage);
//     if (kpage == NULL)
//         return NULL;
//     lock_acquire (&fte->lock);
//     spte = fte->spte;
//     lock_release (&fte->lock);
//     return spte;
// }

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
    lock_acquire (&fte->lock);
    ASSERT (fte->pinned)
    /* Don't need a lock here because no one accessing this data yet since
       it is pinned. */
    fte->upage = upage;
    fte->pd = pd;
    fte->spte = spte;
    lock_release (&fte->lock);
}

/* Removes the frame table entry that corresponds whose physical address 
   corresponds to the kernel virtual page KPAGE from the frame table and
   frees the associated memory. Returns true if the entry was removed and
   false if the entry could not be found in the table. */
static bool
delete_frame (void * kpage)
{
    struct fte *fte = frame_lookup (kpage);
    lock_acquire (&fte->lock);
    fte->upage = NULL;
    /* Coudl change to do cleaning here. I.e. spt_remove_page. */
    fte->spte = NULL;
    fte-> pd = NULL;
    lock_release (&fte->lock);
    return true;
}

/* Returns the frame table entry that corresponds to the physical frame
   associated with the kernel virtual page KPAGE. If no such entry exists,
   returns NULL. */
struct fte *
frame_lookup (uint8_t *kpage)
{
    ASSERT (kpage > user_kpage_base);
    off_t table_ofs = ((kpage - user_kpage_base) / PGSIZE);
    return frame_table_base + table_ofs;
}