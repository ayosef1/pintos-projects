#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "devices/timer.h"
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "vm/frame.h"
#include "vm/page.h"

static thread_func start_process NO_RETURN;
static bool load (struct process_arg *arg, void (**eip) (void), void **esp);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  tid_t tid;
  struct process_arg args;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  char *token;
  char *save_ptr;
  token = strtok_r(fn_copy, " ", &save_ptr);


  args.exec_name = token;
  args.save_ptr = save_ptr;
  args.page = fn_copy;
  args.loaded = false;
  sema_init (&args.loaded_sema, 0);
  
  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (token, PRI_DEFAULT, start_process, &args);

  sema_down (&args.loaded_sema);

  if (!args.loaded)
    return TID_ERROR;

  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *args_)
{
  struct process_arg *args = args_;
  struct intr_frame if_;
  bool success;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (args, &if_.eip, &if_.esp);

  /* Inform parent of load status. */
  args->loaded = success;
  sema_up (&args->loaded_sema);

  /* If load failed, quit. */
  palloc_free_page (args->page);
  if (!success) 
  {
    thread_current ()->exit_status = -1;
    thread_exit ();
  }

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting. Frees the child exit information
   if */
int
process_wait (tid_t child_tid) 
{
  struct thread *parent;
  struct list *children;
  struct list_elem *e;

  parent = thread_current ();
  children = &parent->children;

  for (e = list_begin (children); e != list_end (children); e = list_next (e))
    {
      struct child_exit_info *cur_child;

      cur_child = list_entry (e, struct child_exit_info, child_elem);
      if (cur_child->tid == child_tid)
        {
          /* Wait for child */
          sema_down (&cur_child->exited);
          int status = cur_child->exit_status;

          list_remove (&cur_child->child_elem);

          lock_acquire (&cur_child->refs_lock);
          int ref_cnt = --(cur_child->refs_cnt);
          lock_release (&cur_child->refs_lock);

          /* Parent must free this shared memory if child already exited. */
          if (ref_cnt == 0)
            palloc_free_page (cur_child);

          return status;
        }
    }
  return TID_ERROR;
}

/* Free the current process's resources. All files it has open,
   locks that it holds and signals all orphaned children that 
   they can exit. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  printf ("%s: exit(%d)\n", cur->name, cur->exit_status);
  cur->exit_info->exit_status = cur->exit_status;
  sema_up (&cur->exit_info->exited);

  lock_acquire (&cur->exit_info->refs_lock);
  int ref = --(cur->exit_info->refs_cnt);
  lock_release (&cur->exit_info->refs_lock);
  if (ref == 0)
    palloc_free_page (cur->exit_info);

  /* Iterate through child processes' child exit info structs and
     decrement the reference count since parent is exiting. Free 
     this shared memory if the child process already exited. */
  while (!list_empty (&cur->children))
    {
      struct list_elem *e = list_pop_front (&cur->children);
      struct child_exit_info *cp = list_entry (e, struct child_exit_info, 
                                               child_elem);
      lock_acquire (&cp->refs_lock);
      int refs_cnt = --(cp->refs_cnt);
      lock_release (&cp->refs_lock);

      if (refs_cnt== 0)
        palloc_free_page (cp);
    }

  mmap_destroy ();
  /* Close all file descriptors. */
  for (int fd = EXEC_FD; fd < MAX_FILES; fd++)
    {
      if (cur->fdtable[fd] != NULL)
        {
          file_close (cur->fdtable[fd]);
          cur->fdtable[fd] = NULL;
        }
    }
  
  /* Release all locks held by thread. */
  struct list_elem *e;
  for (e = list_begin (&cur->locks_held); e != list_end (&cur->locks_held);
       e = list_next (e))
    {
      struct lock *l = list_entry (e, struct lock, locks_held_elem);
      lock_release (l);
    }

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Pushes a word at SRC to the minimal stack pointed to by *ESP */
#define PUSH_STACK(ESP) *ESP -= WORD_SIZE

static bool setup_stack (void **esp, char *exec_name, char *save_ptr);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP. Signals parent
   Returns true if successful, false otherwise. */
bool
load (struct process_arg *args, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  lock_acquire(&filesys_lock);
  file = filesys_open (args->exec_name);

  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", args->exec_name);
      goto done;
    }

  file_deny_write (file);
  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", args->exec_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp, args->exec_name, args->save_ptr))
    goto done;
  
  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  lock_release (&filesys_lock);
  
  thread_current ()->fdtable[EXEC_FD] = file;
  return success;
}

/* load() helpers. */

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  size_t lazy_ofs = ofs;
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      union disk_info disk_info;
      disk_info.filesys_info.file = file;
      disk_info.filesys_info.page_read_bytes = page_read_bytes;
      disk_info.filesys_info.ofs = lazy_ofs;
      disk_info.filesys_info.writable = writable;

      /* Lazy load */
      if (!spt_try_add_upage (upage, EXEC, false, true, &disk_info))
        return false;
      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
      lazy_ofs += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp, char *exec_name, char *save_ptr) 
{
  uint8_t *kpage;
  bool success = false;

  kpage = frame_get_page (PAL_USER | PAL_ZERO);
  if (kpage == NULL)
    return false;

  uint8_t * stack_upage = ((uint8_t *) PHYS_BASE) - PGSIZE;

  success = spt_try_add_stack_page (stack_upage, kpage);
  if (success)
    *esp = PHYS_BASE;
  else
    {
      frame_free_page (kpage);
      return false;
    }

  void * start_height = *esp;


  char **argv = palloc_get_page (0);
  if (argv == NULL)
    return false;

  int argc = 0;
  for (char *token = exec_name; token != NULL;
      token = strtok_r (NULL, " ", &save_ptr))
    {
      size_t length = strlen (token) + 1;
      *esp -= length;
      strlcpy (*esp, token, length);
      argv[argc] = *esp;
      argc++;
    }
  
  int padding = (size_t) *esp % WORD_SIZE;

  /* Calculate if will overflow a page. Magic 12 is bytes
     needed for argc, argv and ret address 3 * WORD_SIZE. */
  uint32_t stack_bytes_needed = (start_height - *esp) +
                                (WORD_SIZE * argc) + 12 + padding;
  
  if (stack_bytes_needed > PGSIZE)
    return false;

  if (padding)
    {
      *esp -= padding;
      memset (*esp, 0, padding);
    }

  /* Push Null Pointer Sentinel as required by C standard */
  *esp -= WORD_SIZE;
  memset (*esp, 0, WORD_SIZE);

  /* Push arguments in reverse order */
  for (int i = argc - 1; i >= 0; i--) 
    {
      PUSH_STACK (esp);
      memcpy (*esp, &argv[i], WORD_SIZE);
    }

  palloc_free_page (argv);

  /* Push address of argv */
  void *first_arg_addr = *esp;
  PUSH_STACK (esp);
  memcpy (*esp, &first_arg_addr, WORD_SIZE);

  /* Push argc */
  PUSH_STACK(esp);
  memcpy (*esp, &argc, WORD_SIZE);

  /* Push fake pointer */
  *esp -= WORD_SIZE;
  memset (*esp, 0, WORD_SIZE);

  return success;
}
