/* common.h: device bits, common functions, etc.
 * Copyright (C) 2014 Ryan Pennucci <decimalman@gmail.com>
 */

#ifndef _COMMON_H
#define _COMMON_H

#include <string.h>
#include <stdio.h>
#include <pthread.h>

/* boot partition */
#define BOOTPART "/dev/block/mmcblk0p7"
/* /data */
#define STORAGEPART "/dev/block/mmcblk0p15"
/* /system */
#define SYSTEMPART "/dev/block/mmcblk0p14"

/* boot.img params */
#define KBASE (0x80200000)
#define RDOFF (0x1500000)
#define PGSZ (2048)

/* Inside the zip */
#define ZIMAGE	"dkp-zImage"
#define ZIPPNG	"dkp-splash.png"
//#define ZIPFMT	"dkp-splashX.png"

#define SKIPSPLASH "/data/media/0/dkp/skipsplash"
#define USERRLE "/data/media/0/dkp/splash.rle"
#define USERPNG "/data/media/0/dkp/splash.png"

/* Path to zip, provided by recovery */
extern char *zip_path;
/* Recovery cmd fd */
extern int cmdfd;
// Lazy write to file
static inline int iwrite(int f, char *s)
{ return write(f, s, strlen(s)); }
// Print to recovery console
#ifdef RECOVERY_BUILD
#define rprint(s) iwrite(cmdfd, "ui_print " s "\nui_print\n")
#else
#define rprint(s) printf("%s: " s "\n", __func__)
#endif

/* main.c */
enum thread_slot {
	THREAD_ZIMAGE,
	THREAD_SYSTEM,
	THREAD_GENSPLASH,
	THREAD_RAMDISK,
	THREAD_COUNT,
};
int wait_thread(enum thread_slot slot);
void mod_prio(pthread_t thread, int new_pol);
#ifndef RECOVERY_BUILD
extern char *bootimg_path;
#endif

/* bootimg.c */
int generate_bootimg(void);
int get_ramdisk(char **rdbuf);
int add_ramdisk(void *buf, unsigned int size);
int add_zimage(void *buf, unsigned int size);

/* splash.c */
void *generate_splash(void *arg);

/* ramdisk.c */
int ramdisk_push_override(char *name, char *buf, unsigned int size);
void *generate_ramdisk(void *arg);

/* system.c */
void *unpack_system(void *arg);

/* zimage.c */
void *unpack_zimage(void *arg);

#endif
