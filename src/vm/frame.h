#ifndef VM_FRAME_H
#define VM_FRAME_H

// #include "vm/page.h"
#include <hash.h>
#include "threads/thread.h"

/* A frame table entry of the frame table.
    
   This data structure records all the relevant information to allow
   for efficient eviction of pages using the LRU approximation clock
   algorithm. In particular it maps from the physical frame represented
   by the kernel virtual page `kpage` to a user processes' user virtual
   page `upage`. 

   The `pinned` member is for use by the eviction algorithm, if a frame is
   pinned it is not considered for eviction.
   `tid` represents the thread that currently owns the frame. It is used
   during eviction to get access to a the frame owner's page directory and
   supplementary page table and `upage` is the key to both of these table.
   `hash_elem` is the element in the frame table. */
struct fte
    {
        void *kpage;                    /* The frame's kernel virtual page 
                                           number. */ 
        void *upage;                    /* User Virtual Address of associated
                                           with the frame. */
        tid_t tid;                      /* TID of owner of frame */
        bool pinned;                    /* If the frame can be evicted. */
        struct hash_elem hash_elem;     /* Frame Table element */
    };

void frame_init (void);
void *frame_get_page (enum palloc_flags flags);
void frame_free_page (void *kpage);
void frame_set_upage (void *kpage, void *upage);

#endif /* vm/frame.h */