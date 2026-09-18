/* falcon-boot-sign: the seams the ISO view's tests reach it through.
 *
 * The public surface is in sign.h. What is here builds a view two other ways:
 * over any read callback, so a test can hand it an image in memory or a
 * fixture without a path, and from a bare list of paths, so the matching,
 * the case folding and the kernel table can be tested with no image at all.
 */

#ifndef FALCON_CLI_SIGN_ISOPROBE_H
#define FALCON_CLI_SIGN_ISOPROBE_H

#include <stddef.h>
#include <stdint.h>

#include <fs/fs.h>
#include <sign/sign.h>

/* iso_view_open over a read callback instead of a file. ctx must outlive the
 * view: the mounted volume reads through it until iso_view_close. what names
 * the image in messages. */
int iso_view_open_cb(fs_read_fn read, void *ctx, const char *what,
                     iso_view **out);

/* A view from a NULL-terminated list of paths (a trailing '/' marks a
 * directory) and, optionally, a parallel array of sizes. No image stands
 * behind it: it answers layout questions and reports every member's content
 * as unreadable. */
int iso_view_from_paths(const char *const *paths, const uint64_t *sizes,
                        iso_view **out);

/* How many entries the view indexed. */
size_t iso_view_entries(const iso_view *v);

#endif /* FALCON_CLI_SIGN_ISOPROBE_H */
