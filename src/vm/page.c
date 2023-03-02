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

static bool install_file (void *kpage, struct filesys_info filesys_info);

/* Stores the mapping from the user virtual address UPAGE to the
   relevant information to load the PGSIZE segement into memory from
   disk in the current thread's supplementary page table.
   If mapping exits already, overwrites this mapping.
        - TYPE describes whether it is an executable, mmap or tmp page.
        - IN_MEMORY is a that says whether the spte entry is in memory. 
        - FILESYS_PAGE is true is whether it was last stored in the filesys
        - DISK_INFO is the additional information relevant to load it
          from the relevant part of disk
   
   Returns true if page was successfully added to the supplementary
   page table, false otherwise. */
bool
spt_try_add_upage (void *upage, enum page_type type, bool in_memory, 
                   bool filesys_page, union disk_info *disk_info)
{
    ASSERT (pg_ofs (upage) == 0);

    struct spte * spte;
    uint32_t * pd = thread_current ()->pagedir;

    if (pagedir_get_spte (pd, upage))
    {
        PANIC ("Here is our issue.");
        return false;
    }
    /* When properly implemented we just remove this */
    spte = spt_find (&thread_current ()->spt, upage);
    if (spte == NULL)
        {
            spte = malloc (sizeof (struct spte));
            if (spte == NULL)
                return false;
        }
    else
    /* This should never hit! */
    {
        PANIC ("Found an SPTE entry in spt table.");
        return false;
    }
    
    spte->upage = upage;
    spte->type = type;
    spte->in_memory = in_memory;
    spte->filesys_page = filesys_page;
    spte->disk_info = *disk_info;

    /* This will become useful when we use pagedir to as spte store.
       For now doing nothing. */
    pagedir_add_spte (pd, upage, spte);

    /* This should be redundant shortly. */
    hash_insert (&thread_current ()->spt, &spte->hash_elem);

    return true;
}

/* Attempt to add a stack page with user virtual page UPAGE to the 
   supplementary page table and adding a mapping to the current 
   thread's page table from UPAGE to kernel virtual page KPAGE. */
bool
spt_try_add_stack_page (void *upage)
{
  union disk_info empty_disk_info;
  uint32_t *pd;

  void *kpage = frame_get_page (PAL_USER | PAL_USER);
  if (kpage == NULL)
    return false;
  
  /* Checks each spte not there and upage. */
  pd = thread_current ()->pagedir;
  if (pagedir_get_spte (pd, upage) == NULL && 
      spt_try_add_upage (upage, TMP, true, false, &empty_disk_info))
    {
        if (pagedir_set_page (pd, upage, kpage, true))
                return true;
    }
  return false;
}

/* Loading the current thread's virtual page UPAGE into the frame KPAGE */
bool
spt_try_load_upage (void *upage)
{
    ASSERT (pg_ofs (upage) == 0);

    uint32_t *pd;
    struct spte *spte;
    union disk_info disk_info;

    pd = thread_current ()->pagedir;
    pagedir_clear_page (pd, upage);

    spte = spt_find (&thread_current ()->spt, upage);
    if (spte == NULL)
        return false;


    bool writable = true;
    if (spte->type == EXEC)
        writable = spte->disk_info.filesys_info.writable;
    
    void *kpage = frame_get_page (PAL_USER);
    
    disk_info = spte->disk_info;
    /* Assuming it is a page now. */
    if (spte->filesys_page)
        {
            if (!install_file (kpage, disk_info.filesys_info))
                goto fail;
        }
    // else
    //     {
    //         if (!swap_try_read (disk_info.swap_id, upage))
    //             goto fail;
    //     }

    // TODO: is it right to do this after installing?
    if (!pagedir_set_page (pd, upage, kpage, writable)) 
        goto fail;

    pagedir_set_accessed (pd, upage, true);
    pagedir_set_dirty (pd, upage, false);

    frame_set_udata (kpage, upage, pd, spte);
    spte->in_memory = true;
    return true;

    fail:
        frame_free_page (kpage);
        return false;
}

/* Returns a hash value for a spte P. */
unsigned
spt_hash (const struct hash_elem *p_, void *aux)
{
  const struct spte *spte = hash_entry (p_, struct spte, hash_elem);
  return hash_int ((unsigned)spte->upage);
}

/* Returns true if a spte A_ precedes spte B_. */
bool
spt_less (const struct hash_elem *a_, const struct hash_elem *b_,
           void *aux)
{
  const struct spte *a = hash_entry (a_, struct spte, hash_elem);
  const struct spte *b = hash_entry (b_, struct spte, hash_elem);
  
  return a->upage < b->upage;
}

/* Looks up UPAGE page in a supplemental page table HASH.
   Returns NULL if no such entry, otherwise returns spte pointer. */
struct spte *
spt_find (struct hash *spt, void *upage)
{
  struct spte spte;
  struct hash_elem *e;

  spte.upage = upage;
  e = hash_find (spt, &spte.hash_elem);
  return e != NULL ? hash_entry (e, struct spte, hash_elem) : NULL;
}

static bool
install_file (void *kpage, struct filesys_info filesys_info)
{
    if (filesys_info.page_read_bytes == 0)
        memset (kpage, 0, PGSIZE);
    else
        {
            size_t page_zero_bytes;
            /* Don't have locking here because from a page fault we will
               already have the lock */
            int bytes_read = file_read_at (filesys_info.file, kpage, 
                                           filesys_info.page_read_bytes,
                                           filesys_info.ofs);

            if (bytes_read != (int) filesys_info.page_read_bytes)
                return false;
            page_zero_bytes = PGSIZE - filesys_info.page_read_bytes;
            memset (kpage + filesys_info.page_read_bytes, 0, page_zero_bytes);
        }
    return true;
}