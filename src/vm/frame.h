#ifndef VM_FRAME_H
#define VM_FRAME_H

// #include "vm/page.h"
#include <list.h>

struct frame
    {
        bool pinned;                    /* Whether frame is pinned */
        bool free;                      /* Whether frame is free */
        void *uaddr;                    /* User Virtual Address of the frame
                                           contents */
        void *kpage;                    /* The frame's kernel virtual address */
        struct page *page;              /* The supplemental page table info for
                                           eviction */
        struct list_elem list_elem;     /* Frame list element */
    };

void frame_init (void);
void *frame_get_page (enum palloc_flags);
void frame_free_page (void *kpage);

#endif /* vm/frame.h */