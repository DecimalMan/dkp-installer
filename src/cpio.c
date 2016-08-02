#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/uio.h>

#include "common.h"
#include "cpio.h"
#include <zlib/zlib.h>
#include <zlib/contrib/minizip/unzip.h>

#include <stdio.h>

#include <pthread.h>

/* cpio header manipulation bits:
 * xtol & ltox: 8-char hex conversion
 * nudge_ino: poke ino field similarly to gen_init_cpio.c
 */
unsigned long xtol(char *arg) {
	unsigned long v;
	char *p = arg;
	for (v = 0; p < arg + 8; p++) {
		v <<= 4;
		if (*p >= '0' && *p <= '9')
			v += *p - '0';
		else if (*p >= 'A' && *p <= 'F')
			v += *p + 10 - 'A';
		else if (*p >= 'a' && *p <= 'f')
			v += *p + 10 - 'a';
		else
			return v >> 4;
	}
	return v;
}
void ltox(char *p, unsigned long v) {
	char *s = p + 7;
	unsigned int d;
	for (; s >= p; s--) {
		d = v & 0xf;
		v >>= 4;
		if (d < 10) *s = '0' + d;
		else *s = 'a' + d - 10;
	}
}

static unsigned int global_ino = 721;
void nudge_ino(struct cpio_hdr *hdr) {
	if (xtol(hdr->ino))
		ltox(hdr->ino, global_ino++);
}

/* file chunk bits:
 * The magic __alloc field determines whether to free() each chunk
 * (facilitating e.g. packed allocations).  Otherwise, this is just a simple
 * linked list of buffers.
 *
 * The compression thread knows to pad the first and last chunks.
 */
struct file_chunk *file_chunk_alloc(struct file_chunk *c) {
	struct file_chunk *n;

	n = malloc(ALLOC_CHUNK_SZ);
	n->len = 0;
	n->__alloc = 1;
	n->buf = ((char *)n) + sizeof(struct file_chunk);
	n->next = NULL;

	c->next = n;
	return n;
}
void file_chunk_free(struct file_chunk *c) {
	if (c) {
		file_chunk_free(c->next);
		if (c->__alloc)
			free(c);
	}
}

/* cpio file entries:
 * These are a complete cpio header, with some file_chunk magic to dramatically
 * simplify the compression routines.
 */
struct cpio_ent *cpio_ent_alloc(void) {
	struct cpio_ent *e = malloc(ALLOC_CHUNK_SZ);

	if (e) {
		e->__poison = 0;
		e->data.len = 0;
		e->data.buf = (char *)&e->hdr;
		e->data.next = NULL;
		e->data.__alloc = 0;
	}

	return e;
}
void cpio_ent_free(struct cpio_ent *e) {
	file_chunk_free(&e->data);
	free(e);
}

/* file list bits:
 * Simple synchronized linked list implementation.  No frills.
 */
int file_list_init(struct cpio_file_list *l) {
	int ret;
	if (ret = pthread_mutex_init(&l->lock, NULL))
		return ret;
	if (ret = pthread_cond_init(&l->cond, NULL))
		return ret;
	return 0;
}
void file_list_push(struct cpio_file_list *l, struct cpio_ent *e) {
	e->next = NULL;

	pthread_mutex_lock(&l->lock);
	if (l->head)
		l->head->next = e;
	l->head = e;
	if (!l->tail) {
		l->tail = e;
		pthread_cond_signal(&l->cond);
	}
	pthread_mutex_unlock(&l->lock);
}
struct cpio_ent *file_list_pop(struct cpio_file_list *l) {
	struct cpio_ent *e;

	pthread_mutex_lock(&l->lock);
	if (!l->tail)
		pthread_cond_wait(&l->cond, &l->lock);
	e = (struct cpio_ent *)l->tail;
	if (e) {
		l->tail = e->next;
		if (!l->tail)
			l->head = NULL;
	} else {
		l->tail = NULL;
		l->head = NULL;
	}
	pthread_mutex_unlock(&l->lock);

	if (!e)
		rprint("File list exploded?!");
	return e;
}

