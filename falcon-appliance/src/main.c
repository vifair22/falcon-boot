/* falcon-boot: the on-stick signing appliance's PID 1 (DESIGN Section 7.5).
 *
 * The appliance is a signed UKI that boots under full Secure Boot, surveys the
 * ISO library on p2 and blesses what the user vouches for, through the SAME
 * `falcon-boot-sign` implementation the host route runs (here the static
 * link, /usr/bin/falcon-boot-sign-static). Then it reboots. That is the whole
 * program; there is no shell in this initramfs and nothing else to run.
 *
 *   An ISO is BLESSED (DESIGN Section 7.5). p3 is unlocked, the MOK private
 *   key that lives there is read, and the user marks the unsigned ISOs one by
 *   one and then confirms the marked set as a whole.
 *
 *   A bare EFI payload (a .efi, or a .img whose payload is one) is blessed
 *   too, and it is the one bless that WRITES TO THE USER'S OWN FILE: the
 *   payload boots as itself, so there is no wrap to put the signature in
 *   instead (DESIGN Section 7.2). The review therefore names every file that
 *   will be written to, inside a .img as well as outside one, in the sign
 *   tool's own words, before the yes that releases it.
 *
 *   Windows install media is owed nothing. The loader patches its boot.wim in
 *   memory as it boots it (DESIGN Section 6.4), so the survey reports it and
 *   leaves it alone.
 *
 * The survey reads p2 alone, and p3 is opened only once it has found
 * something to sign. A run with nothing pending decrypts no key and asks for
 * no passphrase.
 *
 * One instruction comes from the loader, and only one: the bless list
 * (list.c). When the user blesses from the loader's menu, the paths they
 * confirmed there arrive as a credential beside the passphrase, and this
 * program signs exactly those, with no UI of its own, and reboots (DESIGN
 * Section 7.5). Without a list it is the interactive
 * appliance described above. `FalconNextBoot` stays the loader's variable
 * and its whole lifecycle the loader's (DESIGN Section 6.3.1: one component
 * sets it and clears it); nothing here reads it or writes it.
 *
 * Why a C init rather than busybox: the appliance is the only place besides an
 * external host where the private key is ever decrypted, so the userland
 * around that moment should be small enough to read in one sitting. busybox
 * would bring several hundred applets to run four programs.
 *
 * What this program must never do, and the reasons, so nobody "improves" it:
 *
 *   - It never signs anything itself. It reads the keypair off the LUKS
 *     volume into two sealed memfds (shelf.c) and hands `sign` their
 *     /proc/self/fd paths, which `sign` hands to sbsign. The key is never on
 *     a disk, never in a file anyone can open after this program ends, and
 *     least of all on p2 (DESIGN Section 7.5).
 *   - It never unlocks p3 when nothing is pending. A run with nothing to sign
 *     has no use for the key, so decrypting it would widen the window it is
 *     decrypted in for nothing, and the smallness of that window is the
 *     reason the appliance exists rather than a full OS (DESIGN Section 7.5).
 *   - The passphrase never leaves this process: p3 is unlocked in process
 *     with falcon-core's LUKS2, the passphrase is never in argv or a file,
 *     and it is wiped from memory as soon as each attempt returns. Core dumps
 *     are disabled at entry for the same reason.
 *   - p3 is only ever READ. It is opened O_RDONLY and walked with core's
 *     ext4 reader; no device-mapper node or mount exists for anything else
 *     to write through. Nothing in the signing flow writes to the shelf: the
 *     UKI wrap and the manifest record both land on p2.
 *   - Nothing is signed that the user did not mark by hand and then confirm
 *     (here, or in the loader's menu for a bless list). The per-image mark
 *     plus the typed "yes" over the reviewed list together ARE the "I vouch
 *     for these" act the design rests on: they are what stops an ISO someone
 *     dropped on the stick from being blessed because it happened to be
 *     sitting there. The list is never pre-marked, and the confirmation
 *     prints every path it is about to act on, so nothing can be swept in
 *     unseen (DESIGN Section 7.5, select-then-sign).
 *   - There is no network in this initramfs (no interface is brought up, and
 *     nothing here would use one), so the decrypted-key window cannot be
 *     reached from outside the machine.
 *
 * The partitions are identified by their records (DESIGN Section 3.1): the
 * loader hands over the stick's install ID as a credential, and only the
 * exFAT library and the LUKS2 shelf whose records carry it are opened
 * (stickid.c), read with the loader's own record, exFAT and LUKS2 readers,
 * so the appliance and the loader cannot disagree about which partition is
 * which, and a second stick plugged in beside this one is never touched. The manifest is read through
 * falcon-core/src/manifest/manifest.c, the same parser the loader will use, so
 * "already blessed" means here exactly what it will mean at the menu. Each ISO
 * is classified through falcon-cli/src/sign/isoprobe.c and
 * falcon-core/src/media/family.c, which is the reader and the table
 * `falcon-boot-sign` itself uses, so the survey cannot put an image in a queue
 * the tool will then refuse it from. The set of file names the survey looks at
 * is the LOADER's set (apps/stick/efi_main.c: *.iso, *.img, *.efi),
 * because an appliance that disagreed with the menu about what counts as media
 * would be unable to bless something the user can see and boot.
 *
 * Built by the Makefile (falcon-appliance/build.mk) and linked static, since
 * it is PID 1 in an initramfs whose dynamic loader exists for the other
 * binaries, not for this one; falcon-appliance/image/mk-appliance.sh stages
 * it as /init. The program is split by job across falcon-appliance/src, and
 * appliance.h has the map.
 */


