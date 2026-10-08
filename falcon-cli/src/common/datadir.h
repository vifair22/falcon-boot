/* falcon-boot: where mkstick finds what it installs (docs/Manual.md
 * Section 11): the loaders, BIOS stages and modules under bin/, shim and
 * MokManager under shim/bins/, named as the build names them. A tree build
 * reads its own build/ from the repository root; a release build is
 * compiled with FALCON_DATADIR set to where its package puts that tree
 * (the Makefile's RELEASE_DATADIR). Every such default is still an option.
 */

#ifndef FALCON_DATADIR_H
#define FALCON_DATADIR_H

#ifndef FALCON_DATADIR
#define FALCON_DATADIR "build"
#endif

#endif
