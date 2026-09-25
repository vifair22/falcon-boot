/*
 * winjump - the WinPE half of the Windows install-media route (DESIGN
 * Section 6.3).
 *
 * A stock Windows ISO booted from our virtual CD reaches WinPE fine and then
 * loses its media at ExitBootServices, so Setup cannot find
 * \sources\install.wim. The fix is to make Windows mount the ISO for itself
 * once it is up, from Windows userland, using the inbox VirtDisk API. There
 * is no inbox CLI for that (diskpart is VHD/VHDX only, Mount-DiskImage is
 * PowerShell and WinPE has none), which is why this executable exists.
 *
 * The loader replaces \Windows\System32\winpeshl.exe inside the ISO's
 * boot.wim with this binary as it serves the ISO (falcon-efi/src/win), keeps
 * the original as winpeshl_orig.exe, and puts falcon-winjump.cfg beside
 * both. At run time we attach the ISO and
 * then run the original shell, so Setup proceeds exactly as it would have.
 *
 * Two constraints are Microsoft's, not ours:
 * ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY is mandatory for an ISO (the docs say
 * ATTACH_VIRTUAL_DISK_FLAG_NONE is unsupported for one), and
 * ATTACH_VIRTUAL_DISK_FLAG_PERMANENT_LIFETIME is the only thing that keeps
 * the mount alive once this process exits.
 *
 * Every failure path still launches the original shell. A machine that boots
 * to a media-less WinPE can be diagnosed; one whose shell never started
 * reboots on the spot and tells nobody anything.
 *
 * NO C RUNTIME. A missing import stops a process before its first
 * instruction, so this binary imports only what every WinPE from Vista's on
 * carries: kernel32, advapi32 and user32. The C runtime is not one of those:
 * a mingw built for the Universal CRT (Arch's) imports api-ms-win-crt-*,
 * which WinPE gained with Windows 10, and one built for msvcrt (Debian's)
 * imports msvcrt.dll, so the same source used to give different binaries on
 * different build hosts, and the laptop's could not start on Windows 8.1.
 * The handful of string routines are here instead, the log's formatting is
 * user32's wvsprintf (in every WinPE, which is a GUI; winpeshl itself
 * imports it), and the entry point is ours. So the imports are kernel32,
 * advapi32 and user32. VirtDisk.dll, which Vista's WinPE
 * does not have at all, is loaded at run time for the same reason: without
 * it there is no attach, but there is still a shell.
 */

/* AttachVirtualDisk's ISO support arrived in Windows 8; ask for the modern
 * header view so the version-2 parameter structures are visible too. This
 * only gates declarations, not the imports we actually bind. */
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
/* Every call here is the explicit -W form; UNICODE is set so the constants
 * that go with them (SE_MANAGE_VOLUME_NAME and friends) widen to match. */
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <virtdisk.h>
#include <winioctl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <media/extmap.h>
#include "winjumpcore.h"
#include "vdiskio.h"

/* ---- what the C runtime would have provided ----
 *
 * memset, memcpy and strlen keep their names because the compiler emits
 * calls to them on its own: zeroing or copying a structure, and recognising
 * a counting loop as strlen. The rest are named for what they are. */
void *memset(void *d, int c, size_t n);
void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;

    while (n-- > 0)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *src, size_t n);
void *memcpy(void *d, const void *src, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = src;

    while (n-- > 0)
        *p++ = *q++;
    return d;
}

size_t strlen(const char *s);
size_t strlen(const char *s)
{
    const char *e = s;

    while (*e != '\0')
        e++;
    return (size_t)(e - s);
}

/* memcmp, for the linked-in falcon-core code (extmap, winjumpcore) that a
 * hosted build would get from libc. */
int memcmp(const void *a, const void *b, size_t n);
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a;
    const unsigned char *q = b;

    for (; n-- > 0; p++, q++) {
        if (*p != *q)
            return (int)*p - (int)*q;
    }
    return 0;
}

/* A number as decimal, into buf, for the log: wvsprintf's only 64-bit
 * conversion is a Microsoft extension the pedantic checker rejects. */
static const char *u64s(char buf[24], unsigned long long v)
{
    char *p = buf + 23;

    *p = '\0';
    do {
        *--p = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v != 0);
    return p;
}

static int jstrcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static char *jstrchr(char *s, char c)
{
    for (; *s != '\0'; s++) {
        if (*s == c)
            return s;
    }
    return NULL;
}

static size_t jwcslen(const wchar_t *s)
{
    size_t n = 0;

    while (s[n] != L'\0')
        n++;
    return n;
}

