#include <hash.h>
#include "threads/malloc.h"
#include "threads/thread.h"
#include "userprog/syscall.h"
#include "vm/mmap.h"
#include "vm/page.h"

static void mmap_destructor_fn (struct hash_elem *e, void *aux UNUSED);

/* Adds an entry to the current thread's mmap table with mapping from
   the first user virtual page BEGIN_UPAGE to the first page END_UPAGE
   that is not in the mapping. Returns -1 if couldn't malloc memory
   otherwise the new mapid.  */
mapid_t
mmap_insert (void *begin_upage, int pg_cnt, struct file *fp)
{
  struct thread * cur;
  mapid_t mapid;
  struct mmap_table_entry *new_entry;

  new_entry = malloc (sizeof (struct mmap_table_entry));
  if (new_entry == NULL)
    return -1;

  cur = thread_current ();
  /* Adding file to fd table so isn't allocated to another file. */
  mapid = cur->next_fd;
  cur->fdtable[mapid] = fp;
  thread_update_next_fd (cur);

  new_entry->begin_upage = begin_upage;
  new_entry->pg_cnt = pg_cnt;
  new_entry->mapid = mapid;
  hash_insert (&cur->mmap_table, &new_entry->hash_elem);

  return mapid;
}

/* Looks up MAPID in the current thread's mmap table. If present,
   removes the entry from the table and frees the entry's memory. */
void
mmap_remove (mapid_t mapid)
{
    struct mmap_table_entry * m = mmap_find (mapid);
    if (m == NULL)
        return;
    
    hash_delete (&thread_current ()->mmap_table, &m->hash_elem);
    free (m);
}

/* Looks up mmap entry in a mmap table HASH with mapid MAPID.
   Returns NULL if no such entry, otherwise returns mmap_table_entry pointer. */
struct mmap_table_entry *
mmap_find (mapid_t mapid)
{
  struct mmap_table_entry mmap_table_entry;
  struct hash_elem *e;

  mmap_table_entry.mapid = mapid;
  e = hash_find (&thread_current ()->mmap_table, &mmap_table_entry.hash_elem);
  return e != NULL ? hash_entry (e, struct mmap_table_entry, hash_elem) : NULL;
}

/* Returns a hash value for a mmap entry M_. */
unsigned
mmap_hash (const struct hash_elem *m_, void *aux UNUSED)
{
  const struct mmap_table_entry *m = hash_entry (m_, struct mmap_table_entry, hash_elem);
  return hash_int ((unsigned)m->mapid);
}

/* Returns true if a mmap entry A_ precedes spte B_. */
bool
mmap_less (const struct hash_elem *a_, const struct hash_elem *b_,
           void *aux UNUSED)
{
  const struct mmap_table_entry *a = hash_entry (a_, struct mmap_table_entry,
                                                 hash_elem);
  const struct mmap_table_entry *b = hash_entry (b_, struct mmap_table_entry,
                                                 hash_elem);
  return a->mapid < b->mapid;
}

void
mmap_destroy ()
{
  hash_destroy (&thread_current ()->mmap_table, &mmap_destructor_fn);
}

/* Destructor function for each mmap_entry E of the current thread's 
   mmap_table. This writes any dirty pages back to memory and frees the
   mmap_table_entry memory. */
static void 
mmap_destructor_fn (struct hash_elem *e, void *aux UNUSED)
{
    struct mmap_table_entry *m = hash_entry (e, struct mmap_table_entry,
                                                   hash_elem);
    spt_remove_mmap_pages (m->begin_upage, m->pg_cnt);
    free (m);
}