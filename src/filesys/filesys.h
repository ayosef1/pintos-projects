#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>
#include "filesys/off_t.h"

/* Sectors of system file inodes. */
#define FREE_MAP_SECTOR 0       /* Free map file inode sector. */
#define ROOT_DIR_SECTOR 1       /* Root directory file inode sector. */

#define IS_FILE true
#define IS_DIR false

/* Block device that contains the file system. */
extern struct block *fs_device;

void filesys_init (bool format);
void filesys_done (void);
bool filesys_create (const char *pathname, off_t initial_size, bool is_file);
struct file *filesys_open (const char *pathname);
bool filesys_remove (const char *pathname);
bool filesys_mkdir (const char *dir);

#endif /* filesys/filesys.h */
