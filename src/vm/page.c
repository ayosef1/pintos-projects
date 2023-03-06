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
#include "vm/swap.h"
#include <stdio.h>

static bool install_file (void *kpage,
                          struct filesys_info *filesys_info);

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
struct spte *
spt_try_add_upage (void *upage, enum page_type type, bool in_memory, 
                   bool filesys_page, union disk_info *disk_info)
{
    ASSERT (pg_ofs (upage) == 0);

    struct spte * spte;

    uint32_t * pd = thread_current ()->pagedir;
    if (pagedir_get_spte (pd, upage, false) != NULL)
        return NULL;

    spte = malloc (sizeof (struct spte));
    if (spte == NULL)
        return NULL;
    
    spte->upage = upage;
    spte->type = type;
    spte->filesys_page = filesys_page;
    spte->disk_info = *disk_info;

    if (!in_memory)
        pagedir_add_spte (pd, upage, spte);

    return spte;
}

/* Attempt to add a stack page with user virtual page UPAGE to the 
   supplementary page table and adding a mapping to the current 
   thread's page table from UPAGE to kernel virtual page KPAGE. */
bool
spt_try_add_stack_page (void *upage)
{
  union disk_info empty_disk_info;
  struct spte *spte;
  uint32_t *pd;

  void *kpage = frame_get_page (ZEROED);
  if (kpage == NULL)
    return false;
  
  /* Checks each spte not there and upage. */
  spte = spt_try_add_upage (upage, TMP, true, false, &empty_disk_info);
  if (spte == NULL)
    return false;

  pd = thread_current ()->pagedir;
  if (!pagedir_set_page (pd, upage, kpage, true) )
    return false;

  memset (upage, 0, PGSIZE);
  frame_set_udata (kpage, upage, pd, spte, false);

  return true;
}

/* Attempts to add PG_CNT consecutive user virtual pages starting from 
   BEGIN_UPAGE to the supplementary page table. To lazily read, the
   spt needs to store the file pointer FP for each. Each page contains all
   read bytes except the final page which has FINAL_READ_BYTES read bytes.
   
   Returns true on success of adding mappings for all pages. */
bool 
spt_try_add_mmap_pages (void *begin_upage, struct file *fp, int pg_cnt,
                            size_t final_read_bytes)
{
  union disk_info disk_info;
  int pg;

  disk_info.filesys_info.file = fp;
  disk_info.filesys_info.page_read_bytes = PGSIZE;
  disk_info.filesys_info.ofs = 0;
  disk_info.filesys_info.writable = true;

  for (pg = 0; pg < pg_cnt; pg += 1)
    {
        if (pg == pg_cnt - 1)
            disk_info.filesys_info.page_read_bytes = final_read_bytes;

        if (spt_try_add_upage (begin_upage + (pg * PGSIZE), MMAP, false, true,
                                &disk_info) == NULL)
            {
                spt_remove_mmap_pages (begin_upage, pg);

                lock_acquire (&filesys_lock);
                file_close (fp);
                lock_release (&filesys_lock);

                return false;
            }
            
        disk_info.filesys_info.ofs += PGSIZE;
    }
  return true;
}


/* Loading the current thread's virtual page UPAGE into the frame KPAGE */
bool
spt_try_load_upage (void *upage, bool keep_pinned)
{
    ASSERT (pg_ofs (upage) == 0);

    uint32_t *pd;
    struct spte *spte;
    union disk_info disk_info;

    pd = thread_current ()->pagedir;

    /* The page should not be in memory if we are making this call. */
    ASSERT (!pagedir_is_present (pd, upage));

    bool hold_frame_lock = false;

    spte = pagedir_get_spte (pd, upage, hold_frame_lock);
    if (spte == NULL)
        return false;
    
    void *kpage = frame_get_page (NOT_ZEROED);
    if (kpage == NULL)
        return false;

    bool writable = true;
    if (spte->type == EXEC)
        writable = spte->disk_info.filesys_info.writable;
    
    disk_info = spte->disk_info;
    /* Assuming it is a page now. */
    if (spte->filesys_page)
        {
            if (!install_file (kpage, &disk_info.filesys_info))
                goto fail;
        }
    else
        {
            if (!swap_try_read (disk_info.swap_id, kpage))
                goto fail;
        }
    
    /* Do this after to prevent a write violation when writing to
       the page. */
    if (!pagedir_set_page (pd, upage, kpage, writable)) 
        goto fail;

    pagedir_set_accessed (pd, upage, true);
    pagedir_set_dirty (pd, upage, false);

    frame_set_udata (kpage, upage, pd, spte, keep_pinned);
    return true;

    fail:
        frame_free_page (kpage, hold_frame_lock);
        return false;
}

