#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include "common.h"
#include <sfpng/src/sfpng.h>
#include <zlib/contrib/minizip/unzip.h>

#include <stdio.h>

#define SCREENX (720)
#define SCREENY (1280)
#define INITLOGO_SIZE (4*SCREENX*SCREENY)

#define to565(r,g,b) ((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
struct rle_block {
	unsigned short count, color;
};

struct rle_state {
	struct rle_block *blocks;
	unsigned char *linebuf;
	int blocknr;
	unsigned short x, y;
	/* Crop/pad? */
	/* Number to skip/pad before and after */
	unsigned short cropx, mincol, maxcol;
	/* Minimum and maximum row num or num to pad */
	unsigned short cropy, minrow, maxrow;
};
static int rle_compress_init(struct rle_state *st) {
	if (!(st->blocks = malloc(INITLOGO_SIZE))) return -ENOMEM;
	st->blocknr = 0;
	st->blocks[0].count = 0;
	return 0;
}
static struct rle_block *rle_compress_next_block(struct rle_state *st,
		unsigned short color) {
	struct rle_block *bl;
	bl = &st->blocks[st->blocknr];
	if (bl->count)
		bl = &st->blocks[++st->blocknr];
	bl->count = 0;
	bl->color = color;
	return bl;
}
#define min(a,b) (a<b?a:b)
static void rle_compress_pad(struct rle_state *st, int x, int y) {
	int num = 0, sub;
	struct rle_block *bl;

	if (!(x || y)) return;
	bl = &st->blocks[st->blocknr];
	if (bl->color)
		bl = rle_compress_next_block(st, 0);
	else
		num = bl->count;
	for (num += x + (y * SCREENX); num; num -= sub) {
		sub = min(num, 65535);
		bl->count = sub;
		bl = rle_compress_next_block(st, 0);
	}
}
static void rle_compress_row(sfpng_decoder *dec, int row, const uint8_t* buf,
		int len) {
	struct rle_state *st;
	struct rle_block *bl;
	unsigned short color;

	st = sfpng_decoder_get_context(dec);
	if (st->cropy && (row < st->minrow || row > st->maxrow)) return;

	sfpng_decoder_transform(dec, 0, buf, st->linebuf);
	buf = st->linebuf;

	// Point the buffer to the right columns
	if (st->cropx) {
		len = SCREENX;
		buf = buf + st->mincol * 4;
	} else {
		len = st->x;
		rle_compress_pad(st, st->mincol, 0);
	}
	bl = &st->blocks[st->blocknr];
	while (len--) {
		color = to565(buf[0], buf[1], buf[2]);
		if (color != bl->color || bl->count == 65535)
			bl = rle_compress_next_block(st, color);
		bl->count++;
		buf += 4;
	}
	if (!st->cropx)
		rle_compress_pad(st, st->maxcol, 0);
}
static void rle_compress_prepare(sfpng_decoder *dec) {
	struct rle_state *st;
	int x, y;

	st = sfpng_decoder_get_context(dec);
	x = sfpng_decoder_get_width(dec);
	// Sure would be nice to check this.
	st->linebuf = malloc(4 * x);
	if (x < SCREENX) {
		st->cropx = 0;
		st->mincol = (SCREENX - x) / 2;
		st->maxcol = SCREENX - x - st->mincol;
	} else if (x > SCREENX) {
		st->cropx = 1;
		st->mincol = (x - SCREENX) / 2;
		st->maxcol = x - SCREENX - st->mincol;
	} else {
		st->cropx = 1;
		st->mincol = st->maxcol = 0;
	}
	y = sfpng_decoder_get_height(dec);
	if (y < SCREENY) {
		st->cropy = 0;
		st->minrow = (SCREENY - y) / 2;
		st->maxrow = SCREENY - y - st->minrow;
	} else if (y > SCREENY) {
		st->cropy = 1;
		st->minrow = (y - SCREENY) / 2;
		st->maxrow = SCREENY + st->minrow - 1;
	} else {
		st->cropy = 1;
		st->minrow = 0;
		st->maxrow = 1279;
	}
	st->x = x;
	st->y = y;
	rle_compress_pad(st, 0, st->cropy ? 0 : st->minrow);
}

static inline int should_skip_splash(void) {
	int fd = open(SKIPSPLASH, O_RDONLY);
	if (fd == -1) return 0;
	close(fd);
	return 1;
}

