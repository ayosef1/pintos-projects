#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include "filesys/file.h"
#include "filesys/off_t.h"

struct file_info
    {
        struct file *file;          /* File pointer. */
        off_t ofs;                  /* File offset. */
        size_t page_read_bytes;     /* Number of page read bytes */
    };

union disk_info
    {
        struct file_info file_info;
    };

/* Supplementary page table entry. Used for freeing correct information when
   a process is evicted and to upload correct information on page faults */
struct spte
    {
        void *upage;                    /* User Virtual Address and SPT key. */
        bool is_file;                   /* Whether page is a file. */
        bool writable;                 /* Whether memory is writeable */
        union disk_info disk_info;      /* Info how to read and  write to 
                                           disk */
        struct hash_elem hash_elem;     /* Page Table hash elem. */
    };

bool page_try_add_file(void *uaddr,bool writable, struct file *file,
                       size_t page_read_bytes, off_t ofs);
bool page_install_upage (void *upage, void *kpage);
unsigned page_hash (const struct hash_elem *p_, void *aux);
bool page_less (const struct hash_elem *a_, const struct hash_elem *b_,
                        void *aux);

#endif /* vm/page.h */