static int jwcscmp(const wchar_t *a, const wchar_t *b)
{
    while (*a != L'\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)*a - (int)*b;
}

/* Decimal digits to a number; stops at the first non-digit, and saturates
 * rather than wrapping, so a mangled size can only fail to match. */
static unsigned long long jparse_u64(const char *s)
{
    unsigned long long v = 0;

    for (; *s >= '0' && *s <= '9'; s++) {
        unsigned d = (unsigned)(*s - '0');

        if (v > (~0ull - d) / 10u)
            return ~0ull;
        v = v * 10u + d;
    }
    return v;
}

/* dst = a followed by b, both whole or not at all. FALSE when they do not fit,
 * with dst left empty, which is swprintf's contract as this file used it. */
static BOOL jwjoin(wchar_t *dst, size_t cap, const wchar_t *a,
                   const wchar_t *b)
{
    size_t la = jwcslen(a);
    size_t lb = jwcslen(b);
    size_t i;

    if (cap == 0)
        return FALSE;
    dst[0] = L'\0';
    if (la + lb + 1u > cap)
        return FALSE;
    for (i = 0; i < la; i++)
        dst[i] = a[i];
    for (i = 0; i < lb; i++)
        dst[la + i] = b[i];
    dst[la + lb] = L'\0';
    return TRUE;
}

/* ---- the stack protector, without libssp ----
 *
 * -fstack-protector-strong stays on (it is the repository's rule for every
 * hosted build), and it needs a guard value and a function to call when a
 * canary is found broken. libssp provides both and reports through the C
 * runtime, which is exactly what this file does without. The guard is set
 * from values that differ per boot before any protected function runs
 * (mainCRTStartup is built without the protector), and a broken canary ends
 * the process at once: whatever overwrote the stack is not something to go
 * on running with. */
#if defined(__x86_64__)
unsigned long long __stack_chk_guard = 0x2d5f3a1c9e7b4d60ull;
#else
unsigned long __stack_chk_guard = 0x9e7b4d60ul;
#endif
void __stack_chk_fail(void) __attribute__((noreturn));
void __stack_chk_fail(void)
{
    TerminateProcess(GetCurrentProcess(), 0xC0000409u);
    for (;;)
        ;
}

/* WinPE's RAM disk is X: and is writable. The log is the only view into a
 * black box: everything here is inside a RAM disk that evaporates on reboot,
 * so a run that fails silently cannot be debugged at all. */
#define LOG_PATH        L"X:\\falcon-winjump.log"
#define CFG_NAME        L"falcon-winjump.cfg"
#define DEFAULT_SHELL   L"winpeshl_orig.exe"
/* The user's own options, on the stick beside their ISOs rather than in the
 * loader: <root>\falcon\windows.cfg on the volume the ISO was found on. */
#define STICK_CFG       L"falcon\\windows.cfg"
/* Where a chosen unattend file is copied, so Setup still has it after the
 * stick's drive letter moves; Setup copies it into the install itself. */
#define UNATTEND_COPY   L"X:\\falcon-unattend.xml"

struct cfg {
    char iso[MAX_PATH];       /* ISO path relative to a volume root */
    unsigned long long size;  /* its exact byte length, 0 if unspecified */
    char shell[MAX_PATH];     /* original shell, empty for the default */
    uint8_t *map;             /* the ISO's extent map, heap, NULL if none */
    DWORD map_len;            /* its length in bytes */
};

/* p2:\falcon\windows.cfg, every key optional:
 *
 *   win11_bypass=1          Setup skips Windows 11's TPM, Secure Boot, RAM,
 *                           CPU and storage checks (the LabConfig keys)
 *   unattend=<path>         an answer file, relative to the stick's root,
 *                           that Setup runs with
 *
 * Ventoy's BypassNRO is absent on purpose: it has to reach the INSTALLED
 * system's registry, which Ventoy does with a process that outlives Setup and
 * waits for it. An unattend file does the same job without one. */
struct stick_opts {
    int win11_bypass;
    char unattend[MAX_PATH];
};

/* The log's formatter: %s, %ls (wide, non-ASCII shown as '?'), %.Nls, %c,
 * %d, %u, %ld, %lu and %%, which is every conversion this file uses and
 * nothing else. Output stops at cap - 1 bytes and is always terminated.
 * Returns the length written. */
/* user32's formatter: %s, %ls (wide), %c, %u, %lu, %ld and %% are all this
 * file uses, and every one is a conversion wvsprintf takes; the 64-bit
 * values go through u64s and %s. Its checker is ms_printf. wvsprintfA caps at
 * 1024 bytes, which is why line is that size and every argument is a path or
 * a number. */
static void __attribute__((format(ms_printf, 1, 2)))
jlog(const char *fmt, ...)
{
    char line[1024 + 2];
    va_list ap;
    HANDLE h;
    DWORD written;
    int n;

    va_start(ap, fmt);
    n = wvsprintfA(line, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > 1024)
        n = 1024;
    line[n] = '\r';
    line[n + 1] = '\n';

    /* The console as well: WinPE shows this process's window, so the log is
     * on screen as it happens, which is the only view there is when the
     * machine then reboots and takes X: with it. */
    {
        HANDLE con = GetStdHandle(STD_OUTPUT_HANDLE);

        if (con != NULL && con != INVALID_HANDLE_VALUE)
            WriteFile(con, line, (DWORD)n + 2, &written, NULL);
    }

    /* Reopened per line so a hang or a crash still leaves everything written
     * so far on disk. The log is a few dozen lines; the cost is irrelevant. */
    h = CreateFileW(LOG_PATH, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, line, (DWORD)n + 2, &written, NULL);
    CloseHandle(h);
}

/* AttachVirtualDisk fails with ERROR_PRIVILEGE_NOT_HELD without this. WinPE
 * runs us as SYSTEM, so the privilege is present but disabled by default. */
static BOOL enable_manage_volume(void)
{
    TOKEN_PRIVILEGES tp;
    HANDLE tok;
    BOOL ok;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &tok)) {
        jlog("privilege: OpenProcessToken failed, err=%lu", GetLastError());
        return FALSE;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(NULL, SE_MANAGE_VOLUME_NAME,
                               &tp.Privileges[0].Luid)) {
        jlog("privilege: LookupPrivilegeValue failed, err=%lu", GetLastError());
        CloseHandle(tok);
        return FALSE;
    }

    ok = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof tp, NULL, NULL);
    /* AdjustTokenPrivileges reports success even when it adjusted nothing. */
    if (ok && GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
        jlog("privilege: SE_MANAGE_VOLUME not held by this token");
        ok = FALSE;
    } else if (!ok) {
        jlog("privilege: AdjustTokenPrivileges failed, err=%lu", GetLastError());
    }

    CloseHandle(tok);
    return ok;
}

