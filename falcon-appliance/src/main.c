/* falcon-boot: the on-stick signing appliance's PID 1 (DESIGN Sections 7.5
 * and 6.3.1, build order step 8).
 *
 * The appliance is a signed UKI that boots under full Secure Boot, surveys the
 * ISO library on p2 and does two different things with the two halves of it,
 * both through the SAME `falcon-boot-sign` implementation the host route runs
 * (here the static link, /usr/bin/falcon-boot-sign-static). Then it reboots.
 * That is the whole program; there is no shell in this initramfs and nothing
 * else to run.
 *
 *   Windows media is PREPARED (DESIGN Sections 6.3 and 6.3.1). The whole
 *   batch runs in one boot, because the reboot is the expensive part and
 *   batching is what makes dropping three images cost one of them. The batch
 *   is still released by one mark-review-confirm: DESIGN Section 7.7 makes
 *   patching a bless operation and sends it through the same gate as signing.
 *   Preparation signs nothing and holds no key.
 *
 *   Everything else is BLESSED (DESIGN Section 7.5). p3 is unlocked, the MOK
 *   private key that lives there is read, and the user marks the unsigned
 *   ISOs one by one and then confirms the marked set as a whole.
 *
 *   A bare EFI payload (a .efi, or a .img whose payload is one) is blessed
 *   too, and it is the one bless that WRITES TO THE USER'S OWN FILE: the
 *   payload boots as itself, so there is no wrap to put the signature in
 *   instead (DESIGN Section 7.2). The review therefore names every file that
 *   will be written to, inside a .img as well as outside one, in the sign
 *   tool's own words, before the yes that releases it.
 *
 * Each half asks its own gate, immediately before the work that gate releases,
 * so a mixed run proposes two operations in the order they will happen and the
 * user can accept either without accepting the other. One combined review
 * would put a minutes-long preparation batch between the signing "yes" and the
 * signing, which is the opposite of Section 7.7's "at the moment of
 * confirmation".
 *
 * The two halves are ordered prepare-then-bless, and that ordering is load
 * bearing rather than arbitrary: p3 is opened only if the bless half has
 * something to do. An auto-prep boot (the loader launched this image because
 * the user asked for a Windows ISO to be prepared) therefore decrypts no key
 * and asks for no passphrase at all, and preparation works unchanged on a
 * --p3-plain stick, which is what falcon-cli/src/sign/prepare.h promises.
 *
 * One instruction comes from the loader, and only one: the bless list
 * (list.c). When the user blesses from the loader's menu, the paths they
 * confirmed there arrive as a credential beside the passphrase, and this
 * program signs exactly those, with no UI of its own, and reboots (DESIGN
 * Section 7.5, revised 2026-09-18). Without a list it is the interactive
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
 *   - It never signs anything itself and never touches the key bytes. It
 *     hands `sign` two PATHS under /mnt/p3 and `sign` hands them to sbsign.
 *     The key is read from the LUKS volume by the process that needs it and
 *     is never copied anywhere, least of all onto p2 (DESIGN Section 7.5).
 *   - It never unlocks p3 for the preparation half. Preparation needs no
 *     secret, so giving it one would widen the window the key is decrypted in
 *     for nothing, and the smallness of that window is the reason the
 *     appliance exists rather than a full OS (DESIGN Section 7.5).
 *   - The passphrase never appears in argv (it goes to cryptsetup on a pipe),
 *     never reaches a file, and is wiped from this process's memory as soon
 *     as the unlock returns. Core dumps are disabled at entry for the same
 *     reason.
 *   - p3 is unlocked and mounted READ-ONLY. Nothing in the signing flow writes
 *     to the shelf: the UKI wrap and the manifest record both land on p2. A
 *     read-only mapping means a bug in anything downstream cannot damage the
 *     one copy of the key.
 *   - Nothing is signed OR patched that the user did not mark by hand and then
 *     confirm. The per-image mark plus the typed "yes" over the reviewed list
 *     together ARE the "I vouch for these" act the design rests on: they are
 *     what stops an ISO someone dropped on the stick from being blessed
 *     because it happened to be sitting there. Neither list is ever
 *     pre-marked, and each confirmation prints every path it is about to act
 *     on, so nothing can be swept in unseen (DESIGN Section 7.5,
 *     select-then-sign; Section 7.7, the same gate for patched media).
 *   - There is no network in this initramfs (no interface is brought up, and
 *     nothing here would use one), so the decrypted-key window cannot be
 *     reached from outside the machine.
 *
 * The partitions are identified out of the GPT through
 * falcon-core/src/part/part.c, the same parser AND the same matcher
 * (part_match_role) the loader uses, so the appliance and the loader cannot
 * disagree about which partition is which. The manifest is read through
 * falcon-core/src/manifest/manifest.c, the same parser the loader will use, so
 * "already blessed" means here exactly what it will mean at the menu. Each ISO
 * is classified through falcon-cli/src/sign/isoprobe.c and
 * falcon-core/src/media/family.c, which is the reader and the table
 * `falcon-boot-sign` itself uses, so the survey cannot put an image in a queue
 * the tool will then refuse it from. The set of file names the survey looks at
 * is the LOADER's set (falcon-efi/src/efi /main.c: *.iso, *.img, *.efi),
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
    (void)mkdir(P3_MNT, 0755);

    (void)mount("proc", "/proc", "proc", 0, NULL);
    (void)mount("sysfs", "/sys", "sysfs", 0, NULL);
    /* The work area for kernel extraction and the unsigned wrap. RAM only:
     * nothing a signing run produces before the signature exists should be
     * able to outlive the run on persistent storage. */
    (void)mount("tmpfs", "/tmp", "tmpfs", 0, "mode=1777");
    /* cryptsetup keeps its device locks here and warns loudly without it. */
    (void)mkdir("/run/cryptsetup", 0700);
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

