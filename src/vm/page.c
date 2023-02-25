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

static struct spte * lookup_page (struct hash *, void *upage);
static bool install_file (void *kpage, struct file_info file_info);

/* Stores the mapping from the user virtual address UPAGE to the
   relevant information to load the PGSIZE segement into memory from
   disk. In particular PAGE_READ_BYTES bytes are read from offset
   OFS in FILE into the frame. The remaining PGSIZE - PAGE_READ_BYTES
   bytes are zeroed out. WRITABLE is a bit to be set in the page table
   entry once the page is loaded into memory. 
   
   Returns true if page was successfully added to the supplementary
   page table adn false otherwise. */
bool
page_try_add_file(void *upage, bool writable, struct file *file,
                  size_t page_read_bytes, off_t ofs)
{
    ASSERT (pg_ofs (upage) == 0);

    struct spte * new_spte;
    struct file_info *file_info;

    new_spte = malloc (sizeof (struct spte));
    if (new_spte == NULL)
        return false;
    
    new_spte->upage = upage;
    new_spte->is_file = true;
    new_spte->writable = writable;

    file_info = &new_spte->disk_info.file_info;
    file_info->file = file;
    file_info->page_read_bytes = page_read_bytes;
    file_info->ofs = ofs;

    hash_insert (&thread_current ()->sup_pagetable, &new_spte->hash_elem);

    return true;
}

/* Loading the current thread's virtual page UPAGE into the frame KPAGE */
bool
page_install_upage (void *upage, void *kpage)
{
    ASSERT (pg_ofs (upage) == 0);

    struct spte *spte;
    union disk_info disk_info;

    spte = lookup_page (&thread_current ()->sup_pagetable, upage);
    if (spte == NULL)
        goto fail;

    disk_info = spte->disk_info;
    /* Assuming it is a page now. */
    if (!spte->is_file)
        PANIC ("Not implemented non FILEs");
    
    if (!install_file (kpage, disk_info.file_info))
        goto fail;


    uint32_t *pd = thread_current ()->pagedir;
    pagedir_clear_page (pd, upage);

    if (!pagedir_set_page (pd, upage, kpage, spte->writable)) 
        goto fail;

    pagedir_set_accessed (pd, upage, true);
    pagedir_set_dirty (pd, upage, false);

    frame_set_uaddr (kpage, upage);
    return true;

    fail:
        frame_free_page (kpage);
        return false;
}

/* Returns a hash value for a spte P. */
unsigned
page_hash (const struct hash_elem *p_, void *aux)
{
  const struct spte *spte = hash_entry (p_, struct spte, hash_elem);
  return hash_int ((unsigned)spte->upage);
}

/* Returns true if a spte A_ precedes spte B_. */
bool
page_less (const struct hash_elem *a_, const struct hash_elem *b_,
           void *aux)
{
  const struct spte *a = hash_entry (a_, struct spte, hash_elem);
  const struct spte *b = hash_entry (b_, struct spte, hash_elem);
  
  return a->upage < b->upage;
}

/* Looks up UPAGE page in a supplemental page table HASH.
   Returns NULL if no such entry, otherwise returns spte pointer. */
static struct spte *
lookup_page (struct hash *spt, void *upage)
{
  struct spte spte;
  struct hash_elem *e;

  spte.upage = upage;
  e = hash_find (spt, &spte.hash_elem);
  return e != NULL ? hash_entry (e, struct spte, hash_elem) : NULL;
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