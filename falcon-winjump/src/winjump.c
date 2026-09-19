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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* WinPE's RAM disk is X: and is writable. The log is the only view into a
 * black box: everything here is inside a RAM disk that evaporates on reboot,
 * so a run that fails silently cannot be debugged at all. */
#define LOG_PATH        L"X:\\falcon-winjump.log"
#define CFG_NAME        L"falcon-winjump.cfg"
#define DEFAULT_SHELL   L"winpeshl_orig.exe"

struct cfg {
    char iso[MAX_PATH];       /* ISO path relative to a volume root */
    unsigned long long size;  /* its exact byte length, 0 if unspecified */
    char shell[MAX_PATH];     /* original shell, empty for the default */
};

static void __attribute__((format(__MINGW_PRINTF_FORMAT, 1, 2)))
jlog(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    HANDLE h;
    DWORD written;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n > sizeof line - 3)
        n = (int)(sizeof line - 3);
    line[n] = '\r';
    line[n + 1] = '\n';

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

/* Dead-simple ASCII key=value, one per line, '#' comments. Written by the
 * bless tool, read only here; it never has to survive a user editing it. */
static BOOL read_cfg(const wchar_t *dir, struct cfg *cfg)
{
    wchar_t path[MAX_PATH];
    char buf[4096];
    HANDLE h;
    DWORD got = 0;
    char *line;
    char *next;

    if (swprintf(path, MAX_PATH, L"%ls%ls", dir, CFG_NAME) < 0)
        return FALSE;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        jlog("cfg: cannot open %ls, err=%lu", path, GetLastError());
        return FALSE;
    }
    if (!ReadFile(h, buf, sizeof buf - 1, &got, NULL)) {
        jlog("cfg: read failed, err=%lu", GetLastError());
        CloseHandle(h);
        return FALSE;
    }
    CloseHandle(h);
    buf[got] = '\0';

    for (line = buf; line != NULL && *line != '\0'; line = next) {
        char *eq;

        next = strchr(line, '\n');
        if (next != NULL)
            *next++ = '\0';
        trim(line);
        if (*line == '\0' || *line == '#')
            continue;

        eq = strchr(line, '=');
        if (eq == NULL)
            continue;
        *eq++ = '\0';

        if (strcmp(line, "iso") == 0) {
            lstrcpynA(cfg->iso, eq, MAX_PATH);
        } else if (strcmp(line, "shell") == 0) {
            lstrcpynA(cfg->shell, eq, MAX_PATH);
        } else if (strcmp(line, "size") == 0) {
            cfg->size = strtoull(eq, NULL, 10);
        } else {
            jlog("cfg: ignoring unknown key '%s'", line);
        }
    }

    if (cfg->iso[0] == '\0') {
        jlog("cfg: no iso= key");
        return FALSE;
    }
    jlog("cfg: iso=%s size=%llu shell=%s", cfg->iso, cfg->size,
         cfg->shell[0] != '\0' ? cfg->shell : "(default)");
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
        if (wcscmp(prev_seen, seen) != 0)
            jlog("scan: GetLogicalDriveStrings failed, err=%lu", GetLastError());
        return FALSE;
    }

    for (root = drives; *root != L'\0'; root += wcslen(root) + 1) {
        int w;

        if (SEEN_CAP - at < 16)
            break;
        w = swprintf(seen + at, SEEN_CAP - at, L"%ls(%u) ", root,
                     (unsigned)GetDriveTypeW(root));
        if (w < 0) {
            /* A truncating swprintf leaves the buffer indeterminate, so the
             * terminator has to be restored before anything logs it. */
            seen[at] = L'\0';
            break;
        }
        at += (size_t)w;
    }

    verbose = wcscmp(prev_seen, seen) != 0;
    if (verbose)
        jlog("scan: drives %ls", seen);

    for (root = drives; *root != L'\0'; root += wcslen(root) + 1) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        unsigned long long size;

        if (swprintf(out, (size_t)cap, L"%ls%ls", root, rel) < 0)
            continue;
        if (!GetFileAttributesExW(out, GetFileExInfoStandard, &fad)) {
            if (verbose)
                jlog("scan: %ls absent", out);
            continue;
        }

        size = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (cfg->size != 0 && size != cfg->size) {
            if (verbose)
                jlog("scan: %ls is %llu bytes, want %llu; skipping", out, size,
                     cfg->size);
            continue;
        }
        jlog("scan: found %ls (%llu bytes)", out, size);
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
            jlog("scan: no volume carries %s after %llu ms and %u passes",
                 cfg->iso, now - start, passes);
            jlog("scan: last drives seen %ls", seen);
            return FALSE;
        }
        if (now - beat >= FIND_HEARTBEAT_MS) {
            jlog("scan: still waiting, %llu ms elapsed, %u passes, drives %ls",
                 now - start, passes, seen);
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
static BOOL attach_iso(const wchar_t *path)
{
    VIRTUAL_STORAGE_TYPE st;
    OPEN_VIRTUAL_DISK_PARAMETERS op;
    ATTACH_VIRTUAL_DISK_PARAMETERS ap;
    HANDLE disk = INVALID_HANDLE_VALUE;
    DWORD before, after, appeared;
    DWORD rc;

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

    rc = OpenVirtualDisk(&st, path, VIRTUAL_DISK_ACCESS_READ,
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
    rc = AttachVirtualDisk(disk, NULL,
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
    } else if (swprintf(path, MAX_PATH, L"%ls%ls", dir, DEFAULT_SHELL) < 0) {
        jlog("shell: path too long");
        return 1;
    }

    /* CreateProcessW may write to the command line buffer, so it cannot be a
     * literal; quoted because the path can contain spaces. */
    if (swprintf(cmdline, MAX_PATH + 4, L"\"%ls\"", path) < 0) {
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

int main(void)
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
     * than a machine that reboots without a shell. */
    if (enable_manage_volume() && read_cfg(dir, &cfg) &&
        find_iso(&cfg, iso, MAX_PATH))
        (void)attach_iso(iso);
    else
        jlog("winjump: attach skipped; launching the original shell anyway");

    return launch_shell(dir, &cfg);
}
