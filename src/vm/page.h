#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include "filesys/file.h"
#include "filesys/off_t.h"

/* Supplementary page table entry. Used for freeing correct information when
   a process is evicted and to upload correct information on page faults */
struct spte
    {
        void *uaddr;                    /* User Virtual Address and SPT key. */
        bool file;                      /* Whether page is a file. */
        union 
            {
                struct
                    {
                        struct file *file;          /* File pointer. */
                        off_t ofs;                  /* File offset. */
                        size_t page_read_bytes;     /* Number of page read 
                                                       bytes */
                        bool writeable;             /* File R/W */
                    } file_info;
                /* TODO - put in something for SWAP & MMU */
            } disk_info;                            /* Info how to read and 
                                                       write to disk */
        struct hash_elem hash_elem;                 /* Page Table hash elem. */
    };

void *page_install (void *upage, void *kpage);
unsigned page_hash (const struct hash_elem *p_, void *aux);
bool page_less (const struct hash_elem *a_, const struct hash_elem *b_,
                        void *aux);

#endif /* vm/page.h */