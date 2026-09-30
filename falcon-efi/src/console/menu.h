/* falcon-boot: text menu over Simple Text Output/Input (DESIGN Section 9).
 * Presentation only: it renders entries and reports what the user picked;
 * acting on the selection is the caller's job.
 */

#ifndef FALCON_MENU_H
#define FALCON_MENU_H

#include <efi/uefi.h>

#define MENU_PATH_MAX 512u
#define MENU_TAG_MAX  48u

/* A row: its name, and two optional tag columns drawn right of it in aligned
 * columns (DESIGN Section 7.8). `sig` says who signed what the entry boots;
 * `why` says why the platform is expected to refuse it, and is set only
 * together with `caution`, which draws the row in the caution colours. The
 * menu renders them and knows nothing of what they mean. `probed` is the
 * menu's, for menu_run_probed: it sets it once the probe has filled the row,
 * and a caller building rows for that call clears it. */
typedef struct {
    CHAR16 label[MENU_PATH_MAX];
    CHAR16 sig[MENU_TAG_MAX];
    CHAR16 why[MENU_TAG_MAX];
    UINT8  caution;
    UINT8  probed;
} menu_entry;

/* The banner is two segments so the Secure Boot state can be coloured apart
 * from the title (DESIGN Section 7.4 wants a relaxed-enforcement state to be
 * unmissable). The menu stays presentation-only: it renders the attribute it
 * is handed and has no idea what the state means, which keeps the module
 * free of any dependency on the sb module and, more to the point, free of
 * anything that could grow into an enforcement decision. */
typedef struct {
    const CHAR16 *title;
    const CHAR16 *state;      /* NULL to render the title alone */
    UINTN state_attr;         /* EFI_TEXT_ATTR value for the state segment */
} menu_banner;

/* Picks a console text mode. Called once, before anything is drawn.
 *
 * Separate from menu_run because SetMode clears the screen, and the loader
 * shows several screens per run. Safe to skip: the console keeps whatever the
 * firmware handed it. */
void menu_init(EFI_SYSTEM_TABLE *st);

/* Every screen the loader draws, not only the menu, sits in one content
 * block. These are how a caller outside this module gets it.
 *
 * menu_screen clears and parks the cursor at the block's first text column;
 * menu_out writes inside it, re-indenting after each line break and wrapping
 * at the right edge. A caller writes text and gets the layout, rather than
 * knowing the layout and computing offsets. Before these existed the menu was
 * the only screen with a frame and every prompt, confirmation and error drew
 * from column zero beside it. */
void menu_screen(EFI_SYSTEM_TABLE *st);
void menu_out(EFI_SYSTEM_TABLE *st, const CHAR16 *s);

/* The route a boot is taking, as a short code (docs/Manual.md, "Route
 * codes"), drawn in the block's top right corner now and by every
 * menu_screen after it, so a module that clears the screen does not lose it.
 * code must outlive the boot (a literal). Null stops drawing it. */
void menu_mode(EFI_SYSTEM_TABLE *st, const CHAR16 *code);

/* "<step>..." on the page's first row, replacing whatever step was there
 * (falcon-core's tui_busy_*): what the loader is doing while no menu is up.
 * Call menu_screen first. */
void menu_busy(EFI_SYSTEM_TABLE *st, const CHAR16 *step);

/* Blocks until the user commits. Returns the selected entry index, or a
 * negative value if the user backed out (ESC). */
INTN menu_run(EFI_SYSTEM_TABLE *st, const menu_banner *banner,
              const menu_entry *entries, UINTN count);

/* Keys beyond enter that also commit a selection, for a caller with more
 * than one way to act on an entry: `keys` lists them in lower case (either
 * case is accepted), `hint` replaces the key line under the list. */
typedef struct {
    const CHAR16 *keys;
    const CHAR16 *hint;
} menu_keys;

/* menu_run, with keys: *which receives the key that committed, L'\r' for
 * enter or one of keys->keys in lower case. keys and which may be NULL. */
INTN menu_run_keys(EFI_SYSTEM_TABLE *st, const menu_banner *banner,
                   const menu_entry *entries, UINTN count,
                   const menu_keys *keys, CHAR16 *which);

/* Lines under the banner that the caller keeps current: the UEFI station's
 * TOTP code (DESIGN Section 13.15). The menu asks for them when it opens
 * and again every second while it waits, and repaints the lines whose text
 * or colour changed; how many there are is fixed by the first answer (at
 * most MENU_STATUS_ROWS, and the list gives up rows for them). The menu
 * knows nothing of what they say. */