/* Everything we need lives beside this binary in System32, and the volume it
 * lives on is the RAM disk rather than a fixed letter. Deriving the directory
 * from our own module path is what keeps both lookups letter-independent. */
static BOOL module_dir(wchar_t *out, size_t cap)
{
    DWORD n;
    size_t i;

    n = GetModuleFileNameW(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap) {
        jlog("module: GetModuleFileName failed, err=%lu", GetLastError());
        return FALSE;
    }
    for (i = n; i > 0; i--) {
        if (out[i - 1] == L'\\') {
            out[i] = L'\0';
            return TRUE;
        }
    }
    jlog("module: no directory separator in module path");
    return FALSE;
}

static void trim(char *s)
{
    size_t n = strlen(s);

    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' ||
                     s[n - 1] == ' '  || s[n - 1] == '\t'))
        s[--n] = '\0';
}

typedef void (*kv_fn)(void *ctx, const char *key, const char *value);

/* Dead-simple ASCII key=value, one per line, '#' comments. FALSE when the
 * file cannot be read; what is in it is the caller's business. */
static BOOL read_kv(const wchar_t *path, kv_fn fn, void *ctx)
{
    /* Large enough for the cfg with the extent map's hex on one line: the
     * header, then map= and up to EXTMAP_MAX_EXTENTS runs, twice over in hex. */
    static char buf[24576];
    HANDLE h;
    DWORD got = 0;
    char *line;
    char *next;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        jlog("cfg: cannot open %ls, err=%lu", path, GetLastError());
        return FALSE;
    }
    if (!ReadFile(h, buf, sizeof buf - 1, &got, NULL)) {
        jlog("cfg: read of %ls failed, err=%lu", path, GetLastError());
        CloseHandle(h);
        return FALSE;
    }
    CloseHandle(h);
    buf[got] = '\0';

    for (line = buf; line != NULL && *line != '\0'; line = next) {
        char *eq;

        next = jstrchr(line, '\n');
        if (next != NULL)
            *next++ = '\0';
        trim(line);
        if (*line == '\0' || *line == '#')
            continue;

        eq = jstrchr(line, '=');
        if (eq == NULL)
            continue;
        *eq++ = '\0';
        fn(ctx, line, eq);
    }
    return TRUE;
}

