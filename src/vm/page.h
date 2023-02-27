#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include "filesys/file.h"
#include "filesys/off_t.h"

/* The the type of page that the supplementary page table entry represents.
   This is relevant information when decided where to write to memory and what
   to clean up when evicting and removing pages from the supplementary page
   table.
        - EXEC pages are never written back to disk. It is written to swap if 
        the page is dirty. If it isn't dirty, it is only every read back from
        the filesys after eviction.
        -MMAP pages is written back only to the filesys and only if the page
        is dirty. Otherwise it is just read from filesys.
        - TMP data is local data only used for the duration of the user process.
        It is stored in SWAP whenever evicted / before lazy loaded and
        discarded once the process exits. */
enum page_type
    {
        EXEC,                       /* An executable file's page. */
        MMAP,                       /* A memory mapped page. */
        TMP,                        /* A temporary page such as the stack. */
    };


/* The filesys_info struct is information needed to load a page
   from the filesys sector of disk.

   The file pointer `file` is used to access the inode and thus
   block sector.
   The offset `ofs` describes the offset in that 
   sector to start from.
   `page_read_bytes` is the number of bytes to read starting from that
   offset. The remaning PGSIZE - `page_read_bytes` are zeroed out.
   `writable` instructs the process how to set the writable bit in
   the page table once loaded into memory. */
struct filesys_info
    {
        struct file *file;          /* File pointer. */
        off_t ofs;                  /* File offset. */
        size_t page_read_bytes;     /* Number of page read bytes */
        bool writable;              /* Whether file is writeable. */
    };

/* This union generalizes the information needed to load a page from disk
   and free.
   In particular it generalizes what is needed from the FILESYS sector vs
   the SWAP sector. */
union disk_info
    {
        struct filesys_info filesys_info;
    };

/* Supplementary page table entry. Each thread has its own supplementary page
   table and each entry is indexed by the unique user virtual page `upage`.
   This table contains the information to correctly page out to disk as well
   as clean up resources when a the page is freed.

   The `filesys_page` member says where the page is currently or was most
   recently stored (depending on whether it is currently `in_memory` or not).
   `disk_info` describes describes the pertinent information to load a page
   from disk and is interpreted based on the `type` of page it is and
   what the `filesys_page` bit. The
   `writable` member desribes how the page table entry's writable bit should
   be initialized when the page is loaded from disk. Finally the `hash_elem`
   is the element of the supplementary page table (a hash table). */

struct spte
    {
        void *upage;                    /* User Virtual Address and SPT key. */
        enum page_type type;            /* What type of page it is. */
        bool in_memory;                 /* For use when removing entries at
                                           cleanup. */
        bool filesys_page;              /* If stored in filesys. */
        union disk_info disk_info;      /* Info how to read and  write to 
                                           disk */
        struct hash_elem hash_elem;     /* Page Table hash elem. */
    };

bool spt_try_add_upage (void *upage, enum page_type type, bool in_memory,
                        bool filesys_page, union disk_info *disk_info);
bool spt_try_add_mmap_pages (void *begin_upage, struct file *fp, int pg_cnt,
                            size_t final_read_bytes);
void spt_remove_upages (void * begin_upage, int num_pages);
void spt_evict_upage (void *upage);
bool spt_load_upage (void *upage, void *kpage);
struct spte * spt_find (void *upage);
unsigned spt_hash (const struct hash_elem *p_, void *aux UNUSED);
bool spt_less (const struct hash_elem *a_, const struct hash_elem *b_,
                void *aux UNUSED);

#endif /* vm/page.h */