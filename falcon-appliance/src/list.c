/* falcon-boot appliance: the bless list the loader delivered (DESIGN Section
 * 7.5).
 *
 * The user chose what to bless in the loader's menu and confirmed it there;
 * the loader hands the paths over as a credential beside the passphrase, and
 * sd-stub leaves it in /.extra/credentials/. This reads it and surveys each
 * path exactly as the walk would have found it, so an image already blessed,
 * or one that is Windows media, is treated here the way the menu-less run
 * treats it. A path the list may not carry (falcon-core/src/manifest/
 * blesslist.c has the rule) or that is not a regular file on p2 is reported
 * and left out; nothing else on p2 is looked at.
 */

#include "appliance.h"

#include <manifest/blesslist.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LIST_PATH "/.extra/credentials/" BLESSLIST_CRED

static char g_list[BLESSLIST_MAX];

int bless_list_survey(void)
{
    char rel[MANIFEST_PATH_MAX];
    size_t len = 0, pos = 0;
    int fd = open(LIST_PATH, O_RDONLY | O_CLOEXEC);
    int rc;

    if (fd < 0)
        return 0;
    for (;;) {
        ssize_t n = read(fd, g_list + len, sizeof(g_list) - len);

        if (n <= 0)
            break;
        len += (size_t)n;
        if (len == sizeof(g_list))
            break;
    }
    (void)close(fd);
    if (len == 0)
        return 0;

    say("bless list from the loader:\n");
    while ((rc = blesslist_next(g_list, len, &pos, rel, sizeof(rel))) != 0) {
        char abs[PATH_MAX];
        struct stat st;

        if (rc != 1) {
            say("  refused     a line that is not a path on p2\n");
            continue;
        }
        if (snprintf(abs, sizeof(abs), "%s/%s", P2_MNT, rel) >=
                (int)sizeof(abs) ||
            lstat(abs, &st) != 0 || !S_ISREG(st.st_mode)) {
            say("  missing     /%s\n", rel);
            continue;
        }
        survey_file(abs, rel, (uint64_t)st.st_size);
    }
    return 1;
}
