dkp-installer
=============

A kernel-focused update-binary.  While it's configured for dkp, very little is
set in stone.  A few changes to src/common.h and src/override.c should be
enough to adapt it for other kernel projects and/or other devices.

It is assumed that the update-binary will be zipped alongside some additional
files:
- dkp-zImage: a kernel zImage (see src/common.h to change the name)
- dkp-splash.png: an optional splash screen (src/common.h again)
- rd/: files in this directory can be injected into the ramdisk (see
  src/override.c)
- system/: files in this directory will be extracted to the /system partition

Features
--------

- Absolutely no Edify!
- Incredibly fast execution (typically 1-2 seconds)
- Small file size (somewhere around 100 KB)
- boot.img manipulation: zImage, ramdisk, and header manipulation
- Ramdisk repackaging: inject new initlogo.rle or arbitrary files from zip
- PNG to initlogo.rle conversion

Ramdisk Manipulation
--------------------

Support for completely overwriting the ramdisk is deliberately not provided,
though it wouldn't be hard to add.  Instead, consider modifying small parts of
the ramdisk: files can be inserted from the install zip (just use
```check_zip```), it's relatively easy to do zero-copy file modification (see
```patch_initrc```), and it's easy to hook other threads (see
```wait_for_gensplash```).  Modifying the existing ramdisk both improves
compatibility and shrinks the resulting install zip.

Building
--------

Rather than rely on external library binaries, the Makefile uses the source
distributions for zlib and sfpng.  ```git submodule init``` followed by ```git
submodule update``` will fetch the zlib and sfpng sources prior to building.

```make``` will generate an update-binary.

External libraries used
-----------------------

- zlib, (C) 1995-2013 Jean-loup Gailly and Mark Adler
- sfpng, (C) 2011 Evan Martin
- minizip, (C) 1998-2010 Gilles Vollant, et al.

TODO
----
- JPEG splash screen support
- Separate ramdisk manipulations per install target
- Implicit ```check_zip``` for everything in rd/
