#include <hash.h>
#include "vm/page.h"

/* Returns a hash value for a spte P. */
unsigned
page_hash (const struct hash_elem *p_, void *aux)
{
  const struct spte *spte = hash_entry (p_, struct spte, hash_elem);
  return hash_int ((unsigned)spte->uaddr);
}

/* Returns true if a spte A_ precedes spte B_. */
bool
page_less (const struct hash_elem *a_, const struct hash_elem *b_,
           void *aux)
{
  const struct spte *a = hash_entry (a_, struct spte, hash_elem);
  const struct spte *b = hash_entry (b_, struct spte, hash_elem);
  
  return a->uaddr < b->uaddr;
}