/* Unwound in the order that can actually succeed: p2 first (nothing else
 * depends on it), then p3's mount, then the mapping that carried it. A
 * failure here is reported and pressed on with, because the next step is a
 * reboot either way and leaving the user staring at a half-closed stick
 * helps nobody. */
static void teardown(int p2_mounted, int p3_mounted, int mapped)
{
    if (p2_mounted && umount(P2_MNT) != 0)
        say("note: could not unmount p2: %s\n", strerror(errno));
    if (p3_mounted && umount(P3_MNT) != 0)
        say("note: could not unmount p3: %s\n", strerror(errno));
    if (mapped) {
        char *argv[] = { (char *)"/usr/bin/cryptsetup", (char *)"close",
                         (char *)MAPPER, NULL };

        if (run(argv, NULL, 0, 0, NULL) != 0)
            say("note: could not close the p3 mapping\n");
    }
    sync();
}

static void finish(int p2_mounted, int p3_mounted, int mapped, int wait)
{
    char line[8];

    /* A run the loader asked for that went as asked has nothing left to
     * read: the reboot is the next step the user is waiting for. */
    if (!wait) {
        say("\nrebooting to the loader.\n");
        teardown(p2_mounted, p3_mounted, mapped);
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
    teardown(p2_mounted, p3_mounted, mapped);
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
    int mapped = 0;
    int p3_mounted = 0;
    int p2_mounted = 0;
    int listed = 0;
    size_t prepared_n = 0;
    size_t prep_failed_n = 0;
    size_t prep_skipped_n = 0;
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
        "Prepares Windows install media so Setup can find its own ISO again,\n"
        "and signs everything else on p2 - ISOs, and the bare EFI payloads\n"
        "beside them - with the MOK private key encrypted on p3.\n\n");

    /* The sign tool's ISO reader is linked in here for the survey. Its own
     * diagnostics are about listing a file, which is this program's plumbing
     * rather than its output, so only warnings and errors reach the console.
     * The tool's INFO-level progress during a real prepare or bless still
     * does: that runs in a child process with its own threshold. */
    sgn_log_set_level(SGN_LOG_WARN);

    load_modules();

    if (!find_stick(&parts)) {
        say("ERROR: no falcon stick found (need an exFAT p2 and a LUKS p3 on\n"
            "       one disk). Nothing was unlocked.\n");
        say_block_devices();
        finish(0, 0, 0, 1);
    }
    say("  p2 (ISO library): %s\n  p3 (UKI shelf):   %s\n\n",
        parts.p2, parts.p3);

    /* ---- the survey ----
     *
     * p2 and nothing else. Every input the survey and the preparation half
     * read lives there, so p3 is left alone until the bless half below has
     * been shown to have work: an auto-prep boot (DESIGN Section 6.3.1) then
     * decrypts no key and asks for no passphrase at all. */
    if (mount(parts.p2, P2_MNT, "exfat", 0, NULL) != 0) {
        say("ERROR: mounting p2: %s\n", strerror(errno));
        finish(0, 0, 0, 1);
    }
    p2_mounted = 1;

    (void)prep_tools_present();

    load_manifest();
    listed = bless_list_survey();
    if (!listed) {
        say("surveying p2...\n");
        walk(P2_MNT, "", 0);
    } else if (g_prep_n > 0) {
        /* The menu never lists Windows media for blessing; one that arrives
         * anyway is not prepared behind the user's back. */
        say("note: %zu listed image(s) are Windows media, which is prepared "
            "rather than\n      blessed; select it in the menu to prepare "
            "it.\n", g_prep_n);
        g_prep_n = 0;
    }

    /* Path order once, here, so the numbers the user types, the order the
     * review prints and the order either batch runs in are all the same one. */
    qsort(g_cand, g_cand_n, sizeof(g_cand[0]), cand_cmp);
    qsort(g_prep, g_prep_n, sizeof(g_prep[0]), cand_cmp);

    say("survey: %zu to prepare, %zu to sign, %zu already prepared,\n"
        "        %zu already blessed, %zu unreadable\n",
        g_prep_n, g_cand_n, g_done_prepared, g_done_blessed, g_unreadable);

    /* A mixed run proposes two different operations, so it says so once,
     * before the first gate, rather than leaving the user to work out why
     * they are being asked twice. */
    if (g_prep_n > 0 && g_cand_n > 0)
        say("\nTwo kinds of work are waiting, and each is asked about on its\n"
            "own: %zu Windows image%s to prepare (patched, never signed, no\n"
            "key) and %zu image%s to sign with the MOK key on p3. Saying yes\n"
            "to one is not saying yes to the other.\n",
            g_prep_n, (g_prep_n == 1u) ? "" : "s",
            g_cand_n, (g_cand_n == 1u) ? "" : "s");

    /* Preparation first, and while p3 is still locked. */
    if (g_prep_n > 0)
        prepare_half(&prepared_n, &prep_failed_n, &prep_skipped_n);

    if (g_cand_n == 0)
        say("\nnothing to sign: nothing on p2 is waiting for the MOK key, so\n"
            "p3 stays locked. FALCON-APPLIANCE-PENDING 0\n");
    else
        bless_half(&parts, listed, &mapped, &p3_mounted, &signed_n, &failed_n,
                   &skipped_n);

    say("\nsummary: FALCON-APPLIANCE-SUMMARY prepared=%zu prep-failed=%zu "
        "prep-skipped=%zu signed=%zu failed=%zu skipped=%zu\n",
        prepared_n, prep_failed_n, prep_skipped_n, signed_n, failed_n,
        skipped_n);

    finish(p2_mounted, p3_mounted, mapped,
           !listed || failed_n != 0 || skipped_n != 0 || prep_failed_n != 0);
    return 0;   /* not reached; finish() reboots */
}