#define MENU_STATUS_ROWS 2u
#define MENU_STATUS_MAX  96u

typedef struct {
    CHAR16 line[MENU_STATUS_ROWS][MENU_STATUS_MAX];
    UINTN  attr[MENU_STATUS_ROWS];
    UINTN  n;
} menu_status_text;

typedef void (*menu_status_fn)(void *ctx, menu_status_text *out);

typedef struct {
    menu_status_fn fn;
    void          *ctx;
} menu_status;

/* menu_run_keys with `sel` highlighted first and, when `seconds` is nonzero,
 * a countdown in the key line: when it runs out, `sel` is returned as if
 * enter had been pressed (*which = L'\r'), and any key stops it for the
 * rest of the menu's life. The station's STATION.INI default and timeout
 * (DESIGN Section 13.7); falcon-bios's screen_menu_timed is the same menu,
 * line for line. A firmware that will not make a timer gets no countdown:
 * the menu then waits, which is what a menu with no timeout does anyway.
 * status, when given, is drawn under the banner and refreshed on the same
 * one-second timer; the countdown's end is unchanged by it. */
INTN menu_run_timed(EFI_SYSTEM_TABLE *st, const menu_banner *banner,
                    const menu_entry *entries, UINTN count,
                    const menu_keys *keys, CHAR16 *which, UINTN sel,
                    UINTN seconds, const menu_status *status);

/* The menu's background work (DESIGN Section 7.8): the caller's probe,
 * which lives on a task of its own (task/task.h) and so outlives the menu
 * call. step runs a slice of it, until the probe next yields, with `sel` the
 * row highlighted; it returns nonzero while there is more to do. fill
 * copies what is known of row idx's tags onto it and returns nonzero once
 * they are final. */
typedef struct {
    int  (*step)(void *ctx, UINTN sel);
    int  (*fill)(void *ctx, UINTN idx, menu_entry *e);
    void *ctx;
} menu_probe;

/* menu_run_keys with the rows' tags filled in the background. The page is
 * painted at once, a row whose tags are not known yet showing
 * tui_tag_pending. Whenever no key is waiting the menu steps the probe,
 * then fills every pending row on screen and repaints the ones that became
 * final (the whole window when a tag column widens); a spinner in the
 * banner's last cell turns while step says there is more. Keys, the idle
 * time and the rest are as menu_run_keys. A row the caller built with
 * probed set is never pending. The tag columns are sized to the window's
 * widest tags. probe may be NULL, which is menu_run_keys, with the columns
 * still sized per window. */
INTN menu_run_probed(EFI_SYSTEM_TABLE *st, const menu_banner *banner,
                     menu_entry *entries, UINTN count, const menu_keys *keys,
                     CHAR16 *which, const menu_probe *probe);

/* Masked line editor, for the p3 passphrase (DESIGN Section 5 step 2).
 *
 * Lives in the menu module because it is console presentation and nothing
 * else: it knows how to echo a '*' and how to take a backspace, and it has no
 * idea what the bytes are for. What they are for, and the wiping discipline
 * that follows from it, is the caller's (falcon-efi/src/pos).
 *
 * The result is BYTES, not CHAR16: a LUKS passphrase is a byte string, and
 * widening it to UTF-16 here only to narrow it again at the LUKS call would
 * put a second copy of the secret in a second buffer. Input is restricted to
 * printable ASCII for exactly the same reason it is on the mkstick side:
 * anything else has no single agreed byte encoding, so a passphrase typed at
 * this prompt and one typed at a Linux prompt would silently differ.
 * Non-ASCII keystrokes are ignored rather than rejected, because a refusal
 * message would be a keystroke-by-keystroke oracle for nothing.
 *
 * Tab follows the systemd ask-password convention: it erases the asterisks
 * already on screen and silences echo for the rest of the entry, so the
 * passphrase's length is not on display. Editing keeps working unchanged.
 *
 * Returns the number of bytes typed (0 is a legal answer, and the LUKS layer
 * will reject it like any wrong passphrase), or a negative value if the user
 * pressed ESC. On ESC the buffer is wiped before returning; on success the
 * caller owns the wipe. buf always ends NUL-terminated within cap. */
INTN menu_prompt_secret(EFI_SYSTEM_TABLE *st, const CHAR16 *prompt,
                        char *buf, UINTN cap);

/* Called once a second while menu_prompt_secret waits: the station's TOTP
 * lines above the prompt, redrawn when the code rolls. NULL for none. */
void menu_prompt_set_tick(void (*fn)(void));

#endif /* FALCON_MENU_H */
