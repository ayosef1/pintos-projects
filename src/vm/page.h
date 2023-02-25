#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include "filesys/file.h"
#include "filesys/off_t.h"

/* The file_info struct is information needed to load a page
   from the filesys sector of disk.

   The file pointer `file` is used to access the inode and thus
   block sector.
   The offset `ofs` describes the offset in that 
   sector to start from.
   `page_read_bytes` is the number of bytes to read starting from that
   offset. The remaning PGSIZE - `page_read_bytes` are zeroed out. */
struct file_info
    {
        struct file *file;          /* File pointer. */
        off_t ofs;                  /* File offset. */
        size_t page_read_bytes;     /* Number of page read bytes */
    };

/* This union generalizes the information needed to load a page from disk
   and free.
   In particular it generalizes what is needed from the FILESYS sector vs
   the SWAP sector. */
union disk_info
    {
        struct file_info file_info;
    };

/* Supplementary page table entry. Each thread has its own supplementary page
   table and each entry is indexed by the unique user virtual page `upage`.
   This table contains the information to correctly page out to disk as well
   as clean up resources when a the page is freed.

   The `is_file` member is used to determine how to interpret the `disk_info`
   that describes the pertinent information to load a page from disk. The
   `writable` member desribes how the page table entry's writable bit should
   be initialized when the page is loaded from disk. Finally the `hash_elem`
   is the element of the supplementary page table (a hash table). */

struct spte
    {
        void *upage;                    /* User Virtual Address and SPT key. */
        bool is_file;                   /* Whether page is a file. */
        bool writable;                 /* Whether memory is writeable */
        union disk_info disk_info;      /* Info how to read and  write to 
                                           disk */
        struct hash_elem hash_elem;     /* Page Table hash elem. */
    };

bool spt_try_add_upage (void *upage, bool writable, bool is_file,
                        union disk_info *disk_info);
bool spt_load_upage (void *upage, void *kpage);
unsigned page_hash (const struct hash_elem *p_, void *aux);
bool page_less (const struct hash_elem *a_, const struct hash_elem *b_,
                void *aux);

#endif /* vm/page.h */