/* Evicts a physical frame pointed to by kpage and stores it in the file system, 
swap space, or neither if it is an executable that hasn't been written to. */
void
spt_evict_kpage (void *kpage, uint32_t *pd, struct spte *spte)
{
    pagedir_clear_page(pd, spte->upage);
    switch (spte->type)
        {
        case (MMAP):
            /* Only need to write if MMAP is written. */
            if (pagedir_is_dirty(pd, spte->upage))
                {
                    /* Write back to memory. */
                    lock_acquire (&filesys_lock);
                    file_write_at (spte->disk_info.filesys_info.file, kpage,
                                   PGSIZE, spte->disk_info.filesys_info.ofs);
                    lock_release (&filesys_lock);
                }
            break;
        case (EXEC):
            /* If an executable page has never been written to, do nothing.
               Otherwise write to swap. */
            if (spte->filesys_page && (!spte->disk_info.filesys_info.writable ||
                !pagedir_is_dirty(pd, spte->upage)))
                break;
        default:
            spte->filesys_page = false; 
            spte->disk_info.swap_id = swap_write (kpage);
            break;
        }
    pagedir_add_spte (pd, spte->upage, spte);
}

/* Removes PG_CNT consecutive mmaped user virtual pages from the current 
   thread's supplementary page table starting from BEGIN_UPAGE. This is used
   on process exit, failed mmap calls and munmap. */
void
spt_remove_mmap_pages (void * begin_upage, int num_pages)
{
    struct spte * spte;
    uint32_t *pd = thread_current ()->pagedir;
    bool hold_frame_lock = true;
    for (int pg = 0; pg < num_pages; pg ++)
        {
            void *cur_upage = begin_upage + (pg * PGSIZE);
            spte = pagedir_get_spte (pd, cur_upage, hold_frame_lock);
            if (spte == NULL)
                continue;
            /* Go through the frame table to do this -> similar to pagedir
               get_page. */
            if (pagedir_is_present (pd, cur_upage))
                {
                    if (pagedir_is_dirty (pd, cur_upage))
                        {
                            lock_acquire (&filesys_lock);
                            file_write_at (spte->disk_info.filesys_info.file,
                                           cur_upage, PGSIZE,
                                           spte->disk_info.filesys_info.ofs);
                            lock_release (&filesys_lock);
                        }
                    frame_free_page (pagedir_get_page (pd, cur_upage),
                                     hold_frame_lock);
                }
            pagedir_null_page (pd, cur_upage);        
        }
}

/* Install file content to kernel page. If the previously installed filesystem 
    information incdicates bytes to read is 0, the kernel page will be zeroed 
    out. Otherwise, the content of the file  specified in filesys_info will be 
    read into the kernel page. */
static bool
install_file (void *kpage, struct filesys_info *filesys_info)
{
    if (filesys_info->page_read_bytes == 0)
        {
            memset (kpage, 0, PGSIZE);
        }

    else
        {
            /* Locking not neccesary here because we will have lock from 
                page fault. */
            size_t page_zero_bytes;
            memset (kpage, 0, PGSIZE);
            int bytes_read = file_read_at (filesys_info->file, kpage, 
                                           filesys_info->page_read_bytes,
                                           filesys_info->ofs);

            if (bytes_read != (int) filesys_info->page_read_bytes)
                return false;
            page_zero_bytes = PGSIZE - filesys_info->page_read_bytes;
            memset (kpage + filesys_info->page_read_bytes, 0, page_zero_bytes);
        }
    return true;
}