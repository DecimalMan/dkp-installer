/* cpio.h: in-memory cpio archive manipulation
 * Copyright (C) 2014 Ryan Pennucci <decimalman@gmail.com>
 */

#ifndef _IM_CPIO_H
#define _IM_CPIO_H

#include <pthread.h>

/* To (maybe) keep malloc performant, we use a single allocation size.  We aim
 * for a reasonably-sized power-of-two allocation, less a few bytes for malloc
 * overhead.
 */
#define ALLOC_CHUNK_SZ (1020*64)

/* cpio header and related functions
 * Very little manipulation is actually needed here.
 */
#define CPIO_MAGIC "070701"
#define CPIO_NAME_MAX (8191) /* busybox limit */
struct cpio_hdr {
	char magic[6];
	char ino[8];
	char mode[8];
	char uid[8];
	char gid[8];
	char nlink[8];
	char mtime[8];
	char size[8];
	char major[8];
	char minor[8];
	char rmajor[8];
	char rminor[8];
	char namesize[8];
	char chksum[8];
	char name[CPIO_NAME_MAX];
};
#define CPIO_HDR_LEN (sizeof(struct cpio_hdr)-CPIO_NAME_MAX)

/* cpio header manipulation:
 * xtol: convert 8-byte string to long
 * ltox: write long to 8-byte header member
 * nudge_ino: write a unique inode to header
 */
unsigned long xtol(char *arg) __attribute__((pure));
void ltox(char *p, unsigned long v);
void nudge_ino(struct cpio_hdr *hdr);

/* Generic chunked i/o
 * In order to make file patching easy, we need chunked i/o.  These are pretty
 * straightforward linked lists of buffers, with some magic to allow them to be
 * split.  __alloc determines whether this chunk will be automatically freed.
 *
 * NB: chunks are freed in reverse.  In a packed (ordered) file_chunk[],
 *     __alloc should only be set for the first chunk.
 */
struct file_chunk {
	unsigned long len;
	char *buf;
	struct file_chunk *next;
	int __alloc; /* should we free() this chunk? */
};
/* To simplify padding, overcommit and ensure we're a multiple of 4 bytes */
#define CHUNK_DATA_SZ ((ALLOC_CHUNK_SZ-sizeof(struct file_chunk)-3)&~3)

struct file_chunk *file_chunk_alloc(struct file_chunk *c);
void file_chunk_free(struct file_chunk *c);

/* cpio file entries
 * For simplicity, the header is contained inside this struct, but data is not.
 * __poison informs the compression thread to skip this entry.  cpio_ent_free
 * also frees any file_chunks associated with the entry.
 */
struct cpio_ent {
	struct cpio_ent *next;
	/* data.buf = &hdr; data.next->buf = malloc(); */
	struct file_chunk data;
	struct cpio_hdr hdr;
	int __poison; /* don't write this file */
};

struct cpio_ent *cpio_ent_alloc(void);
void cpio_ent_free(struct cpio_ent *e);

/* cpio file lists
 * These are used to pass cpio_ents between threads.  Nothing fancy, just a
 * linked list with a mutex+condition for synchronization.
 *
 * A spinlock would be ideal, but pthreads conditions require mutexes.
 */
struct cpio_file_list {
	//pthread_spinlock_t lock;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	struct cpio_ent *head;
	struct cpio_ent *tail;
};

extern struct cpio_file_list read_files, write_files;

int file_list_init(struct cpio_file_list *l);
void file_list_push(struct cpio_file_list *l, struct cpio_ent *e);
struct cpio_ent *file_list_pop(struct cpio_file_list *l);

/* Get the patch/replace/insert logic out of the cpio guts. */
int ramdisk_init_overrides(void);
int ramdisk_handle_overrides(struct cpio_ent *e);
void ramdisk_free_overrides(void);

#endif /* _IM_CPIO_H */
