#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>

#include "common.h"
#include "cpio.h"
#include <zlib/contrib/minizip/unzip.h>

#include <stdio.h>

// TODO: helper functions for manipulating file_chunks

enum override_flags {
	OVER_CREATE = 1<<0, /* create if (apparently) missing */
	OVER_POISON = 1<<1, /* duplicate; don't compress */
};
struct ramdisk_override {
	const char	*name;
	int		(*get_func)(struct cpio_ent *,
				    struct ramdisk_override *);
	int 		flags;
	char		*buf;
	unsigned int	size;
};

/* ramdisk file overrides:
 */
static int wait_for_gensplash(struct cpio_ent *e, struct ramdisk_override *o);
static int check_zip(struct cpio_ent *e, struct ramdisk_override *o);
static int patch_initrc(struct cpio_ent *e, struct ramdisk_override *o);
static int patch_qcomrc(struct cpio_ent *e, struct ramdisk_override *o);
static struct ramdisk_override overrides[] = {
	{ "dkp.profile.sh", check_zip, OVER_CREATE },
	{ "init.dkp.rc", check_zip, OVER_CREATE },
	{ "init.qcom.rc", patch_qcomrc },
	{ "init.rc", patch_initrc },
	{ "init.superuser.rc", check_zip, OVER_CREATE },
	//{ "init.target.rc", check_zip },
	{ "initlogo.rle", wait_for_gensplash },
	{ "mpdecision", check_zip, OVER_CREATE },
	{ NULL }
};

static unzFile zip;

/* ramdisk_push_override:
 * Provide a buffer for later overriding.  No synchronization is done.
 * Currently, splash screen generation is the only consumer, and wait_thread()
 * is sufficient.
 */
int ramdisk_push_override(char *name, char *buf, unsigned int size) {
	struct ramdisk_override *rdo = overrides;
	while (rdo->name) {
		if (!strcmp(name, rdo->name)) {
			if (rdo->buf) return -EBUSY;
			rdo->buf = buf;
			rdo->size = size;
			return 0;
		}
		rdo++;
	}

	return -ENOENT;
}

/* ramdisk_init_overrides:
 * Handle any preparation needed for overriding files.  Currently, that means
 * opening the install zip for check_zip().
 */
int ramdisk_init_overrides(void) {
	int ret = 0;
	if (!(zip = unzOpen(zip_path))) {
		ret = -ENOENT;
		goto out;
	}

out:
	return ret;
}

/* ramdisk_free_overrides:
 * Clean up data used for overriding files.  Currently, that means closing the
 * install zip and freeing any buffers not freed by the compression thread.
 */
void ramdisk_free_overrides(void) {
	struct ramdisk_override *rdo = overrides;

	while (rdo->name) {
		if (rdo->buf)
			free(rdo->buf);
		rdo->buf = NULL;
		rdo++;
	}

	unzClose(zip);
}

/* insert_file:
 * Create a valid cpio_ent & header, pass it to a get_func, and insert it in
 * the output file list.
 */
int insert_file(struct ramdisk_override *o) {
	int ret;
	struct cpio_ent *e = cpio_ent_alloc();
	if (!e) {
		rprint("Allocation failed!");
		return -ENOMEM;
	}

	/* Populate the header */
	memcpy(&e->hdr,
		/* magic */	"070701"
		/* ino */	"00000001"
		/* mode */	"00000000"
		/* uid */	"00000000"
		/* gid */	"00000000"
		/* nlink */	"00000001"
		/* mtime */	"00000000"
		/* size */	"00000000"
		/* major */	"00000000"
		/* minor */	"00000000"
		/* rmajor */	"00000000"
		/* rminor */	"00000000"
		/* namesize */	"00000000"
		/* chksum */	"00000000"
		, CPIO_HDR_LEN);
	// compress thread populates ino, but must already be non-zero
	ltox(e->hdr.mode, 0100750);
	//ltox(e->hdr.mtime, (unsigned long)time(NULL));
	ltox(e->hdr.namesize, strlen(o->name) + 1);
	// get_func populates size
	strcpy(e->hdr.name, o->name);
	e->data.len = CPIO_HDR_LEN + strlen(o->name) + 1;

	ret = o->get_func(e, o);
	if (!ret) {
		file_list_push(&write_files, e);
		o->flags |= OVER_POISON;
	} else {
		cpio_ent_free(e);
	}
	return ret;
}