/* If we blow up, unstick the other threads with a TRAILER!!!. */
static void decompress_cleanup(void *arg) {
	struct cpio_ent *e = cpio_ent_alloc();
	rprint("Aborting decompression");
	if (!e) {
		rprint("Out of memory, blowing up!");
		abort();
	}
	strcpy(e->hdr.name, "TRAILER!!!");
	e->data.len = CPIO_HDR_LEN + 10;
	file_list_push(&read_files, e);
}
/* decompress (with optional 4-byte pad) data into a file_chunk, allocating
 * more chunks as needed
 */
static int decompress(z_stream *strm, struct file_chunk *data,
		unsigned long len, int pad) {
	if (!len)
		return 0;
	if (!data || !data->buf)
		return -EINVAL;
	while (len) {
		strm->next_out = (uint8_t *)data->buf + data->len;
		if (len < CHUNK_DATA_SZ - data->len)
			strm->avail_out = len;
		else
			strm->avail_out = CHUNK_DATA_SZ - data->len;
		data->len += strm->avail_out;
		len -= strm->avail_out;
		/* We overcommit chunks to simplify this */
		if (!len && pad && (data->len & 3))
			strm->avail_out += 4 - (data->len & 3);

		while (strm->avail_out) {
			int ret = inflate(strm, Z_SYNC_FLUSH);
			if (ret != Z_OK && ret != Z_STREAM_END)
				return -EFAULT;
			if ((ret == Z_STREAM_END || !strm->avail_in) &&
					strm->avail_out) {
				rprint("Premature end of stream!");
				return -EFAULT;
			}
		}

		if (len) {
			data = file_chunk_alloc(data);
			if (!data)
				return -ENOMEM;
		}
	}
	return 0;
}

/* decompression:
 * Build a cpio_ent for each file, the push it to override_thread.
 */
struct cpio_file_list read_files;
static void *decompress_thread(void *arg) {
	z_stream *strm = (z_stream *)arg;
	struct cpio_ent *e;
	long ret = 0;
	int more = 1;

	pthread_cleanup_push(decompress_cleanup, NULL);

	do {
		e = cpio_ent_alloc();
		if (!e) {
			ret = -ENOMEM;
			goto out_fail;
		}

		/* Decompress and sanity-check header */
		if (ret = decompress(strm, &e->data, CPIO_HDR_LEN, 0))
			goto out_fail;
		if (strncmp(e->hdr.magic, CPIO_MAGIC, strlen(CPIO_MAGIC))) {
			rprint("Header mismatch!");
			ret = -EINVAL;
			goto out_fail;
		}
		if (!xtol(e->hdr.namesize)) {
			rprint("Bogus filename!");
			ret = -EINVAL;
			goto out_fail;
		}

		/* Decompress filename */
		if (ret = decompress(strm, &e->data, xtol(e->hdr.namesize), 1))
			goto out_fail;

		/* Maybe decompress body */
		if (!strncmp(e->hdr.name, "TRAILER!!!", 10)) {
			more = 0;
		} else if (xtol(e->hdr.size)) {
			if (ret = decompress(strm,
				file_chunk_alloc(&e->data),
				xtol(e->hdr.size), 1))
				goto out_fail;
		}
		file_list_push(&read_files, e);
	} while (more);
	inflateEnd(strm);

	pthread_cleanup_pop(0);

out_fail:
	return (void *)ret;
}

/* compression:
 * Pull cpio_ents from override_thread and compress them.
 */
struct cpio_file_list write_files;
static void *compress_thread(void *arg) {
	z_stream *strm = (z_stream *)arg;
	struct cpio_ent *e;
	struct file_chunk *c;
	int more = 1;
	long ret = 0;
	int byte_cnt;
	unsigned long pad = 0;

	do {
		e = file_list_pop(&write_files);
		if (!e) {
			ret = -EFAULT;
			goto out_fail;
		}
		if (e->__poison) {
			cpio_ent_free(e);
			continue;
		}

		nudge_ino(&e->hdr);
		byte_cnt = 0;

		for (c = &e->data; c; c = c->next) {
			strm->next_in = (uint8_t *)c->buf;
			strm->avail_in = c->len;
			byte_cnt += c->len;
			/* Pad header and file data */
			while (strm->avail_in) {
				if (deflate(strm, Z_NO_FLUSH) != Z_OK) {
					ret = -EFAULT;
					goto out_fail;
				}
			}
			if ((c == &e->data || !c->next) && (byte_cnt & 3)) {
				strm->avail_in = 4 - (byte_cnt & 3);
				byte_cnt += strm->avail_in;
				strm->next_in = (uint8_t *)&pad;
				while (strm->avail_in) {
					if (deflate(strm, Z_NO_FLUSH) != Z_OK) {
						ret = -EFAULT;
						goto out_fail;
					}
				}
			}
		}

		if (!strncmp(e->hdr.name, "TRAILER!!!", 10))
			more = 0;

		cpio_ent_free(e);
	} while (more);

	if (deflate(strm, Z_FINISH) != Z_STREAM_END) {
		rprint("Error finishing compression!");
		ret = -EFAULT;
		goto out_fail;
	}
	deflateEnd(strm);

out_fail:
	return (void *)ret;
}