/* A hex digit's value, or -1. */
static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Decode the map= hex into a heap buffer on cfg (freed when winjump exits). */
static void cfg_map(struct cfg *cfg, const char *hex)
{
    size_t n = strlen(hex), i;
    uint8_t *out;

    if ((n & 1u) != 0 || n == 0) {
        jlog("cfg: map= has an odd or empty hex length");
        return;
    }
    out = HeapAlloc(GetProcessHeap(), 0, n / 2u);
    if (out == NULL)
        return;
    for (i = 0; i < n; i += 2) {
        int hi = hexval(hex[i]), lo = hexval(hex[i + 1]);

        if (hi < 0 || lo < 0) {
            jlog("cfg: map= has a non-hex digit");
            HeapFree(GetProcessHeap(), 0, out);
            return;
        }
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    cfg->map = out;
    cfg->map_len = (DWORD)(n / 2u);
}

static void cfg_key(void *ctx, const char *key, const char *value)
{
    struct cfg *cfg = ctx;

    if (jstrcmp(key, "iso") == 0)
        lstrcpynA(cfg->iso, value, MAX_PATH);
    else if (jstrcmp(key, "shell") == 0)
        lstrcpynA(cfg->shell, value, MAX_PATH);
    else if (jstrcmp(key, "size") == 0)
        cfg->size = jparse_u64(value);
    else if (jstrcmp(key, "map") == 0)
        cfg_map(cfg, value);
    else
        jlog("cfg: ignoring unknown key '%s'", key);
}

/* falcon-winjump.cfg beside this binary. Written by the loader, read only
 * here; it never has to survive a user editing it. */
static BOOL read_cfg(const wchar_t *dir, struct cfg *cfg)
{
    wchar_t path[MAX_PATH];

    if (!jwjoin(path, MAX_PATH, dir, CFG_NAME))
        return FALSE;
    if (!read_kv(path, cfg_key, cfg))
        return FALSE;

    if (cfg->iso[0] == '\0') {
        jlog("cfg: no iso= key");
        return FALSE;
    }
    {
        char n[24];

        jlog("cfg: iso=%s size=%s shell=%s", cfg->iso, u64s(n, cfg->size),
             cfg->shell[0] != '\0' ? cfg->shell : "(default)");
    }
    return TRUE;
}

/* Volume roots come back as "C:\" strings; the ISO path is relative to one of
 * them, so normalise separators and drop any leading one. */
static BOOL widen_relative(const char *rel, wchar_t *out, int cap)
{
    int i;

    while (*rel == '/' || *rel == '\\')
        rel++;
    if (MultiByteToWideChar(CP_ACP, 0, rel, -1, out, cap) == 0) {
        jlog("path: MultiByteToWideChar failed, err=%lu", GetLastError());
        return FALSE;
    }
    for (i = 0; out[i] != L'\0'; i++) {
        if (out[i] == L'/')
            out[i] = L'\\';
    }
    return TRUE;
}

/* Enough for 26 roots rendered as "C:\(2) ". */
#define SEEN_CAP 256

/*
 * One pass over the volumes Windows reports right now.
 *
 * Honest statement of the ambiguity we accept: the match is "this relative
 * path exists on this volume and is exactly this many bytes long". Two sticks
 * plugged into the same machine, both carrying the same ISO at the same
 * relative path, both match, and we take the first letter Windows reports.
 * Size is the tiebreak because it is free and it catches the realistic case
 * (a different ISO at the same path), not the adversarial one. Ventoy solves
 * this properly by matching the backing physical disk's GUID through
 * IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS; doing the same here needs the bless
 * step to record our p2 disk identity, which it does not yet.
 *
 * seen receives the drive set this pass observed, each root with its drive
 * type. The caller passes back what the previous pass saw, and detail is
 * logged only when the two differ: across a poll that runs dozens of times
 * the interesting event is the storage stack changing, not the dozens of
 * identical passes around it.
 */
static BOOL find_iso_once(const struct cfg *cfg, const wchar_t *rel,
                          wchar_t *out, int cap,
                          const wchar_t *prev_seen, wchar_t *seen)
{
    wchar_t drives[512];
    size_t at = 0;
    DWORD n;
    const wchar_t *root;
    BOOL verbose;

    seen[0] = L'\0';

    n = GetLogicalDriveStringsW(sizeof drives / sizeof drives[0] - 1, drives);
    if (n == 0 || n >= sizeof drives / sizeof drives[0]) {
        lstrcpynW(seen, L"(enumeration failed)", SEEN_CAP);
        if (jwcscmp(prev_seen, seen) != 0)
            jlog("scan: GetLogicalDriveStrings failed, err=%lu", GetLastError());
        return FALSE;
    }

    for (root = drives; *root != L'\0'; root += jwcslen(root) + 1) {
        UINT type = GetDriveTypeW(root);
        size_t i;

        /* "C:\(2) ": a root is three characters, a drive type one digit. */
        if (SEEN_CAP - at < 16)
            break;
        for (i = 0; i < 3u && root[i] != L'\0'; i++)
            seen[at++] = root[i];
        seen[at++] = L'(';
        seen[at++] = (wchar_t)(L'0' + (type % 10u));
        seen[at++] = L')';
        seen[at++] = L' ';
        seen[at] = L'\0';
    }

    verbose = jwcscmp(prev_seen, seen) != 0;
    if (verbose)
        jlog("scan: drives %ls", seen);

    for (root = drives; *root != L'\0'; root += jwcslen(root) + 1) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        unsigned long long size;

        if (!jwjoin(out, (size_t)cap, root, rel))
            continue;
        if (!GetFileAttributesExW(out, GetFileExInfoStandard, &fad)) {
            if (verbose)
                jlog("scan: %ls absent", out);
            continue;
        }

        size = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (cfg->size != 0 && size != cfg->size) {
            if (verbose) {
                char a[24];
                char b[24];

                jlog("scan: %ls is %s bytes, want %s; skipping", out,
                     u64s(a, size), u64s(b, cfg->size));
            }
            continue;
        }
        {
            char a[24];

            jlog("scan: found %ls (%s bytes)", out, u64s(a, size));
        }
        return TRUE;
    }

    return FALSE;
}

/*
 * Deadline, interval and heartbeat for the search.
 *
 * WinPE brings its storage stack up asynchronously, so the exFAT volume can
 * arrive after we do: a USB 2.0 port behind a hub, a large disk still
 * spinning up, or simply more devices to enumerate all push it later. Looking
 * once loses that race silently and totally - no ISO, no attach, and Setup
 * reports a missing media driver - so we poll instead.
 *
 * The budget is wall time, not a pass count, because a pass costs whatever
 * the slowest volume in it costs: a stalled device can make one
 * GetFileAttributesEx block for seconds, and a count would then expire in an
 * unpredictable amount of real time. 30 s is several times the few seconds
 * USB enumeration takes when it is merely slow, and is nothing against a
 * Windows install measured in minutes. It is also bounded on purpose: a
 * media-less WinPE is recoverable and explainable, a shell that never starts
 * is neither.
 *
 * 500 ms between passes gives about 60 passes over the budget and adds at
 * most half a second of latency once the media does appear. A pass is a
 * drive-string enumeration plus one attribute query per volume, so polling at
 * that rate costs nothing measurable.
 */
#define FIND_DEADLINE_MS  30000u
#define FIND_INTERVAL_MS    500u
#define FIND_HEARTBEAT_MS  5000u

