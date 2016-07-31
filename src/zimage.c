#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>

#include "common.h"
#include <zlib/zlib.h>
#include <zlib/contrib/minizip/unzip.h>

#include <stdio.h>

#define ZIMAGE_SIZE (8*1024*1024)
void *unpack_zimage(void *arg) {
	long ret;
	int rd = 1, pos;
	unzFile zip;
	char *buf;

	if (!(buf = malloc(ZIMAGE_SIZE))) return (void *)-ENOMEM;

	if (!(zip = unzOpen(zip_path))) {
		ret = -ENOENT;
		rprint("Can't open zip!");
		goto out_free;
	}
	if (unzLocateFile(zip, ZIMAGE, 1) != UNZ_OK) {
		ret = -ENOENT;
		rprint("zImage is missing!");
		goto out_close;
	}
	if (unzOpenCurrentFile(zip) != UNZ_OK) {
		ret = -EBADF;
		rprint("Can't open zImage!");
		goto out_close;
	}

	for (pos = 0; rd > 0; pos += rd) {
		rd = unzReadCurrentFile(zip, buf + pos, ZIMAGE_SIZE - pos);
	}
	if (rd) {
		ret = -EIO;
		rprint("Reading zImage failed!");
		goto out_close;
	}

	if (unzCloseCurrentFile(zip) == UNZ_CRCERROR)
		rprint("Corrupt zImage in zip!");

	unzClose(zip);
	ret = add_zimage(buf, pos);
	rprint("Unpacked new zImage");
	return (void *)ret;

out_close:
	unzClose(zip);
out_free:
	free(buf);
	return (void *)ret;
}