/* ramdisk_handle_overrides:
 * Check overrides[] for any appropriate override functions.  If a file is
 * "missing" (i.e. strcmp shows we've passed its assumed location), use
 * insert_file to create it.
 */
int ramdisk_handle_overrides(struct cpio_ent *e) {
	int ret;
	int cmp;
	struct ramdisk_override *rdo = overrides;

	/* For bizarre ramdisks, don't add files before the . entry. */
	if (!strcmp(e->hdr.name, "."))
		return 0;

	while (rdo->name) {
		ret = 0;

		cmp = strcmp(e->hdr.name, rdo->name);
		if (cmp < 0) {
			rdo++;
			continue;
		}

		if (cmp > 0) {
			if (rdo->flags & OVER_CREATE &&
			    !(rdo->flags & OVER_POISON) &&
			    rdo->get_func) {
					ret = insert_file(rdo);
					rdo->get_func = NULL;
			}
		} else {
			if (rdo->flags & OVER_POISON) {
				e->__poison = 1;
			} else if (rdo->get_func) {
				ret = rdo->get_func(e, rdo);
				rdo->flags |= OVER_POISON;
			}
		}

		rdo++;

		if (ret < 0) {
			rprint("File patching failed, ignoring!");
			//break;
		}
	}
	//return ret < 0 ? ret : 0;
	return 0;
}

/* wait_for_gensplash:
 * Wait for the splash thread to complete, then (maybe) attach its buffer.
 */
static int wait_for_gensplash(struct cpio_ent *e, struct ramdisk_override *o) {
	int ret;
	struct file_chunk *c, *s;

	if (ret = wait_thread(THREAD_GENSPLASH)) {
		rprint("Not writing splash screen");
		return 0;
		//return ret;
	}

	s = e->data.next;
	if (!(c = file_chunk_alloc(&e->data))) {
		rprint("Allocation failed!");
		e->data.next = s;
		return -ENOMEM;
	}
	file_chunk_free(s);

	ltox(e->hdr.size, o->size);

	c->buf = o->buf;
	c->len = o->size;

	return 0;
}

/* check_zip:
 * Look for a file in rd/ inside the install zip.
 */
static int check_zip(struct cpio_ent *e, struct ramdisk_override *o) {
	unz_file_info info;
	int len, r, ret = 0;
	char zipf[40] = "rd/";
	struct file_chunk *c, *s;

	strcat(zipf, o->name);
	if (unzLocateFile(zip, zipf, 1) != UNZ_OK) {
		//rprint("File missing from zip!");
		//return -ENOENT;
#ifndef RECOVERY_BUILD
		printf("%s: skipping %s\n", __func__, o->name);
#endif
		/* Don't insert an empty file */
		return 1;
	}

	if (unzGetCurrentFileInfo(zip, &info, NULL, 0, NULL, 0, NULL, 0)
		!= UNZ_OK)
		return -EFAULT;
	s = e->data.next;
	if (!(c = file_chunk_alloc(&e->data)))
		return -ENOMEM;
	if (!(o->buf = malloc(info.uncompressed_size))) {
		ret = -ENOMEM;
		goto out;
	}
	//o->size = info.uncompressed_size;

	if (unzOpenCurrentFile(zip) != UNZ_OK) {
		ret = -EFAULT;
		goto out;
	}

	for (len = 0; len < info.uncompressed_size; ) {
		r = unzReadCurrentFile(zip, o->buf + len,
			info.uncompressed_size - len);
		if (r > 0) {
			len += r;
			continue;
		}
		if (r < 0) ret = -EFAULT;
		break;
	}

	if (len < info.uncompressed_size)
		ret = -EBADF;
	if (unzCloseCurrentFile(zip) == UNZ_CRCERROR)
		ret = -EBADF;

out:
	if (ret) {
		rprint("Error reading zipped file!");
		free(o->buf);
		o->buf = NULL;
		e->data.next = s;
	} else {
		file_chunk_free(s);
		c->buf = o->buf;
		c->len = info.uncompressed_size;
		ltox(e->hdr.size, c->len);
	}
	return ret;
}

