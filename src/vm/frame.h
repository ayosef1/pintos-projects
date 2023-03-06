#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <hash.h>
#include "threads/synch.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "vm/page.h"

#define NOT_ZEROED false
#define ZEROED true
/* A frame table entry of the frame table.
    
   This data structure records all the relevant information to allow
   for efficient eviction of pages using the LRU approximation clock
   algorithm. In particular it maps from the physical frame represented
   by the kernel virtual page `kpage` to a user processes' user virtual
   page `upage`. 

   The `pinned` member is for use by the eviction algorithm, if a frame is
   pinned it is not considered for eviction.
   Because our frame table is preallocated, we map each kernel virtual addresses
   to a specific entry in our table. Furthermore, the fte lock allows for fine
   grain locking since it is per a frame table entry. */
struct fte
    {
        void *upage;                    /* User Virtual Address of associated
                                           with the frame. */
        uint32_t *pd;                   /* Pagedirectory of owner thread. */
        struct spte *spte;              /* Supplementary page table entry
                                           of owner. */
        bool pinned;                    /* If the frame can be evicted. */
        struct lock lock;               /* Lock for access to frame entry */
    };

void frame_table_init (uint8_t *user_pool_base, size_t user_pages);
void frame_table_create (void);
void frame_table_destroy (void);

void *frame_get_page (bool zeroed);
struct spte *frame_get_spte (void *kpage, const void *upage, bool hold);
void frame_free_page (void *kpage, bool lock_held);
void frame_set_udata (void *kpage, void *upage, uint32_t *pd,
                      struct spte *spte, bool keep_pinned);
void frame_set_pin (void *upage, uint32_t *pd, bool pin);

#endif /* vm/frame.h */
