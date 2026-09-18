/* falcon-boot: boot predictions (DESIGN Section 7.8).
 *
 * Given what the platform trusts (db, dbx, MokListRT, MokListXRT, SbatLevelRT)
 * and what an image carries (its certificate table, its Authenticode hash, its
 * .sbat section), say whether shim is expected to accept the image, and in
 * words short enough for a menu row.
 *
 * A PREDICTION, NEVER AN INPUT TO A BOOT. The result of this module is read by
 * the menu's label builder and details screen and by nothing else; no boot
 * path takes a trust_result, and none may be changed to (DESIGN 7.8, "Why this
 * does not cross Section 7.3"). Every entry stays selectable, and selecting
 * one hands it to shim exactly as before. A wrong prediction in either
 * direction changes what the screen said before shim answered, and nothing
 * else.
 *
 * Structural, not cryptographic. No signature is checked and the signed digest
 * is not compared: see authenticode.h and x509.h for exactly what is compared.
 *
 * Yellow only on a positive finding. Anything that does not parse, and any
 * store that cannot be read, produces TRUST_NO_VERDICT, never a refusal.
 *
 * Pure and EFI-free: the stores and the image arrive as byte spans, so the
 * whole decision is table-tested on the host.
 */

#ifndef FALCON_SECBOOT_TRUST_H
#define FALCON_SECBOOT_TRUST_H

#include <trust/der.h>

#define TRUST_NAME_MAX  32u     /* a CN, cut */
#define TRUST_SIGS_MAX  4u      /* signatures reported on the details screen */
#define TRUST_CHAIN_MAX 8u      /* DESIGN 7.8's walk bound */

typedef struct {
    der_span db, dbx;           /* EFI_SIGNATURE_LIST sequences */
    der_span mok, mokx;         /* MokListRT, MokListXRT */
    der_span sbat_level;        /* SbatLevelRT text */
    der_span own_mok;           /* this stick's MOK certificate, DER; may be empty */
    /* Set when an allow list (db, MokListRT) exists but could not be read.
     * An absence of trust is then not a finding, so UNTRUSTED and UNSIGNED
     * become TRUST_NO_VERDICT; OK and REVOKED are positive findings and
     * stand. */
    uint8_t  incomplete;
} trust_stores;

typedef struct {
    der_span       certs;       /* the certificate table; empty when unsigned */
    const uint8_t *hash;        /* Authenticode SHA-256, or NULL when not computed */
    der_span       sbat;        /* the .sbat section; empty when absent */
} trust_image;

typedef enum {
    TRUST_NO_VERDICT = 0,       /* nothing certain enough to say */
    TRUST_OK,                   /* expected to load */
    TRUST_UNTRUSTED,            /* signed, and nothing enrolled reaches it */
    TRUST_UNSIGNED,             /* no certificate table, no hash enrolled */
    TRUST_REVOKED               /* dbx, MokListXRT or SBAT refuses it */
} trust_verdict;

typedef enum {
    TRUST_LIST_NONE = 0,
    TRUST_LIST_DB,
    TRUST_LIST_MOK,
    TRUST_LIST_DBX,
    TRUST_LIST_MOKX,
    TRUST_LIST_SBAT
} trust_list;

/* One signature, as the details screen shows it. */
typedef struct {
    char       signer[TRUST_NAME_MAX];  /* the signing certificate's CN */
    char       top[TRUST_NAME_MAX];     /* the anchor it reached, or the CN of
                                           the issuer the chain ends at */
    trust_list list;                    /* where that anchor is, or NONE */
    uint8_t    parsed;                  /* 0: the blob did not parse */
} trust_sig;

typedef struct {
    trust_verdict verdict;
    trust_list    by;           /* the list that decided an OK or a REVOKED */
    uint8_t       by_hash;      /* ...through the image hash, not a certificate */
    uint8_t       own_mok;      /* OK through this stick's own MOK */
    char          label[TRUST_NAME_MAX];   /* the bracket: who signed it */
    uint8_t       nsigs;        /* signatures in the table, capped */
    trust_sig     sig[TRUST_SIGS_MAX];
} trust_result;

void trust_evaluate(const trust_stores *st, const trust_image *im,
                    trust_result *out);

/* What the banner names (DESIGN 7.8, "Machine state"). */
typedef struct {
    uint8_t  mok;               /* own_mok is in MokListRT (or db) */
    uint8_t  ms_uefi;           /* a Microsoft third-party UEFI CA is in db */
    uint8_t  ms_windows;        /* a Microsoft Windows CA is in db */
    uint16_t other;             /* every other X.509 anchor in db or MokListRT */
} trust_summary;

void trust_summarize(const trust_stores *st, trust_summary *out);

#endif /* FALCON_SECBOOT_TRUST_H */