/* patch_initrc:
 * Patch init.rc to include init.dkp.rc.
 */
#define IMPORTDKP "import /init.dkp.rc\n"
#define IMPORTSU "import /init.superuser.rc\n"
static int patch_initrc(struct cpio_ent *e, struct ramdisk_override *o) {
	struct file_chunk *c, *s;
	char *ptr, *last = NULL, *buf;
	int have_dkp = 0, have_su = 0;

	if (!e->data.next) {
		rprint("Missing init.rc data?!");
		return -EINVAL;
	}

	/* Add a new (packed) chunk. */
	s = e->data.next->next;
	c = file_chunk_alloc(e->data.next);
	if (!c) {
		rprint("Out of memory?!");
		e->data.next = s;
		return 0;
	}
	/* Make sure we can return safely */
	c[0].len = 0;
	c[0].buf = (char *)&c[2];
	c[0].next = &c[1];
	c[1].len = 0;
	c[1].buf = NULL;
	c[1].next = s;
	c[1].__alloc = 0;

	/* Split the chunk after the final import line */
	ptr = buf = e->data.next->buf;
	while (ptr = strstr(ptr, "\nimport /")) {
		/* While we're here, check which imports are present */
		ptr += strlen("\nimport /");
		if (!strncmp(ptr, "init.superuser.rc",
			strlen("init.superuser.rc")))
			have_su = 1;
		else if (!strncmp(ptr, "init.dkp.rc",
			strlen("init.dkp.rc")))
			have_dkp = 1;
		last = ptr;
	}
	if (!last) return 0;
	ptr = strchr(last, '\n');
	if (!ptr) return 0;
	ptr++;
	c[1].buf = ptr;
	c[1].len = e->data.next->len - (ptr - buf);
	e->data.next->len = ptr - buf;

	/* Built our import lines */
	c[0].buf[0] = '\0';
	if (!have_su && unzLocateFile(zip, "rd/init.superuser.rc", 1) == UNZ_OK)
		strcat(c[0].buf, "import /init.superuser.rc\n");
	if (!have_dkp && unzLocateFile(zip, "rd/init.dkp.rc", 1) == UNZ_OK)
		strcat(c[0].buf, "import /init.dkp.rc\n");
	c[0].len = strlen(c[0].buf);
	ltox(e->hdr.size, xtol(e->hdr.size) + c[0].len);

	return 0;
}

/* patch_qcomrc:
 * Comment out the power save profile hook.  It's replaced in init.dkp.rc.
 */
#define PERFLINE "on property:sys.perf.profile="
static int patch_qcomrc(struct cpio_ent *e, struct ramdisk_override *o) {
	char *ptr;

	if (!e->data.next) {
		rprint("Missing init.qcom.rc data?!");
		return -EINVAL;
	}

	ptr = strstr(e->data.next->buf, PERFLINE);
	if (!ptr) return 0;

	do {
		ptr = strchr(ptr, '\n');
		if (!ptr)
			break;

		ptr++;
		if (*ptr == ' ' || *ptr == '\t')
			*ptr = '#';
		else if (*ptr == '\r' || *ptr == '\n' || *ptr == '#')
			continue;
		else if (strncmp(ptr, PERFLINE, strlen(PERFLINE)))
			break;
	} while (1);

	return 0;
}