static BOOL find_iso(const struct cfg *cfg, wchar_t *out, int cap)
{
    wchar_t rel[MAX_PATH];
    /* Two buffers rather than one: the drive set is re-enumerated every pass,
     * and the previous pass's set has to survive to compare against. */
    wchar_t seen[SEEN_CAP];
    wchar_t prev_seen[SEEN_CAP];
    ULONGLONG start;
    ULONGLONG beat;
    unsigned passes = 0;

    if (!widen_relative(cfg->iso, rel, MAX_PATH))
        return FALSE;

    /* Not a drive set any enumeration can produce, so the first pass always
     * counts as a change and logs what it saw. */
    lstrcpynW(prev_seen, L"(nothing yet)", SEEN_CAP);

    start = GetTickCount64();
    beat = start;
    for (;;) {
        ULONGLONG now;

        passes++;
        if (find_iso_once(cfg, rel, out, cap, prev_seen, seen))
            return TRUE;
        lstrcpynW(prev_seen, seen, SEEN_CAP);

        now = GetTickCount64();
        if (now - start >= FIND_DEADLINE_MS) {
            char a[24];

            jlog("scan: no volume carries %s after %s ms and %u passes",
                 cfg->iso, u64s(a, now - start), passes);
            jlog("scan: last drives seen %ls", seen);
            return FALSE;
        }
        if (now - beat >= FIND_HEARTBEAT_MS) {
            char a[24];

            jlog("scan: still waiting, %s ms elapsed, %u passes, drives %ls",
                 u64s(a, now - start), passes, seen);
            beat = now;
        }
        Sleep(FIND_INTERVAL_MS);
    }
}

/*
 * Single-shot on purpose, unlike the search above. This runs only after a
 * pass found the ISO, which means the volume is mounted and readable, so the
 * asynchronous-storage window the poll exists for is already closed. The
 * failures left are the deterministic ones - privilege not held, flags an ISO
 * does not support, an image the VHD stack will not open - and retrying those
 * only delays the shell launch that is the actual fail-safe.
 */
/* VirtDisk.dll's two calls, looked up at run time: Vista's WinPE has no such
 * DLL, and a static import of it would stop this process before it could
 * start the shell. */
typedef DWORD (WINAPI *open_vd_fn)(PVIRTUAL_STORAGE_TYPE, PCWSTR,
                                   VIRTUAL_DISK_ACCESS_MASK,
                                   OPEN_VIRTUAL_DISK_FLAG,
                                   POPEN_VIRTUAL_DISK_PARAMETERS, PHANDLE);
typedef DWORD (WINAPI *attach_vd_fn)(HANDLE, PSECURITY_DESCRIPTOR,
                                     ATTACH_VIRTUAL_DISK_FLAG, ULONG,
                                     PATTACH_VIRTUAL_DISK_PARAMETERS,
                                     LPOVERLAPPED);

static BOOL attach_iso(const wchar_t *path)
{
    HMODULE vd;
    open_vd_fn open_vd;
    attach_vd_fn attach_vd;
    VIRTUAL_STORAGE_TYPE st;
    OPEN_VIRTUAL_DISK_PARAMETERS op;
    ATTACH_VIRTUAL_DISK_PARAMETERS ap;
    HANDLE disk = INVALID_HANDLE_VALUE;
    DWORD before, after, appeared;
    DWORD rc;

    vd = LoadLibraryW(L"virtdisk.dll");
    if (vd == NULL) {
        jlog("attach: no virtdisk.dll in this WinPE, err=%lu", GetLastError());
        return FALSE;
    }
    /* GetProcAddress hands back a generic pointer; through uintptr_t so the
     * cast is not a function-pointer pun the compiler has to warn about. */
    open_vd = (open_vd_fn)(uintptr_t)GetProcAddress(vd, "OpenVirtualDisk");
    attach_vd = (attach_vd_fn)(uintptr_t)GetProcAddress(vd,
                                                        "AttachVirtualDisk");
    if (open_vd == NULL || attach_vd == NULL) {
        jlog("attach: virtdisk.dll has no OpenVirtualDisk/AttachVirtualDisk");
        return FALSE;
    }

    memset(&st, 0, sizeof st);
    st.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_ISO;
    /* VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT, spelled out rather than pulled
     * in through DEFINE_GUID so nothing depends on a GUID library object. */
    st.VendorId.Data1 = 0xEC984AECu;
    st.VendorId.Data2 = 0xA0F9;
    st.VendorId.Data3 = 0x47E9;
    st.VendorId.Data4[0] = 0x90; st.VendorId.Data4[1] = 0x1F;
    st.VendorId.Data4[2] = 0x71; st.VendorId.Data4[3] = 0x41;
    st.VendorId.Data4[4] = 0x5A; st.VendorId.Data4[5] = 0x66;
    st.VendorId.Data4[6] = 0x34; st.VendorId.Data4[7] = 0x5B;

    memset(&op, 0, sizeof op);
    op.Version = OPEN_VIRTUAL_DISK_VERSION_1;

    rc = open_vd(&st, path, VIRTUAL_DISK_ACCESS_READ,
                         OPEN_VIRTUAL_DISK_FLAG_NONE, &op, &disk);
    if (rc != ERROR_SUCCESS) {
        jlog("attach: OpenVirtualDisk failed, rc=%lu", rc);
        return FALSE;
    }

    before = GetLogicalDrives();

    memset(&ap, 0, sizeof ap);
    ap.Version = ATTACH_VIRTUAL_DISK_VERSION_1;

    /* READ_ONLY is mandatory for an ISO and PERMANENT_LIFETIME is what
     * survives our exit; neither is a preference. */
    rc = attach_vd(disk, NULL,
                           ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY |
                           ATTACH_VIRTUAL_DISK_FLAG_PERMANENT_LIFETIME,
                           0, &ap, NULL);
    if (rc != ERROR_SUCCESS) {
        jlog("attach: AttachVirtualDisk failed, rc=%lu", rc);
        CloseHandle(disk);
        return FALSE;
    }

    after = GetLogicalDrives();
    appeared = after & ~before;
    if (appeared != 0) {
        int i;

        for (i = 0; i < 26; i++) {
            if ((appeared & (1u << i)) != 0)
                jlog("attach: mounted as %c:", 'A' + i);
        }
    } else {
        /* Not fatal: the volume arrives through PnP, so the letter can lag
         * this call by a moment. Setup only needs it before it reads media. */
        jlog("attach: succeeded, no new drive letter visible yet");
    }

    CloseHandle(disk);
    return TRUE;
}

