#ifndef VM_FRAME_H
#define VM_FRAME_H

// #include "vm/page.h"
#include <hash.h>
#include "threads/thread.h"

struct fte
    {
        void *kaddr;                    /* The frame's kernel virtual address 
                                           used as hash entry */
        void *uaddr;                    /* User Virtual Address of the frame
                                           contents */
        tid_t tid;                      /* TID of owner of frame */
        bool pinned;                    /* Whether frame is pinned */
        struct hash_elem hash_elem;     /* Frame list element */
    };

void frame_init (void);
void *frame_get_page (enum palloc_flags flags);
void frame_free_page (void *kpage);

#endif /* vm/frame.h */