/* override_thread:
 * Pull cpio_ents from decompress_thread, check them against the ramdisk
 * overrides, and push them to compress_thread.
 */
static int override_thread(void) {
	int ret;
	int more = 1;
	struct cpio_ent *e;

	do {
		e = file_list_pop(&read_files);
		if (!e)
			return -EFAULT;

		if (ret = ramdisk_handle_overrides(e)) {
			rprint("Patching file failed!");
			return ret;
		}

		if (!strncmp(e->hdr.name, "TRAILER!!!", 10))
			more = 0;

		file_list_push(&write_files, e);
	} while (more);

	return 0;
}

/* generate_ramdisk:
 * Set up the file queues, buffers, zlib streams and override miscellany.  Kick
 * off the decompress and compress threads, then start checking files for
 * overrides.
 */
#define RAMDISK_SIZE (5*1024*1024)
void *generate_ramdisk(void *arg) {
	long ret = 0;
	void *thread_ret;
	char *rdbuf, *oldrd;
	pthread_t decomp_th, comp_th;
	z_stream decomp, comp;

	/* Start decompression */
	decomp.zalloc = Z_NULL;
	decomp.zfree = Z_NULL;
	decomp.opaque = Z_NULL;
	decomp.avail_in = get_ramdisk(&oldrd);
	decomp.next_in = (unsigned char *)oldrd;

	if (decomp.avail_in < 0) {
		rprint("Error reading ramdisk!");
		ret = decomp.avail_in;
		goto out;
	}

	if (ret = inflateInit2(&decomp, 31)) {
		rprint("Error starting decompression!");
		goto out;
	}

	file_list_init(&read_files);
	pthread_create(&decomp_th, NULL, decompress_thread, (void *)&decomp);
#ifndef __ANDROID__
	mod_prio(decomp_th, SCHED_BATCH);
#endif

	/* Start compression */
	comp.zalloc = Z_NULL;
	comp.zfree = Z_NULL;
	comp.opaque = Z_NULL;
	comp.avail_out = RAMDISK_SIZE;
	rdbuf = malloc(RAMDISK_SIZE);
	comp.next_out = (uint8_t *)rdbuf;

	if (!comp.next_out) {
		rprint("Out of memory?!");
		ret = -ENOMEM;
		goto out;
	}

	if (ret = deflateInit2(&comp, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
		31, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
		rprint("Error starting compression!");
		goto out;
	}

	file_list_init(&write_files);
	pthread_create(&comp_th, NULL, compress_thread, (void *)&comp);

	/* Start passing files */
	ramdisk_init_overrides();
	if (ret = override_thread()) {
		rprint("Ramdisk patching failed!");
		goto out;
	}

	/* Verify sane exit status */
	if (ret = (long)pthread_join(decomp_th, &thread_ret)) {
		rprint("Pthreads exploded!");
		goto out;
	}
	if (thread_ret) {
		ret = (long)thread_ret;
		rprint("Decompression failed!");
		goto out;
	}
	if (ret = (long)pthread_join(comp_th, &thread_ret)) {
		rprint("Pthreads exploded!");
		goto out;
	}
	if (thread_ret) {
		ret = (long)thread_ret;
		rprint("Compression failed!");
		goto out;
	}

	/* Wait until compression finishes before freeing */
	ramdisk_free_overrides();
	free(oldrd);

	rprint("Compressed new ramdisk");
	add_ramdisk(rdbuf, RAMDISK_SIZE - comp.avail_out);

out:
	return (void *)ret;
}