/* ---- the driver-mount fallback (DESIGN Section 6.3) ----
 *
 * When VirtDisk cannot attach the ISO, present it through falcon-vdisk, the
 * kernel driver carried in this binary. The loader drops the ISO's extent map
 * beside winjump; winjump finds the disk the map names, hands the driver the
 * disk and the runs, and the driver serves the ISO as a CD Setup mounts.
 *
 * Nothing here is reached when the VirtDisk attach worked. Every failure is
 * logged and returns FALSE, and the shell still launches. */

extern const unsigned char falcon_vdisk_bin[];
extern const size_t falcon_vdisk_bin_len;

/* The driver's names. */
#define VDISK_SERVICE   L"falconvdisk"
#define VDISK_CTL_PATH  L"\\\\.\\FalconVDisk"

/* Writes the embedded driver to a temp file and returns its path (a static
 * buffer). NULL on failure. */
static const wchar_t *write_driver(void)
{
    static wchar_t path[MAX_PATH];
    wchar_t dir[MAX_PATH];
    HANDLE h;
    DWORD wrote = 0;

    if (GetTempPathW(MAX_PATH, dir) == 0 ||
        !jwjoin(path, MAX_PATH, dir, L"falcon-vdisk.sys"))
        return NULL;
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        jlog("driver: cannot write %ls, err=%lu", path, GetLastError());
        return NULL;
    }
    if (!WriteFile(h, falcon_vdisk_bin, (DWORD)falcon_vdisk_bin_len, &wrote,
                   NULL) || wrote != (DWORD)falcon_vdisk_bin_len) {
        jlog("driver: write failed, err=%lu", GetLastError());
        CloseHandle(h);
        return NULL;
    }
    CloseHandle(h);
    return path;
}

/* Registers and starts the driver, returns an open handle to its control
 * device, or NULL. A stale service from an earlier run is reused. */
