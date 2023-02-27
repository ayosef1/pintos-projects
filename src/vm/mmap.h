#ifndef VM_MMAP_H
#define VM_MMAP_H

#include <hash.h>

typedef int mapid_t;

struct mmap_table_entry
    {
        mapid_t mapid;                  /* Map id of mmap file. */
        void *begin_upage;              /* Start user virtual address. */
        int pg_cnt;                     /* Number of mappped pages. */
        struct hash_elem hash_elem;     /* Memory Map Table hash elem. */
    };

mapid_t mmap_insert (void *begin_upage, int pg_cnt);
void mmap_remove (mapid_t mapid);
struct mmap_table_entry * mmap_find (mapid_t mapid);
unsigned mmap_hash (const struct hash_elem *m_, void *aux UNUSED);
bool mmap_less (const struct hash_elem *a_, const struct hash_elem *b_,
                void *aux UNUSED);

#endif /* vm/mmap.h */