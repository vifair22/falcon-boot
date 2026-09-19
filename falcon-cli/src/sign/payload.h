/* falcon-boot-sign: blessing a bare EFI payload (docs/DESIGN.md Section 7.2).
 *
 * The one bless that WRITES TO THE USER'S FILE, and the reason is mechanical
 * rather than a change of policy. A bare payload boots as itself, so the image
 * the platform verifies IS the file on p2; there is no wrap to build beside
 * it. Signing a PE appends a certificate, so the payload grows. The tool
 * therefore says what it is about to do, names the file, and does it.
 *
 * Two shapes, decided by the extension:
 *
 *   .efi   the file itself is the PE; it is signed in place.
 *   .img   the PE lives in a FAT filesystem inside the image, under
 *          /EFI/BOOT/BOOTX64.EFI, and that is what is signed. An image may
 *          carry more than one such volume (PassMark's MemTest86 image
 *          carries two, a basic-data partition and an ESP, with identical
 *          loaders), and the firmware picks whichever its partition driver
 *          surfaces first, so every one of them is signed rather than a guess
 *          at which will win.
 *
 * No manifest record is written for either, and that is a decision rather than
 * an omission; see sgn_payload_bless.
 */

#ifndef FALCON_CLI_SIGN_PAYLOAD_H
#define FALCON_CLI_SIGN_PAYLOAD_H

#include <stdint.h>
#include <stddef.h>

#include <media/family.h>

/* What the extension says a file is, and nothing more: this is the same kind
 * of parsing hint the loader's own scan takes from an extension (DESIGN
 * Section 6 stage 2), never a claim about the contents. The contents are
 * checked by whoever acts on them. */
typedef enum {
    SGN_PAYLOAD_NONE = 0,  /* not a bare payload; .iso takes the wrap path */
    SGN_PAYLOAD_EFI  = 1,  /* .efi */
    SGN_PAYLOAD_IMG  = 2   /* .img */
} sgn_payload_shape;

/* Extension of the path's last component, ASCII case-insensitive. */
sgn_payload_shape sgn_payload_shape_of(const char *path);

typedef struct {
    const char *path;     /* the .efi or .img to bless */
    const char *key;      /* MOK private key, PEM */
    const char *cert;     /* MOK certificate, PEM */
    const char *work;     /* scratch directory for the mount point */
    int         dry_run;  /* say everything, write nothing */
} sgn_payload_opts;

/* Blesses a bare payload in place.
 *
 * Before anything is opened for writing this prints, to stdout, what is about
 * to happen to which file, because it is the one operation in this tool that
 * changes media the user did not produce (Section 7.2 and the asymmetry
 * Section 7.7 draws out). It does not prompt: this tool is the non-interactive
 * engine both the host and the on-stick appliance drive, and the per-item "I
 * vouch for this" confirmation of Section 7.5 belongs to the appliance. Under
 * dry_run it prints and stops.
 *
 * Signing discipline: ADD our signature, never replace what is there. The
 * payload is somebody else's binary and its existing signature is what makes
 * it boot on machines that have never heard of this stick - memtest's loader
 * ships signed by the Microsoft UEFI CA - so stripping that to add ours would
 * trade "boots almost everywhere" for "boots here", silently, on a file the
 * user did not write. This is the mirror image of the rule for artifacts we
 * produce ourselves (the UKI wraps, and the user's own sign-uki script), where
 * stripping first is right precisely because the old signature is a previous
 * version of ours.
 *
 * Nothing stacks, because a payload that already carries this certificate is
 * reported and SKIPPED rather than signed again. The result is checked three
 * ways before it replaces the original - one more signature than before, ours
 * among them, and the bytes that were there still there - and the replacement
 * is a rename within the same directory, so a failure anywhere leaves the
 * original payload exactly as it was.
 *
 * NO MANIFEST RECORD is written, for either shape. A record exists to point
 * the loader at a SECOND artifact (the signed wrap of a blessed ISO) and to
 * say whether that artifact still matches the file beside it. A payload has
 * no second artifact: the file is the artifact, the loader boots it whether
 * or not anything names it, and the one fact a record could carry - that it
 * is signed - the loader already reads out of the file's own PE headers when
 * it labels the entry. A record would be a second, editable, and therefore
 * disagreeable source of truth about a fact the artifact carries itself.
 *
 * A refusal to write anything is SGN_ERR_*; SGN_OK covers both "signed it" and
 * "it was already signed with this key", which are distinguished on stdout. */
int sgn_payload_bless(const sgn_payload_opts *o);

/* ---- the decisions above, exposed so they can be exercised ----
 *
 * Not a second entry point for anything: these are the three questions the
 * bless asks whose wrong answer is silent, and a rule that can only be checked
 * by signing a real image on real media is a rule nobody checks.
 */

/* Why an image of this family must NOT be blessed as a bare payload, or NULL
 * when it may be.
 *
 * The permissive answer is for the two families that genuinely end at the
 * payload: memtest, which runs no second stage, and an image nothing
 * recognised, where the user has looked at it and this tool has not. Everything
 * else runs something after the payload that this would leave unsigned, and
 * signing its loader would report success and still be refused at boot - the
 * failure a user cannot diagnose from what the tool said. */
const char *sgn_payload_refusal(family_id fam);

/* The DER bytes of the first certificate in a PEM file. Caller frees *der.
 *
 * Base64 between the two markers and nothing else: this is not an X.509
 * parser and does not need to be, because the only use of the result is to
 * look for it verbatim inside another file. */
int sgn_payload_cert_der(const char *pem_path, uint8_t **der, size_t *len);

/* Does this PE's certificate table carry that exact certificate?
 *
 * This is the "already signed with your key" test, and it is NOT
 * `sbverify --cert`: sbverify 0.9.5 answers OK for PassMark's
 * Microsoft-signed MemTest86 loader against an unrelated certificate
 * (measured 2026-09-13, on the very binary this path exists for), which would
 * make the check skip the one file the user asked to have signed. Anything
 * unreadable answers no, because "sign it" is the right response to a file
 * this cannot make sense of. */
int sgn_payload_signed_with(const char *path, const uint8_t *der,
                            size_t der_len);

/* The PE's certificate table (data directory 4), malloc'd; caller frees.
 * SGN_OK with *out NULL means the image is unsigned, which is an answer and
 * not an error. This is what lets the bless keep the table it found before
 * signing and confirm afterwards that the vendor's signatures are still in
 * the one it produced: a check that only asks "does our certificate verify"
 * would pass a strip-and-replace and miss exactly that regression. */
int sgn_payload_cert_table(const char *path, uint8_t **out, size_t *len);

#endif /* FALCON_CLI_SIGN_PAYLOAD_H */