static HANDLE driver_start(const wchar_t *sys)
{
    SC_HANDLE scm, svc;
    HANDLE ctl;

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (scm == NULL) {
        jlog("driver: OpenSCManager err=%lu", GetLastError());
        return NULL;
    }
    svc = CreateServiceW(scm, VDISK_SERVICE, VDISK_SERVICE, SERVICE_ALL_ACCESS,
                         SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
                         SERVICE_ERROR_NORMAL, sys, NULL, NULL, NULL, NULL,
                         NULL);
    if (svc == NULL && GetLastError() == ERROR_SERVICE_EXISTS)
        svc = OpenServiceW(scm, VDISK_SERVICE, SERVICE_ALL_ACCESS);
    if (svc == NULL) {
        jlog("driver: CreateService err=%lu", GetLastError());
        CloseServiceHandle(scm);
        return NULL;
    }
    if (!StartServiceW(svc, 0, NULL) &&
        GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        jlog("driver: StartService err=%lu", GetLastError());
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return NULL;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    ctl = CreateFileW(VDISK_CTL_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                      OPEN_EXISTING, 0, NULL);
    if (ctl == INVALID_HANDLE_VALUE) {
        jlog("driver: open control device err=%lu", GetLastError());
        return NULL;
    }
    return ctl;
}

/* Finds the physical disk whose GPT carries the partition `part_guid` names.
 * Fills `nt_path` with the disk's NT device path for the driver and *first_lba
 * with the partition's first LBA. */
static BOOL find_falcon_disk(const uint8_t part_guid[16], wchar_t *nt_path,
                             size_t cap, uint64_t *first_lba)
{
    uint8_t hdr[512];
    static uint8_t ents[128 * 128];
    int n;

    for (n = 0; n < 32; n++) {
        wchar_t phys[32];
        HANDLE h;
        DWORD got = 0;
        LARGE_INTEGER at;

        wsprintfW(phys, L"\\\\.\\PhysicalDrive%d", n);
        h = CreateFileW(phys, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        at.QuadPart = 512;                          /* LBA 1: the GPT header */
        got = 0;
        if (SetFilePointerEx(h, at, NULL, FILE_BEGIN) &&
            ReadFile(h, hdr, sizeof hdr, &got, NULL) && got == sizeof hdr) {
            at.QuadPart = 1024;                     /* LBA 2: the entry array */
            got = 0;
            if (SetFilePointerEx(h, at, NULL, FILE_BEGIN) &&
                ReadFile(h, ents, sizeof ents, &got, NULL) &&
                wj_gpt_match(hdr, ents, got, part_guid, first_lba) == WJ_OK) {
                CloseHandle(h);
                if ((size_t)wsprintfW(nt_path, L"\\Device\\Harddisk%d\\DR%d",
                                      n, n) >= cap)
                    return FALSE;
                jlog("driver: ISO on PhysicalDrive%d, partition first LBA %s",
                     n, u64s((char[24]){ 0 }, *first_lba));
                return TRUE;
            }
        }
        CloseHandle(h);
    }
    jlog("driver: no disk carries the ISO's partition GUID");
    return FALSE;
}

/* Presents the ISO to Windows through the driver: from the map the loader put
 * in the cfg, find the disk, hand the driver the disk path and runs, and bring
 * the CD online. */
static BOOL mount_via_driver(const struct cfg *cfg)
{
    wchar_t nt_path[64];
    const wchar_t *sys;
    extmap_hdr hdr;
    uint64_t first_lba = 0;
    HANDLE ctl;
    wj_extent exts[EXTMAP_MAX_EXTENTS];
    uint32_t n_ext = 0;
    struct falcon_vdisk_backing *b;
    size_t path_bytes, msg_len;
    uint8_t *msg;
    DWORD ret = 0;
    BOOL ok = FALSE;

    if (cfg->map == NULL) {
        jlog("driver: the loader left no extent map (an older loader?)");
        return FALSE;
    }
    if (extmap_decode(cfg->map, cfg->map_len, &hdr) != EXTMAP_OK ||
        wj_backing_extents(&hdr, cfg->map, cfg->map_len, exts,
                           EXTMAP_MAX_EXTENTS, &n_ext) != WJ_OK) {
        jlog("driver: extent map is not one this reads");
        return FALSE;
    }

    if (!find_falcon_disk(hdr.part_guid, nt_path, 64, &first_lba))
        return FALSE;

    sys = write_driver();
    if (sys == NULL)
        return FALSE;
    ctl = driver_start(sys);
    if (ctl == NULL)
        return FALSE;

    /* SET_BACKING: header, the disk's NT path in UTF-16 (no NUL), the runs. */
    path_bytes = jwcslen(nt_path) * sizeof(wchar_t);
    msg_len = sizeof *b + path_bytes + (size_t)n_ext * sizeof(wj_extent);
    msg = HeapAlloc(GetProcessHeap(), 0, msg_len);
    if (msg == NULL) {
        CloseHandle(ctl);
        return FALSE;
    }
    b = (struct falcon_vdisk_backing *)msg;
    b->iso_bytes = hdr.iso_size;
    b->path_bytes = (unsigned short)path_bytes;
    b->reserved = 0;
    b->extent_count = n_ext;
    memcpy(msg + sizeof *b, nt_path, path_bytes);
    memcpy(msg + sizeof *b + path_bytes, exts, (size_t)n_ext * sizeof(wj_extent));

    if (!DeviceIoControl(ctl, IOCTL_FALCON_VDISK_SET_BACKING, msg,
                         (DWORD)msg_len, NULL, 0, &ret, NULL)) {
        jlog("driver: SET_BACKING err=%lu", GetLastError());
    } else if (!DeviceIoControl(ctl, IOCTL_FALCON_VDISK_ONLINE, NULL, 0, NULL,
                                0, &ret, NULL)) {
        jlog("driver: ONLINE err=%lu", GetLastError());
    } else {
        jlog("driver: ISO online as a CD, %lu run(s)", (unsigned long)n_ext);
        ok = TRUE;
    }
    HeapFree(GetProcessHeap(), 0, msg);
    CloseHandle(ctl);
    return ok;
}

/* ---- the user's options (p2:\\falcon\\windows.cfg) ---- */

static void opt_key(void *ctx, const char *key, const char *value)
{
    struct stick_opts *o = ctx;

    if (jstrcmp(key, "win11_bypass") == 0)
        o->win11_bypass = (jstrcmp(value, "1") == 0 ||
                           jstrcmp(value, "yes") == 0 ||
                           jstrcmp(value, "true") == 0);
    else if (jstrcmp(key, "unattend") == 0)
        lstrcpynA(o->unattend, value, MAX_PATH);
    else
        jlog("options: ignoring unknown key '%s'", key);
}

/* Setup reads HKLM\\SYSTEM\\Setup\\LabConfig before it judges the machine;
 * the names are Microsoft's, documented only by use. */
static void win11_bypass(void)
{
    static const wchar_t *const names[] = {
        L"BypassTPMCheck", L"BypassSecureBootCheck", L"BypassRAMCheck",
        L"BypassCPUCheck", L"BypassStorageCheck"
    };
    HKEY key;
    DWORD one = 1;
    LSTATUS rc;
    size_t i;

    rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\Setup\\LabConfig", 0,
                         NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL,
                         &key, NULL);
    if (rc != ERROR_SUCCESS) {
        jlog("options: cannot create LabConfig, rc=%ld", (long)rc);
        return;
    }
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        rc = RegSetValueExW(key, names[i], 0, REG_DWORD, (const BYTE *)&one,
                            sizeof one);
        if (rc != ERROR_SUCCESS)
            jlog("options: setting %ls failed, rc=%ld", names[i], (long)rc);
    }
    RegCloseKey(key);
    jlog("options: Windows 11 requirement checks bypassed");
}

/* HKLM\\SYSTEM\\Setup\\UnattendFile is where Setup looks for an answer
 * file before it looks anywhere else. */
static void use_unattend(const wchar_t *root, const char *rel)
{
    wchar_t tail[MAX_PATH];
    wchar_t src[MAX_PATH];
    HKEY key;
    LSTATUS rc;

    if (!widen_relative(rel, tail, MAX_PATH) ||
        !jwjoin(src, MAX_PATH, root, tail))
        return;
    if (!CopyFileW(src, UNATTEND_COPY, FALSE)) {
        jlog("options: cannot copy %ls, err=%lu", src, GetLastError());
        return;
    }
    rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\Setup", 0, NULL,
                         REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &key,
                         NULL);
    if (rc == ERROR_SUCCESS) {
        rc = RegSetValueExW(key, L"UnattendFile", 0, REG_SZ,
                            (const BYTE *)UNATTEND_COPY,
                            (DWORD)sizeof UNATTEND_COPY);
        RegCloseKey(key);
    }
    if (rc != ERROR_SUCCESS) {
        jlog("options: cannot set UnattendFile, rc=%ld", (long)rc);
        return;
    }
    jlog("options: unattend %ls", src);
}