#include "appliance.h"

#include <ident/ident.h>

#include <sign/sign.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static void early_mounts(void)
{
    (void)mkdir("/proc", 0555);
    (void)mkdir("/sys", 0555);
    (void)mkdir("/tmp", 01777);
    (void)mkdir("/run", 0755);
    (void)mkdir("/mnt", 0755);
    (void)mkdir(P2_MNT, 0755);

    (void)mount("proc", "/proc", "proc", 0, NULL);
    (void)mount("sysfs", "/sys", "sysfs", 0, NULL);
    /* The work area for kernel extraction and the unsigned wrap. RAM only:
     * nothing a signing run produces before the signature exists should be
     * able to outlive the run on persistent storage. */
    (void)mount("tmpfs", "/tmp", "tmpfs", 0, "mode=1777");
}

static void load_modules(void)
{
    struct dirent **ents = NULL;
    int n = scandir(MODULE_DIR, &ents, NULL, alphasort);
    int i;

    if (n < 0) {
        say("note: no %s directory in this initramfs\n", MODULE_DIR);
        return;
    }
    for (i = 0; i < n; i++) {
        char path[PATH_MAX];
        size_t len = strlen(ents[i]->d_name);
        int fd;

        if (len < 4u || strcmp(ents[i]->d_name + len - 3u, ".ko") != 0) {
            free(ents[i]);
            continue;
        }
        (void)snprintf(path, sizeof(path), "%s/%s", MODULE_DIR,
                       ents[i]->d_name);
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            say("note: %s could not be opened (%s)\n", path, strerror(errno));
            free(ents[i]);
            continue;
        }
        if (syscall(SYS_finit_module, fd, "", 0) != 0 && errno != EEXIST) {
            /* Not fatal on its own: a kernel with the facility built in
             * refuses the module and the flow works anyway. The failure that
             * matters shows up as a failed unlock or a failed mount, with a
             * message that says which. */
            say("note: %s did not load (%s)\n", ents[i]->d_name,
                strerror(errno));
        }
        (void)close(fd);
        free(ents[i]);
    }
    free(ents);
}

/* ---- teardown ---- */

/* p3 is closed by the bless half itself (shelf.c keeps nothing mapped or
 * mounted), so p2 is all there is to unwind. A failure here is reported and
 * pressed on with, because the next step is a reboot either way. */
static void teardown(int p2_mounted)
{
    if (p2_mounted && umount(P2_MNT) != 0)
        say("note: could not unmount p2: %s\n", strerror(errno));
    sync();
}

static void finish(int p2_mounted, int wait)
{
    char line[8];

    /* A run the loader asked for that went as asked has nothing left to
     * read: the reboot is the next step the user is waiting for. */
    if (!wait) {
        say("\nrebooting to the loader.\n");
        teardown(p2_mounted);
        (void)sleep(2);
        (void)reboot(RB_AUTOBOOT);
        for (;;)
            (void)pause();
    }

    /* The summary (or an error) is the last thing the user needs to read, and
     * an immediate reboot snatches it off the screen. Waiting on Enter costs a
     * keystroke. A failed read now means every console this machine has went
     * quiet, not that the one console was the wrong one (see the console
     * section above), but it must still not fall straight through: the case
     * where nothing can be read is exactly the case where a screen someone is
     * reading may be all there is. A run with nobody at it ends on its own,
     * just later. */
    say("\npress Enter to reboot to the loader.\n");
    if (read_line(line, sizeof(line)) < 0) {
        say("(no readable console; rebooting in 30 seconds)\n");
        (void)sleep(30);
    }
    say("rebooting.\n");
    teardown(p2_mounted);
    (void)sleep(1);
    (void)reboot(RB_AUTOBOOT);
    for (;;)
        (void)pause();
}

