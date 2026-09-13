/* airies-boot host tooling: the small filesystem and parsing helpers the
 * commands share. Nothing here is clever; it exists so that the command
 * bodies read as the sequence of steps they are, and so that "copy a file and
 * get the mode right" is written once rather than four times.
 */

#ifndef AIRIES_CLI_UTIL_H
#define AIRIES_CLI_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <cli/common/status.h>

/* Sizes as a human writes them: 64M, 1G, 512K, 2048 (bytes). Both the SI and
 * the IEC spellings are accepted and both mean the binary multiple, because
 * a partition table has no use for a megabyte that is not 1048576 bytes and
 * silently disagreeing with the user about 1 GB would be worse than picking
 * a side. The literal "rest" is refused here; callers that accept it check
 * for it themselves, since only they know what the remainder is. */
cli_status util_parse_size(const char *text, uint64_t *out);

/* Bytes rendered for a human, into caller storage (needs 32 bytes). */
const char *util_format_size(uint64_t bytes, char *buf, size_t cap);

cli_status util_copy_file(const char *src, const char *dst, mode_t mode);
cli_status util_read_file(const char *path, void **buf, size_t *len,
                          size_t max);
cli_status util_write_file(const char *path, const void *buf, size_t len,
                           mode_t mode);
cli_status util_mkdir_p(const char *path, mode_t mode);

/* Overwrite a file's bytes before unlinking it. Used for the private key's
 * staging copy. Best effort by definition on a journalling filesystem, and
 * the comment at the call site says so; it still beats leaving the key in a
 * temp file for the next process that reuses the block. */
cli_status util_shred(const char *path);

/* dst = a "/" b, with the separator collapsed. Fails rather than truncates. */
cli_status util_join(char *dst, size_t cap, const char *a, const char *b);

const char *util_basename(const char *path);

/* Is this a regular file / block device / directory that exists? */
int util_is_file(const char *path);
int util_is_dir(const char *path);
int util_is_block(const char *path);

#endif /* AIRIES_CLI_UTIL_H */