/* The options file on the volume the ISO was found on. Absent is the
 * ordinary case and changes nothing. */
static void apply_stick_options(const wchar_t *iso)
{
    struct stick_opts o;
    wchar_t root[4];
    wchar_t path[MAX_PATH];

    memset(&o, 0, sizeof o);
    if (jwcslen(iso) < 3 || iso[1] != L':' || iso[2] != L'\\')
        return;
    lstrcpynW(root, iso, 4);
    if (!jwjoin(path, MAX_PATH, root, STICK_CFG))
        return;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        jlog("options: no %ls", path);
        return;
    }
    if (!read_kv(path, opt_key, &o))
        return;
    if (o.win11_bypass)
        win11_bypass();
    if (o.unattend[0] != '\0')
        use_unattend(root, o.unattend);
}

/*
 * Run the shell we displaced.
 *
 * We wait for it. WinPE treats the shell process exiting as the end of the
 * session and reboots, so returning while Setup is still running would kill
 * the machine mid-install. This matches vtoyjump, which waits on the same
 * handle for the same reason.
 */
static int launch_shell(const wchar_t *dir, const struct cfg *cfg)
{
    wchar_t path[MAX_PATH];
    wchar_t cmdline[MAX_PATH + 4];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 0;

    if (cfg->shell[0] != '\0') {
        if (MultiByteToWideChar(CP_ACP, 0, cfg->shell, -1, path, MAX_PATH) == 0) {
            jlog("shell: cannot widen %s, err=%lu", cfg->shell, GetLastError());
            return 1;
        }
    } else if (!jwjoin(path, MAX_PATH, dir, DEFAULT_SHELL)) {
        jlog("shell: path too long");
        return 1;
    }

    /* CreateProcessW may write to the command line buffer, so it cannot be a
     * literal; quoted because the path can contain spaces. */
    if (!jwjoin(cmdline, MAX_PATH + 4, L"\"", path) ||
        !jwjoin(cmdline + jwcslen(cmdline), MAX_PATH + 4 - jwcslen(cmdline),
                L"\"", L"")) {
        jlog("shell: command line too long");
        return 1;
    }

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);

    if (!CreateProcessW(path, cmdline, NULL, NULL, FALSE, 0, NULL, dir,
                        &si, &pi)) {
        jlog("shell: CreateProcess %ls failed, err=%lu", path, GetLastError());
        return 1;
    }
    jlog("shell: launched %ls, waiting", path);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    jlog("shell: exited with %lu", code);
    return (int)code;
}

static int winjump_main(void)
{
    wchar_t dir[MAX_PATH];
    wchar_t iso[MAX_PATH];
    struct cfg cfg;

    memset(&cfg, 0, sizeof cfg);
    jlog("winjump: start");

    if (!module_dir(dir, MAX_PATH)) {
        jlog("winjump: cannot locate own directory; nothing else is possible");
        return 1;
    }
    jlog("winjump: module dir %ls", dir);

    /* From here every failure is logged and then ignored: a WinPE with no
     * media still has Shift+F10 and this log, and is worth a great deal more
     * than a machine that reboots without a shell.
     *
     * VirtDisk first (Windows 8+, when the ISO's volume is readable), then the
     * driver (Vista and 7, whose WinPE has neither ISO support nor a way to
     * read the exFAT partition the ISO lives on). */
    {
        BOOL mounted = FALSE;
        BOOL have_cfg = read_cfg(dir, &cfg);

        if (have_cfg && enable_manage_volume() &&
            find_iso(&cfg, iso, MAX_PATH)) {
            mounted = attach_iso(iso);
            apply_stick_options(iso);
        }
        if (!mounted && have_cfg) {
            jlog("winjump: VirtDisk did not mount the ISO; trying the driver");
            mounted = mount_via_driver(&cfg);
        }
        if (!mounted)
            jlog("winjump: no media presented; launching the shell anyway");
    }

    return launch_shell(dir, &cfg);
}

/* The process entry point, in place of the C runtime's. The stack guard is
 * seeded here, before any function the protector instruments runs: the tick
 * count and the performance counter differ on every boot, and this function
 * is built without the protector so its own frame has no canary to break. */
/* The name is the linker's default entry point for a console program, which
 * is why nothing names it on the command line: each toolchain decorates it
 * (the leading underscore of an i386 symbol) its own way, and passing -e
 * with one spelling left Debian's i686 linker entering at the start of .text
 * instead, silently. build.mk checks the entry after every link. */
void mainCRTStartup(void) __attribute__((no_stack_protector, noreturn));
void mainCRTStartup(void)
{
    LARGE_INTEGER pc;

    pc.QuadPart = 0;
    (void)QueryPerformanceCounter(&pc);
    __stack_chk_guard ^= (__typeof__(__stack_chk_guard))
        ((unsigned long long)pc.QuadPart ^ GetTickCount64() ^
         (unsigned long long)GetCurrentProcessId() << 16);
    ExitProcess((UINT)winjump_main());
}