#define CHUNK_SIZE (32*1024)
static int try_userrle(void) {
	int fd;
	int rd = 1, sz;
	char *buf;

	fd = open(USERRLE, O_RDONLY);
	if (fd == -1) return -ENOENT;

	if (!(buf = malloc(INITLOGO_SIZE))) return -ENOMEM;
	for (sz = 0; rd; sz += rd) {
		rd = read(fd, buf + sz, INITLOGO_SIZE - sz);
		if (rd < 0) return -errno;
	}
	return ramdisk_push_override("initlogo.rle", buf, sz);
}
static int try_userpng(sfpng_decoder *dec, char *buf) {
	int fd, rd;
	sfpng_status stat;

	fd = open(USERPNG, O_RDONLY);
	if (fd == -1) return -ENOENT;
	do {
		rd = read(fd, buf, CHUNK_SIZE);
		if (rd < 0) {
			rprint("Reading PNG failed!");
			break;
		}
		stat = sfpng_decoder_write(dec, buf, rd);
		if (stat != SFPNG_SUCCESS) {
			rprint("PNG is invalid!");
			break;
		}
	} while (rd > 0);
	return 0;
}

#ifdef ZIPFMT
static char random_zip_name[] = ZIPFMT;
static void randomize_zipsplash(void) {
	unsigned char rand = 0;
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd == -1)
		goto bail;
	if (read(fd, &rand, sizeof(rand)) <= 0)
		goto bail;
	close(fd);

bail:
	rand &= 0x7;
	random_zip_name[10] = '0' + rand;
}
#else
static const char random_zip_name[] = ZIPPNG;
static inline void randomize_zipsplash(void) { }
#endif

static void splash_display_text(sfpng_decoder *dec,
		const char *keyword, const uint8_t *text, int len) {
	char print[80] = "ui_print Using a splash screen from ";
	if (strncmp((const char *)keyword, "Author", 12))
		return;
	if (len > 30) len = 30;
	strncat(print, (const char *)text, len);
	strcat(print, "\nui_print\n");
	iwrite(cmdfd, print);
}

static int try_zippng(sfpng_decoder *dec, char *buf) {
	int ret = 0;
	int rd;
	unzFile zip;
	sfpng_status stat;

	if (!(zip = unzOpen(zip_path))) {
		ret = -ENOENT;
		rprint("Can't open zip!");
		goto out;
	}
	randomize_zipsplash();
	if (unzLocateFile(zip, random_zip_name, 1) != UNZ_OK) {
		ret = -ENOENT;
		//rprint("Can't find PNG in zip!");
		goto out_close;
	}
	if (unzOpenCurrentFile(zip) != UNZ_OK) {
		ret = -EBADF;
		rprint("Can't open PNG in zip!");
		goto out_close;
	}

	do {
		rd = unzReadCurrentFile(zip, buf, CHUNK_SIZE);
		if (rd < 0) {
			ret = -4;
			rprint("Reading PNG failed!");
			break;
		}
		stat = sfpng_decoder_write(dec, buf, rd);
		if (stat != SFPNG_SUCCESS) {
			ret = -4;
			rprint("PNG is invalid!");
			break;
		}
	} while (rd > 0);

	if (unzCloseCurrentFile(zip) == UNZ_CRCERROR)
		rprint("Corrupt PNG in zip!");

out_close:
	unzClose(zip);
out:
	return ret;
}

void *generate_splash(void *arg) {
	long ret;
#ifdef RECOVERY_BUILD
	int do_umount = 1;
#endif
	int do_data = 1;
	char *buf;
	struct rle_state st;
	sfpng_decoder *dec;

#ifdef RECOVERY_BUILD
	mkdir("/data", 0755);
	if (ret = mount(STORAGEPART, "/data", "ext4",
		MS_NOATIME | MS_NODEV | MS_NODIRATIME, "")) {
		do_umount = 0;
		if (errno != EBUSY) do_data = 0;
	}
#endif

	if (should_skip_splash()) goto out_umount;

	if (do_data) {
		if (!(ret = try_userrle())) goto out_umount;
	}

	if (!(buf = malloc(CHUNK_SIZE))) {
		ret = -ENOMEM;
		goto out_umount;
	}
	if (ret = rle_compress_init(&st))
		goto out_umount;
	dec = sfpng_decoder_new();
	sfpng_decoder_set_context(dec, &st);
	sfpng_decoder_set_info_func(dec, rle_compress_prepare);
	sfpng_decoder_set_row_func(dec, rle_compress_row);
	sfpng_decoder_set_text_func(dec, splash_display_text);

	if (do_data) {
		if (!(ret = try_userpng(dec, buf))) goto finish_rle;
	}
	if (!(ret = try_zippng(dec, buf))) goto finish_rle;
	goto out_free;

finish_rle:
	if (!st.cropy)
		rle_compress_pad(&st, 0, st.maxrow);
	if (st.blocks[st.blocknr].count)
		st.blocknr++;
	rprint("Converted splash screen");
	ret = ramdisk_push_override("initlogo.rle", (char *)st.blocks,
		st.blocknr * sizeof(struct rle_block));

out_free:
	sfpng_decoder_free(dec);
	free(buf);

out_umount:
#ifdef RECOVERY_BUILD
	if (do_umount) umount("/data");
#endif
	return (void *)ret;
}
