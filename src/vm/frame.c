#include <list.h>
#include "threads/synch.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "vm/frame.h"

/* Frame table */
static struct list frame_list;
/* Sync for frame table */
static struct lock frame_lock;

static bool frame_remove (void * vaddr);

/* Initializes Frame Table to allow for paging */
void
frame_init()
{
    list_init(&frame_list);
    lock_init(&frame_lock);
}

/* Returns a new frame for the user process. It first tries to acquire a
   new frame from the user pool. If there are none left it acquires it
   evicts using LRU */
void *
frame_get_page (enum palloc_flags flags)
{
    void *kpage;
    struct frame *frame;

    kpage = palloc_get_page (flags);
    if (kpage == NULL)
        PANIC ("Eviction not implemented yet");
    
    frame = malloc (sizeof (struct frame));
    /* Probably want to make sure isn't evicted until load
    frame->pinned = true;
    */
    frame->uaddr = NULL;
    frame->page = NULL;
    frame->kpage = kpage;
    frame->free = false;

    lock_acquire (&frame_lock);
    list_push_back (&frame_list, &frame->list_elem);
    lock_release (&frame_lock);

    return kpage;
}

void
frame_free_page (void *kpage)
{
    if (!frame_remove (kpage))
        return;

    palloc_free_page (kpage);
}

static bool
frame_remove (void * vaddr)
{
    struct list_elem *e;
    struct frame *cur;
    lock_acquire (&frame_lock);
    for (e = list_begin (&frame_list); e != list_end (&frame_list);
        e = list_next(e)) 
        {
            cur = list_entry (e, struct frame, list_elem);
            if (cur->kpage == vaddr)
                {
                    list_remove (&cur->list_elem);
                    lock_release (&frame_lock);
                    free (cur);
                    return true;
                }
        }
    lock_release (&frame_lock);
    return false;
}

