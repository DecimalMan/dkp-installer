#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <linux/fs.h>

#include "common.h"
#include <zlib/zlib.h>
#include <zlib/contrib/minizip/unzip.h>

#define CHUNK_SIZE (32*1024)
void *unpack_system(void *arg) {
	long ret;
	unzFile zip;
#ifdef RECOVERY_BUILD
	int do_umount = 1;
	int rd, wr, fd;
#else
	int rd;
#endif
	char namebuf[256];
	char *buf = 0;

	if (!(buf = malloc(CHUNK_SIZE))) goto out;
	if (!(zip = unzOpen(zip_path))) {
		ret = -ENOENT;
		rprint("Can't open zip!");
		goto out;
	}
	if (unzGoToFirstFile(zip) != UNZ_OK) {
		ret = -EIO;
		rprint("Zip is corrupt!");
		goto out_close;
	}
	if (unzLocateFile(zip, "system/", 1) != UNZ_OK) {
		unzClose(zip);
#ifndef RECOVERY_BUILD
		rprint("No system/ dir in zip, skipping.");
#endif
		return 0;
	}

#ifdef RECOVERY_BUILD
	mkdir("/system", 0755);
	if (mount(SYSTEMPART, "/system", "ext4",
		MS_NOATIME | MS_NODEV | MS_NODIRATIME, "")) {
		if (errno != EBUSY) {
			rprint("Couldn't mount system!");
			return (void *)(-(long)errno);
		}
		do_umount = 0;
	}
#endif

	do {
		char *scan;
		ret = unzGetCurrentFileInfo(zip, NULL, namebuf, 256,
			NULL, 0, NULL, 0);
		if (ret == UNZ_END_OF_LIST_OF_FILE)
			break;
		else if (ret != UNZ_OK) {
			rprint("Can't get file info!");
			ret = -EIO;
			goto out_close;
		}
		if (strncmp(namebuf, "system/", 7))
			break;
		/* For now, just skip directories */
		for (scan = namebuf; *scan; scan++);
		scan--;
		if (*scan == '/')
			continue;
		ret = unzOpenCurrentFile(zip);
		if (ret != UNZ_OK) {
			rprint("Error opening file in zip!");
			continue;
		}
#ifdef RECOVERY_BUILD
		fd = open(namebuf, O_WRONLY | O_CREAT | O_TRUNC, 0755);
		if (fd < 0) {
			rprint("Unable to create file!");
			continue;
		}
		do {
			int cnt;
			cnt = rd = unzReadCurrentFile(zip, buf, CHUNK_SIZE);
			for (wr = 0; cnt > 0; cnt -= wr)
				wr = write(fd, buf + wr, cnt);
		} while (rd > 0);
		if (close(fd))
			rprint("Error writing file!");
#else
		printf("%s: extracting %s\n", __func__, namebuf);
		do {
			rd = unzReadCurrentFile(zip, buf, CHUNK_SIZE);
		} while (rd > 0);
#endif
		if (unzCloseCurrentFile(zip) == UNZ_CRCERROR)
			rprint("CRC error!");
	} while (unzGoToNextFile(zip) == UNZ_OK);

#if defined(RECOVERY_BUILD) && defined(TW_SELINUX_HACK)
	fd = open("/system/etc/sec_config", O_RDONLY);
	if (fd != -1) {
		fd = close(fd);
		if (!unlink("/system/etc/selinux_restore"))
			rprint("Cleaned SELinux contexts");
	}
#endif

#ifdef RECOVERY_BUILD
	if (do_umount) umount("/system");
#endif
	rprint("Unpacked system files");

out_close:
	unzClose(zip);
out:
	if (ret)
		rprint("Error unpacking system files");
	if (buf) free(buf);
	return (void *)ret;
}
