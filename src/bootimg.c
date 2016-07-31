#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#include "bootimg.h"
#include "common.h"

#include <stdio.h>

/* Protects the boot partition and saved header. */
static struct boot_img_hdr boot_hdr;
static char *zimage = NULL;
static char *ramdisk = NULL;
static unsigned int zimage_size = 0, ramdisk_size = 0;

/* No need for locks here */
int add_ramdisk(void *buf, unsigned int size) {
	if (ramdisk) return -EBUSY;
	ramdisk = buf;
	ramdisk_size = size;
	return 0;
}
int add_zimage(void *buf, unsigned int size) {
	if (zimage) return -EBUSY;
	zimage = buf;
	zimage_size = size;
	return 0;
}

int get_ramdisk(char **rdbuf) {
	char *hdrbuf;
	int fd, rd, sz, ret = 0;
	if (!(hdrbuf = malloc(1024))) return -ENOMEM;
#ifdef RECOVERY_BUILD
	fd = open(BOOTPART, O_RDONLY);
#else
	fd = open(bootimg_path, O_RDONLY);
#endif
	if (fd < 0) {
		ret = -errno;
		goto out;
	}
	rd = read(fd, hdrbuf, 1024);
	if (rd < 0) {
		ret = -errno;
		goto out;
	}
	for (sz = 0; sz + sizeof(struct boot_img_hdr) < 1024; sz++) {
		if (!memcmp(&hdrbuf[sz], BOOT_MAGIC, BOOT_MAGIC_SIZE))
			goto found;
	}
	ret = -ENOENT;
	goto out;

found:
	memcpy(&boot_hdr, &hdrbuf[sz], sizeof(struct boot_img_hdr));
	rd = boot_hdr.page_size + boot_hdr.kernel_size;
	if (rd % boot_hdr.page_size)
		rd += boot_hdr.page_size - (rd % boot_hdr.page_size);
	if (lseek(fd, rd, SEEK_SET) == -1) {
		ret = -errno;
		goto out;
	}
	if (!(*rdbuf = malloc(boot_hdr.ramdisk_size))) {
		ret = -ENOMEM;
		goto out;
	}
	for (sz = 0; sz < boot_hdr.ramdisk_size; sz += rd) {
		rd = read(fd, *rdbuf + sz, boot_hdr.ramdisk_size - sz);
		if (rd < 0) {
			free(*rdbuf);
			*rdbuf = NULL;
			ret = -EIO;
			goto out;
		}
	}
	ret = boot_hdr.ramdisk_size;
out:
	free(hdrbuf);
	return ret;
}

int generate_bootimg(void) {
	int ret, bootfd;
#ifdef WRITE_BOOTIMG
	int wr;
#endif
	int pos;
	uint64_t szlim;

	/* Extracting the zImage should be done first */
	if (ret = wait_thread(THREAD_ZIMAGE)) return ret;
	/* Make sure ramdisk patching is successful before wiping out the
	 * existing image.
	 */
	if (ret = wait_thread(THREAD_RAMDISK)) return ret;

	rprint("Writing boot.img");
#ifdef RECOVERY_BUILD
	bootfd = open(BOOTPART, O_RDWR);
	if (!bootfd) return -errno;
	if (ioctl(bootfd, BLKGETSIZE64, &szlim)) return -errno;
#else
	printf("%s: %i @ %p, %i @ %p\n", __func__,
		zimage_size, zimage,
		ramdisk_size, ramdisk);
	bootfd = open(bootimg_path, O_RDWR);
	if (!bootfd) return -errno;
	szlim = 10 << 20;
#endif

	if (zimage_size + ramdisk_size + PGSZ * 3 > szlim) {
		rprint("Ramdisk is too big!");
		ret = -ENOSPC;
		goto ramdisk_close;
	}

	if (zimage_size + PGSZ * 3 > szlim) {
		rprint("zImage is too big!");
		ret = -ENOSPC;
		goto ramdisk_close;
	}
	if (lseek(bootfd, PGSZ, SEEK_SET) == -1) {
		rprint("zImage seek failed!");
		ret = -errno;
		goto ramdisk_close;
	}
#ifdef WRITE_BOOTIMG
	for (pos = 0; pos < zimage_size; pos += wr) {
		wr = write(bootfd, zimage + pos, zimage_size - pos);
		if (wr < 0) {
			rprint("Writing zImage failed!");
			ret = -errno;
			goto ramdisk_close;
		}
	}
#endif
	pos = zimage_size + PGSZ;
	if (pos & (PGSZ - 1)) pos = (pos & ~(PGSZ - 1)) + PGSZ;
	if (lseek(bootfd, pos, SEEK_SET) == -1) {
		rprint("Ramdisk seek failed!");
		ret = -errno;
		goto ramdisk_close;
	}
#ifdef WRITE_BOOTIMG
	for (pos = 0; pos < ramdisk_size; pos += wr) {
		wr = write(bootfd, ramdisk + pos, ramdisk_size - pos);
		if (wr < 0) {
			rprint("Writing ramdisk failed!");
			ret = -errno;
			goto ramdisk_close;
		}
	}
#endif

	/* Our header is fully populated, write it */
	boot_hdr.kernel_addr = KBASE + 0x8000;
	boot_hdr.kernel_size = zimage_size;
	boot_hdr.ramdisk_addr = KBASE + RDOFF;
	boot_hdr.ramdisk_size = ramdisk_size;
	boot_hdr.tags_addr = KBASE + 0x100;
	boot_hdr.page_size = PGSZ;
	if (lseek(bootfd, 0, SEEK_SET)) return -errno;
#ifdef WRITE_BOOTIMG
	for (pos = 0; pos < sizeof(boot_hdr); pos += wr) {
		wr = write(bootfd, ((char *)&boot_hdr) + pos,
			sizeof(boot_hdr) - pos);
		if (wr < 0) {
			rprint("Writing header failed!");
			ret = -errno;
			break;
		}
	}
#endif
ramdisk_close:
	close(bootfd);
	return ret;
}
