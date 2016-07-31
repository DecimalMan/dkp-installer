CC := arm-linux-musleabi-gcc
CFLAGS ?= -O3 -mthumb -mcpu=cortex-a15 -mfpu=neon
CFLAGS += -flto -fno-fat-lto-objects -flto-partition=none
CFLAGS += -fdata-sections -ffunction-sections -Wl,-gc-sections
CFLAGS += -s -static -pthread
CFLAGS += -I. -Izlib
CFLAGS += -pipe -std=gnu11 -Wall -Wno-parentheses -pedantic

# Build the full install package, including (un)mounting partitions?
CFLAGS += -DRECOVERY_BUILD
# Actually write the boot.img?
CFLAGS += -DWRITE_BOOTIMG

# core sources
SRC := src/main.c src/bootimg.c src/cpio.c src/override.c src/splash.c
SRC += src/system.c src/zimage.c

# sfpng
SRC += sfpng/src/sfpng.c sfpng/src/transform.c

# zlib
SRC += zlib/adler32_vec.c zlib/crc32.c zlib/deflate.c zlib/inffast.c zlib/inflate.c
SRC += zlib/inftrees.c zlib/trees.c zlib/zutil.c
CFLAGS += -DHAVE_HIDDEN -DZLIB_CONST
CFLAGS += -DDYNAMIC_CRC_TABLE -DUNALIGNED_OK

SRC += zlib/contrib/inflateneon/inflate_fast_copy_neon.s
CFLAGS += -D__ARM_HAVE_NEON

# minizip
SRC += zlib/contrib/minizip/unzip.c zlib/contrib/minizip/ioapi.c

update-binary:
	@echo CC $(notdir $@)
	@$(CC) $(CFLAGS) -o $@ $^
	@echo
	@size $@

# Build $(SRC) into obj/subdir-name.o, add deps for *.o and update-binary
$(foreach s,$(SRC),\
$(eval o := obj/$(if $(filter-out ./,$(dir $(s))),$(firstword $(subst /, ,$(dir $(s))))-)$(patsubst %.s,%.o,$(patsubst %.c,%.o,$(notdir $(s)))))\
$(eval tl := $(filter src,$(firstword $(subst /, ,$(s)))))\
$(eval update-binary: $(o))\
$(eval $(o): $(s) $(if $(tl),src/common.h src/cpio.h) Makefile))
obj/%.o:
	@echo CC $(notdir $@)
	@$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f obj/*.o

install: update-binary
	cp $^ ../../installer-dkp-aosp44/META-INF/com/google/android
	cp $^ ../../installer-dkp-aosp51/META-INF/com/google/android

test: CFLAGS += -URECOVERY_BUILD -UWRITE_BOOTIMG
test: update-binary

.PHONY: clean
