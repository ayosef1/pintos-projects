#include <hash.h>
#include <string.h>
#include "filesys/file.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "vm/frame.h"

static bool install_file (void *kpage, struct file_info file_info);

/* Stores the mapping from the user virtual address UPAGE to the
   relevant information to load the PGSIZE segement into memory from
   disk in the current thread's supplementary page table.
   If mapping exits already, overwrites this mapping.
        - WRITABLE is a bit to be set in the page table
          entry once the page is loaded into memory. 
        - IS_FILE is true if the page should be stored in the filesys
          and false if it should be stored in swap
        - DISK_INFO is the additional information relevant to load it
          from the relevant part of disk
   
   Returns true if page was successfully added to the supplementary
   page table, false otherwise. */
bool
spt_try_add_upage (void *upage, bool writable, bool  filesys_page,
                    union disk_info *disk_info)
{
    ASSERT (pg_ofs (upage) == 0);

    struct spte * spte;

    spte = spt_find (upage);
    if (spte == NULL)
        {
            spte = malloc (sizeof (struct spte));
            if (spte == NULL)
                return false;
        }
    
    spte->upage = upage;
    spte->filesys_page = filesys_page;
    spte->writable = writable;

    spte->disk_info = *disk_info;

    hash_insert (&thread_current ()->spt, &spte->hash_elem);

    return true;
}

bool spt_try_add_mmap_file (void *begin_upage, struct file *fp, int pg_cnt,
                            size_t final_read_bytes)
{
  union disk_info disk_info;
  int pg;

  disk_info.file_info.file = fp;
  disk_info.file_info.page_read_bytes = PGSIZE;
  disk_info.file_info.ofs = 0;

  for (pg = 0; pg < pg_cnt - 1; pg += 1)
    {
        if (!spt_try_add_upage (begin_upage + (pg * PGSIZE), true, true,
                                &disk_info))
            {
                spt_remove_upages (begin_upage, pg);
                return false;
            }
        disk_info.file_info.ofs += PGSIZE;
    }
  
  /* Final case. */
  disk_info.file_info.page_read_bytes = final_read_bytes % PGSIZE;
  if (!spt_try_add_upage (begin_upage + (pg * PGSIZE), true, true, &disk_info))
    {
        spt_remove_upages (begin_upage, pg);
        return false;
    }
  return true;
}

/* Removes PG_CNT consecutive user virtual pages from the current thread's
starting from */
void
spt_remove_upages (void * begin_upage, int num_pages)
{
    struct hash * spt = &thread_current ()->spt;
    struct spte * spte;
    for (int pg = 0; pg < num_pages; pg ++)
        {
            spte = spt_find (begin_upage + (pg * PGSIZE));
            if (spte == NULL)
                continue;
            /* Later will have to look at whether loaded, for now assume it
               is not and therfore what to do with frame table. */
            ASSERT (!spte->in_memory);
            hash_delete (spt, &spte->hash_elem);
            free (spt);
        }
}

/* Loading the current thread's virtual page UPAGE into the frame KPAGE. */
bool
spt_load_upage (void *upage, void *kpage)
{
    ASSERT (pg_ofs (upage) == 0);

    struct spte *spte;
    union disk_info disk_info;

    spte = spt_find (upage);
    if (spte == NULL)
        goto fail;

    disk_info = spte->disk_info;
    /* Assuming it is a page now. */
    if (!spte->filesys_page)
        PANIC ("Not implemented non FILEs");
    
    if (!install_file (kpage, disk_info.file_info))
        goto fail;


    uint32_t *pd = thread_current ()->pagedir;
    pagedir_clear_page (pd, upage);

    if (!pagedir_set_page (pd, upage, kpage, spte->writable)) 
        goto fail;

    pagedir_set_accessed (pd, upage, true);
    pagedir_set_dirty (pd, upage, false);

    frame_set_upage (kpage, upage);
    return true;

    fail:
        frame_free_page (kpage);
        return false;
}

/* Looks up UPAGE page in a supplemental page table HASH.
   Returns NULL if no such entry, otherwise returns spte pointer. */
struct spte *
spt_find (void *upage)
{
  struct spte spte;
  struct hash_elem *e;

  spte.upage = upage;
  e = hash_find (&thread_current ()->spt, &spte.hash_elem);
  return e != NULL ? hash_entry (e, struct spte, hash_elem) : NULL;
}

/* Returns a hash value for a spte P. */
unsigned
spt_hash (const struct hash_elem *p_, void *aux UNUSED)
{
  const struct spte *spte = hash_entry (p_, struct spte, hash_elem);
  return hash_int ((unsigned)spte->upage);
}

/* Returns true if a spte A_ precedes spte B_. */
bool
spt_less (const struct hash_elem *a_, const struct hash_elem *b_,
           void *aux UNUSED)
{
  const struct spte *a = hash_entry (a_, struct spte, hash_elem);
  const struct spte *b = hash_entry (b_, struct spte, hash_elem);
  
  return a->upage < b->upage;
}

static bool
install_file (void *kpage, struct file_info file_info)
{
    if (file_info.page_read_bytes == 0)
        memset (kpage, 0, PGSIZE);
    else
        {
            size_t page_zero_bytes;
            /* Don't have locking here because from a page fault we will
               already have the lock */
            int bytes_read = file_read_at (file_info.file, kpage, 
                                           file_info.page_read_bytes,
                                           file_info.ofs);

            if (bytes_read != (int) file_info.page_read_bytes)
                return false;
            page_zero_bytes = PGSIZE - file_info.page_read_bytes;
            memset (kpage + file_info.page_read_bytes, 0, page_zero_bytes);
        }
    return true;
}