/* "Already blessed" for a bare payload, applied to the queue the survey built.
 *
 * It is a property of the file rather than of a record: our exact certificate
 * in the PE's certificate table, which is the tool's own idempotence rule
 * (falcon-cli/src/sign/payload.h) rather than a second answer invented here.
 * The survey cannot ask it, because the certificate that defines "ours" lives
 * on p3 and p3 is still locked while p2 is walked. So it is asked once, here,
 * between the unlock and the listing the user marks. Asking it earlier would
 * mean reading a certificate from somewhere unencrypted and letting that decide
 * what this stick signs with, which is a second and editable answer to a
 * question p3 already answers.
 *
 * A .efi is asked directly. A payload inside a .img is not: the question
 * lives inside the image, sgn_payload_signed_with takes a path, and reaching
 * those paths means the volume walk payload.c already implements. Rather than
 * a second copy of it, such an image keeps its place in the list and the tool
 * answers per volume when it runs - it signs nothing there and says "already
 * signed with this key, left alone", which reaches the console with the rest
 * of its output. */

/* ---- the flow ---- */

int main(void)
{
    stick_parts parts;
    int p2_mounted = 0;
    int listed = 0;
    size_t signed_n = 0;
    size_t failed_n = 0;
    size_t skipped_n = 0;
    struct rlimit no_core = { 0, 0 };

    /* Before anything reads a passphrase: this process is about to hold one,
     * and a core file would be a copy of it on whatever filesystem is current. */
    (void)setrlimit(RLIMIT_CORE, &no_core);

    console_open();
    early_mounts();

    say("\n"
        "falcon-boot signing appliance\n"
        "=============================\n"
        "Signs what is on p2 - ISOs, and the bare EFI payloads beside them -\n"
        "with the MOK private key encrypted on p3.\n\n");

    /* The sign tool's ISO reader is linked in here for the survey. Its own
     * diagnostics are about listing a file, which is this program's plumbing
     * rather than its output, so only warnings and errors reach the console.
     * The tool's INFO-level progress during a real bless still does: that
     * runs in a child process with its own threshold. */
    sgn_log_set_level(SGN_LOG_WARN);

    load_modules();

    switch (find_stick(&parts)) {
    case STICK_FOUND:
        break;
    case STICK_NO_ID:
        say("ERROR: the loader passed no install ID (" IDENT_CRED ").\n"
            "       Start the appliance from the falcon-boot menu. Nothing "
            "was unlocked.\n");
        finish(0, 1);
        break;
    case STICK_NOT_FOUND:
    default:
        say("ERROR: no ISO library and LUKS shelf carry this stick's install "
            "ID.\n       Nothing was unlocked.\n");
        say_block_devices();
        finish(0, 1);
        break;
    }
    say("  p2 (ISO library): %s\n  p3 (UKI shelf):   %s\n\n",
        parts.p2, parts.p3);

    /* ---- the survey ----
     *
     * p2 and nothing else. p3 is left alone until the survey has found
     * something to sign. */
    /* exFAT, or FAT32 (DESIGN Section 3): whichever the kernel takes. */
    if (mount(parts.p2, P2_MNT, "exfat", 0, NULL) != 0 &&
        mount(parts.p2, P2_MNT, "vfat", 0, NULL) != 0) {
        say("ERROR: mounting p2: %s\n", strerror(errno));
        finish(0, 1);
    }
    p2_mounted = 1;

    load_manifest();
    listed = bless_list_survey();
    if (!listed) {
        say("surveying p2...\n");
        walk(P2_MNT, "", 0);
    }

    /* Path order once, here, so the numbers the user types, the order the
     * review prints and the order the batch runs in are all the same one. */
    qsort(g_cand, g_cand_n, sizeof(g_cand[0]), cand_cmp);

    say("survey: %zu to sign, %zu already blessed, %zu Windows, "
        "%zu unreadable\n", g_cand_n, g_done_blessed, g_windows,
        g_unreadable);

    if (g_cand_n == 0)
        say("\nnothing to sign; p3 stays locked. FALCON-APPLIANCE-PENDING 0\n");
    else
        bless_half(&parts, listed, &signed_n, &failed_n, &skipped_n);

    say("\nsummary: FALCON-APPLIANCE-SUMMARY signed=%zu failed=%zu "
        "skipped=%zu\n", signed_n, failed_n, skipped_n);

    finish(p2_mounted, !listed || failed_n != 0 || skipped_n != 0);
    return 0;   /* not reached; finish() reboots */
}
