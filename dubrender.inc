/* ===================================================================
   dubrender.inc - the "Dub Renderer" tab.

   A C port of the STANDARD (non-co-op) half of the open-source
   "Choicer Voicer Dub Renderer" (Go, MIT licensed). Nothing from its
   co-op section is included. Included from main.c just before
   SwitchTab(), so it can use the launcher's fonts, colors, MakeButton()
   and DrawThemedOwnerButton() directly.

   What it does (same behavior as the original):
     1. Scans %APPDATA%\YeahMaybe\ChoicerVoicer\game\recordings\dub_recordings
        for export folders (any folder holding _dubrecord_*.wav files).
     2. Auto-detects the matching original pack under packs_voice
        (dub_video.* + a .txt/.ini with dub_timestamps = [ ... ]).
     3. Finds a compatible FFmpeg (beside the launcher, WinGet, Scoop,
        Chocolatey, PATH) and can install one through winget on request.
     4. Mixes every timed voice clip (batches of 20) with the pack's
        _backing_track, centers single-sided mono recordings, and encodes
        an H.264/AAC MP4 over dub_video.*.
   Everything slow runs on worker threads and reports back with
   PostMessage, so the launcher never freezes.
   =================================================================== */

#include <stdarg.h>

#define DUB_PATH 1024

#define ID_DUB_COMBO        8101
#define ID_DUB_REFRESH      8102
#define ID_DUB_BROWSE_REC   8103
#define ID_DUB_BROWSE_PACK  8104
#define ID_DUB_BROWSE_OUT   8105
#define ID_DUB_RENDER       8106
#define ID_DUB_CANCEL       8107
#define ID_DUB_OPEN         8108
#define ID_DUB_INFO         8109
#define ID_DUB_CREDIT_LINK  8110
#define ID_DUB_CREDIT_CLOSE 8111
#define DUB_CREDIT_URL      L"https://gamebanana.com/tools/23587"

#define WM_DUB_LOG        (WM_APP + 300)  /* lParam: malloc'd wchar_t* line           */
#define WM_DUB_STATUS     (WM_APP + 301)  /* wParam: color kind, lParam: malloc'd text */
#define WM_DUB_SCANDONE   (WM_APP + 302)  /* lParam: DubScanResult*                   */
#define WM_DUB_FFCHECK    (WM_APP + 303)  /* wParam: 1 found / 0 none / 2 incompatible, lParam: malloc'd path */
#define WM_DUB_FFINSTALL  (WM_APP + 304)  /* wParam: 1 ok / 0 failed, lParam: malloc'd path or error */
#define WM_DUB_RENDERDONE (WM_APP + 305)  /* wParam: 0 ok / 1 error / 2 cancelled, lParam: malloc'd text */

enum { DUBC_MUTED = 0, DUBC_ACCENT, DUBC_SUCCESS, DUBC_WARNING, DUBC_DANGER };
enum { DST_SCAN = 0, DST_PACK, DST_RENDER, DST_FF, DST_COUNT };

/* ---------------- state (UI thread unless noted) ---------------- */

/* g_hDubWnd / g_hDubCombo / g_hDubLog are declared near the top of main.c
   so ApplyThemeColors() can reach them */
static HWND g_hDubTitle, g_hDubSubtitle;
static HWND g_hDubCapRec, g_hDubCapPack, g_hDubCapOut;
static HWND g_hDubRecEdit, g_hDubPackEdit, g_hDubOutEdit;
static HWND g_hDubRefreshBtn, g_hDubBrowseRecBtn, g_hDubBrowsePackBtn, g_hDubBrowseOutBtn;
static HWND g_hDubRenderBtn, g_hDubCancelBtn, g_hDubOpenBtn, g_hDubInfoBtn;
static HWND g_hDubStatus[DST_COUNT];
static int  g_dubStatusKind[DST_COUNT];

static BOOL g_dubStarted = FALSE;       /* first-open work (scan + FFmpeg check) kicked off */
static BOOL g_dubScanning = FALSE;
static BOOL g_dubRendering = FALSE;
static BOOL g_dubFfChecking = FALSE;
static BOOL g_dubFfInstalling = FALSE;
static BOOL g_dubFfReady = FALSE;
static int  g_dubFfDetected = 0;        /* 0 none found, 2 found but incompatible (for the install prompt) */
static wchar_t g_dubFfPath[DUB_PATH];
static wchar_t g_dubLastOutput[DUB_PATH];

typedef struct {
    wchar_t  path[DUB_PATH];
    wchar_t  display[DUB_PATH + 256];
    FILETIME mod;
} DubExport;
typedef struct { DubExport* items; int count; } DubScanResult;
static DubExport* g_dubExports = NULL;
static int g_dubExportCount = 0;

static RECT g_dubHdrRect[3];
static const wchar_t* g_dubHdrText[3] = { L"Choose an export", L"Verify source folders", L"Render the finished dub" };
static RECT g_dubBarRect;
static int  g_dubBarTick = 0;
#define DUB_TIMER_ID 7

/* shared with worker threads */
static volatile LONG g_dubCancel = 0;
static CRITICAL_SECTION g_dubProcCs;
static BOOL   g_dubProcCsInit = FALSE;
static HANDLE g_dubProc = NULL;         /* the running FFmpeg/winget process, so Cancel can kill it */
static wchar_t g_dubStage[64] = L"";    /* name of the FFmpeg stage currently running (render thread only) */

/* ---------------- tiny string builder ---------------- */

typedef struct { wchar_t* p; size_t len, cap; } DubSb;

static void DubSbInit(DubSb* s)
{
    s->cap = 256; s->len = 0;
    s->p = (wchar_t*)malloc(s->cap * sizeof(wchar_t));
    if (s->p) s->p[0] = 0;
}
static void DubSbAppend(DubSb* s, const wchar_t* t)
{
    if (!s->p) return;
    size_t n = wcslen(t);
    if (s->len + n + 1 > s->cap) {
        size_t nc = s->cap;
        while (nc < s->len + n + 1) nc *= 2;
        wchar_t* np = (wchar_t*)realloc(s->p, nc * sizeof(wchar_t));
        if (!np) { free(s->p); s->p = NULL; s->len = s->cap = 0; return; }
        s->p = np; s->cap = nc;
    }
    memcpy(s->p + s->len, t, n * sizeof(wchar_t));
    s->len += n;
    s->p[s->len] = 0;
}
static void DubSbAppendCh(DubSb* s, wchar_t c) { wchar_t t[2] = { c, 0 }; DubSbAppend(s, t); }
static void DubSbFree(DubSb* s) { free(s->p); s->p = NULL; s->len = s->cap = 0; }

/* appends one argument to a command line, quoted the way
   CommandLineToArgvW expects to read it back */
static void DubAppendArg(DubSb* cmd, const wchar_t* arg)
{
    if (cmd->len > 0) DubSbAppendCh(cmd, L' ');
    if (*arg && !wcspbrk(arg, L" \t\n\v\"")) { DubSbAppend(cmd, arg); return; }
    DubSbAppendCh(cmd, L'"');
    for (const wchar_t* p = arg; ; p++) {
        int bs = 0;
        while (*p == L'\\') { bs++; p++; }
        if (!*p) { for (int i = 0; i < bs * 2; i++) DubSbAppendCh(cmd, L'\\'); break; }
        if (*p == L'"') { for (int i = 0; i < bs * 2 + 1; i++) DubSbAppendCh(cmd, L'\\'); DubSbAppendCh(cmd, L'"'); }
        else            { for (int i = 0; i < bs; i++) DubSbAppendCh(cmd, L'\\'); DubSbAppendCh(cmd, *p); }
    }
    DubSbAppendCh(cmd, L'"');
}

/* ---------------- posting to the UI thread ---------------- */

static void DubPost(UINT msg, WPARAM w, LPARAM l, BOOL ownsPtr)
{
    HWND h = g_hDubWnd;
    if (!h || !PostMessageW(h, msg, w, l)) {
        if (ownsPtr && l) free((void*)l);
    }
}
static void DubPostText(UINT msg, WPARAM w, const wchar_t* text)
{
    wchar_t* d = _wcsdup(text ? text : L"");
    if (d) DubPost(msg, w, (LPARAM)d, TRUE);
}
static void DubLog(const wchar_t* text) { DubPostText(WM_DUB_LOG, 0, text); }
static void DubLogF(const wchar_t* fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    buf[2047] = 0;
    DubLog(buf);
}

/* ---------------- path helpers ---------------- */

static BOOL DubStartsWithI(const wchar_t* s, const wchar_t* prefix) { return _wcsnicmp(s, prefix, wcslen(prefix)) == 0; }
static BOOL DubEndsWithI(const wchar_t* s, const wchar_t* suffix)
{
    size_t a = wcslen(s), b = wcslen(suffix);
    return a >= b && _wcsicmp(s + a - b, suffix) == 0;
}

static void DubCopy(wchar_t* dst, size_t cap, const wchar_t* src)
{
    wcsncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

/* forward slashes -> backslashes, and drop trailing backslashes (but keep "C:\") */
static void DubClean(wchar_t* p)
{
    for (wchar_t* c = p; *c; c++) if (*c == L'/') *c = L'\\';
    size_t n = wcslen(p);
    while (n > 3 && p[n - 1] == L'\\') p[--n] = 0;
}

static const wchar_t* DubBaseName(const wchar_t* p)
{
    const wchar_t* s = wcsrchr(p, L'\\');
    return s ? s + 1 : p;
}

static void DubDirName(const wchar_t* p, wchar_t* out, size_t cap)
{
    DubCopy(out, cap, p);
    wchar_t* s = wcsrchr(out, L'\\');
    if (!s) { DubCopy(out, cap, L"."); return; }
    if (s == out) { out[1] = 0; return; }                 /* "\x" -> "\" */
    if (s == out + 2 && out[1] == L':') { out[3] = 0; return; }   /* "C:\x" -> "C:\" */
    *s = 0;
}

static void DubJoin(wchar_t* out, size_t cap, const wchar_t* a, const wchar_t* b)
{
    size_t la = wcslen(a);
    BOOL sep = la > 0 && (a[la - 1] == L'\\' || a[la - 1] == L'/');
    _snwprintf(out, cap - 1, L"%ls%ls%ls", a, sep ? L"" : L"\\", b);
    out[cap - 1] = 0;
}

static void DubBaseNoExt(const wchar_t* name, wchar_t* out, size_t cap)
{
    DubCopy(out, cap, name);
    wchar_t* dot = wcsrchr(out, L'.');
    if (dot) *dot = 0;
}

static BOOL DubIsDir(const wchar_t* path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* a real, non-empty file */
static BOOL DubFileExists(const wchar_t* path)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &d)) return FALSE;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return FALSE;
    return d.nFileSizeHigh != 0 || d.nFileSizeLow != 0;
}

static BOOL DubPathExists(const wchar_t* path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

/* %APPDATA%\YeahMaybe\ChoicerVoicer\game[\a[\b]] */
static BOOL DubAppPath(wchar_t* out, size_t cap, const wchar_t* a)
{
    wchar_t gd[DUB_PATH];
    if (!GetAppDataGameDir(gd)) return FALSE;
    if (a && *a) DubJoin(out, cap, gd, a); else DubCopy(out, cap, gd);
    return TRUE;
}

typedef struct { wchar_t name[MAX_PATH]; DWORD attrs; } DubEnt;
static int DubEntCmp(const void* a, const void* b) { return wcscmp(((const DubEnt*)a)->name, ((const DubEnt*)b)->name); }

/* sorted directory listing; caller frees the result. NULL = couldn't open. */
static DubEnt* DubListDir(const wchar_t* dir, int* count)
{
    *count = 0;
    wchar_t pat[DUB_PATH];
    DubJoin(pat, DUB_PATH, dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    int cap = 64, n = 0;
    DubEnt* a = (DubEnt*)malloc(cap * sizeof(DubEnt));
    if (!a) { FindClose(h); return NULL; }
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (n == cap) {
            cap *= 2;
            DubEnt* na = (DubEnt*)realloc(a, cap * sizeof(DubEnt));
            if (!na) break;
            a = na;
        }
        DubCopy(a[n].name, MAX_PATH, fd.cFileName);
        a[n].attrs = fd.dwFileAttributes;
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    qsort(a, n, sizeof(DubEnt), DubEntCmp);
    *count = n;
    return a;
}
#define DUB_ENT_IS_DIR(e) (((e)->attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)

static char* DubReadFile(const wchar_t* path, size_t* lenOut)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > (LONGLONG)512 * 1024 * 1024) {
        CloseHandle(h);
        return NULL;
    }
    char* buf = (char*)malloc((size_t)sz.QuadPart + 1);
    if (!buf) { CloseHandle(h); return NULL; }
    size_t got = 0;
    while (got < (size_t)sz.QuadPart) {
        DWORD chunk = 0;
        size_t want = (size_t)sz.QuadPart - got;
        if (want > 1u << 24) want = 1u << 24;
        if (!ReadFile(h, buf + got, (DWORD)want, &chunk, NULL) || chunk == 0) break;
        got += chunk;
    }
    CloseHandle(h);
    buf[got] = 0;
    if (lenOut) *lenOut = got;
    return buf;
}

typedef struct { wchar_t part[32][260]; int count; } DubParts;

/* the folder names between root and path, or count 0 if path isn't below root */
static void DubPartsBelow(const wchar_t* rootIn, const wchar_t* pathIn, DubParts* out)
{
    out->count = 0;
    wchar_t root[DUB_PATH], path[DUB_PATH];
    DubCopy(root, DUB_PATH, rootIn); DubClean(root);
    DubCopy(path, DUB_PATH, pathIn); DubClean(path);
    size_t rl = wcslen(root);
    if (_wcsnicmp(path, root, rl) != 0 || path[rl] == 0) return;
    if (path[rl] != L'\\' && !(rl > 0 && root[rl - 1] == L'\\')) return;
    const wchar_t* rel = path + rl;
    if (*rel == L'\\') rel++;
    while (*rel && out->count < 32) {
        const wchar_t* e = wcschr(rel, L'\\');
        size_t n = e ? (size_t)(e - rel) : wcslen(rel);
        if (n > 0) {
            if (n >= 260) n = 259;
            if (n == 2 && rel[0] == L'.' && rel[1] == L'.') { out->count = 0; return; }
            memcpy(out->part[out->count], rel, n * sizeof(wchar_t));
            out->part[out->count][n] = 0;
            out->count++;
        }
        if (!e) break;
        rel = e + 1;
    }
}

/* ---------------- pack / export detection ---------------- */

static BOOL DubIsRecordingWav(const wchar_t* name)
{
    return DubStartsWithI(name, L"_dubrecord_") && DubEndsWithI(name, L".wav");
}

static int DubCountRecordingWavs(const wchar_t* dir)
{
    int n = 0, count = 0;
    DubEnt* e = DubListDir(dir, &n);
    for (int i = 0; i < n; i++) if (!DUB_ENT_IS_DIR(&e[i]) && DubIsRecordingWav(e[i].name)) count++;
    free(e);
    return count;
}
static BOOL DubContainsRecordingWavs(const wchar_t* dir) { return DubCountRecordingWavs(dir) > 0; }

/* does buf[i...] look like  dub_timestamps = [  ?  (case-insensitive, any whitespace) */
static BOOL DubMatchTsOpenAt(const char* buf, size_t len, size_t i, size_t* after)
{
    static const char key[] = "dub_timestamps";
    const size_t kl = sizeof(key) - 1;
    if (i + kl > len || _strnicmp(buf + i, key, kl) != 0) return FALSE;
    size_t j = i + kl;
    while (j < len && isspace((unsigned char)buf[j])) j++;
    if (j >= len || buf[j] != '=') return FALSE;
    j++;
    while (j < len && isspace((unsigned char)buf[j])) j++;
    if (j >= len || buf[j] != '[') return FALSE;
    *after = j + 1;
    return TRUE;
}

static BOOL DubHasTsOpen(const char* buf, size_t len)
{
    size_t after;
    for (size_t i = 0; i < len; i++) if (DubMatchTsOpenAt(buf, len, i, &after)) return TRUE;
    return FALSE;
}

/* the text between the [ and the first ] of  dub_timestamps = [ ... ] */
static BOOL DubFindTsBlock(const char* buf, size_t len, size_t* bStart, size_t* bEnd)
{
    size_t after;
    for (size_t i = 0; i < len; i++) {
        if (!DubMatchTsOpenAt(buf, len, i, &after)) continue;
        size_t k = after;
        while (k < len && buf[k] != ']') k++;
        if (k >= len) return FALSE;
        *bStart = after; *bEnd = k;
        return TRUE;
    }
    return FALSE;
}

/* a pack folder has a dub_video.* file plus a .txt/.ini with dub_timestamps = [ */
static BOOL DubValidPackFolder(const wchar_t* dir)
{
    int n = 0;
    DubEnt* e = DubListDir(dir, &n);
    if (!e) return FALSE;
    BOOL hasVideo = FALSE;
    for (int i = 0; i < n; i++) {
        if (DUB_ENT_IS_DIR(&e[i])) continue;
        wchar_t base[MAX_PATH];
        DubBaseNoExt(e[i].name, base, MAX_PATH);
        if (_wcsicmp(base, L"dub_video") == 0) { hasVideo = TRUE; break; }
    }
    BOOL ok = FALSE;
    if (hasVideo) {
        for (int i = 0; i < n && !ok; i++) {
            if (DUB_ENT_IS_DIR(&e[i])) continue;
            if (!DubEndsWithI(e[i].name, L".txt") && !DubEndsWithI(e[i].name, L".ini")) continue;
            wchar_t p[DUB_PATH];
            DubJoin(p, DUB_PATH, dir, e[i].name);
            size_t len = 0;
            char* b = DubReadFile(p, &len);
            if (b) { ok = DubHasTsOpen(b, len); free(b); }
        }
    }
    free(e);
    return ok;
}

/* first folder beneath ...\recordings\dub_recordings that contains the export */
static void DubPackNameFromExport(const wchar_t* recordingFolder, wchar_t* out, size_t cap)
{
    wchar_t root[DUB_PATH];
    if (DubAppPath(root, DUB_PATH, L"recordings\\dub_recordings")) {
        DubClean(root);
        wchar_t cur[DUB_PATH];
        DubCopy(cur, DUB_PATH, recordingFolder);
        DubClean(cur);
        for (int guard = 0; guard < 64; guard++) {
            wchar_t parent[DUB_PATH];
            DubDirName(cur, parent, DUB_PATH);
            if (_wcsicmp(parent, root) == 0) { DubCopy(out, cap, DubBaseName(cur)); return; }
            if (wcscmp(parent, cur) == 0) break;
            DubCopy(cur, DUB_PATH, parent);
        }
    }
    /* folders outside Choicer Voicer's normal root: the parent folder's name */
    wchar_t rec[DUB_PATH], par[DUB_PATH];
    DubCopy(rec, DUB_PATH, recordingFolder); DubClean(rec);
    DubDirName(rec, par, DUB_PATH);
    DubCopy(out, cap, DubBaseName(par));
}

/* the _dubrecord_ clip names (without prefix/extension) in an export, de-duplicated */
static wchar_t** DubClipBaseNames(const wchar_t* dir, int* count)
{
    *count = 0;
    int n = 0;
    DubEnt* e = DubListDir(dir, &n);
    if (!e) return NULL;
    wchar_t** names = (wchar_t**)calloc(n > 0 ? n : 1, sizeof(wchar_t*));
    int c = 0;
    for (int i = 0; names && i < n; i++) {
        if (DUB_ENT_IS_DIR(&e[i]) || !DubIsRecordingWav(e[i].name)) continue;
        wchar_t base[MAX_PATH];
        DubBaseNoExt(e[i].name + wcslen(L"_dubrecord_"), base, MAX_PATH);
        BOOL dup = FALSE;
        for (int k = 0; k < c; k++) if (_wcsicmp(names[k], base) == 0) { dup = TRUE; break; }
        if (!dup) names[c++] = _wcsdup(base);
    }
    free(e);
    *count = c;
    return names;
}
static void DubFreeNames(wchar_t** names, int count)
{
    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
}

/* how many clip names have a matching .txt/.ini in the pack folder */
static int DubMetadataMatchCount(const wchar_t* pack, wchar_t** clips, int clipCount)
{
    if (clipCount == 0) return 0;
    int n = 0, matches = 0;
    DubEnt* e = DubListDir(pack, &n);
    if (!e) return 0;
    for (int c = 0; c < clipCount; c++) {
        for (int i = 0; i < n; i++) {
            if (DUB_ENT_IS_DIR(&e[i])) continue;
            if (!DubEndsWithI(e[i].name, L".txt") && !DubEndsWithI(e[i].name, L".ini")) continue;
            wchar_t base[MAX_PATH];
            DubBaseNoExt(e[i].name, base, MAX_PATH);
            if (_wcsicmp(base, clips[c]) == 0) { matches++; break; }
        }
    }
    free(e);
    return matches;
}

typedef struct { wchar_t path[DUB_PATH]; long score; } DubPackCand;
typedef struct {
    DubPackCand* a; int n, cap;
    wchar_t** clips; int clipCount;
    DubParts parts;
} DubPackSearch;

static void DubSearchPacks(const wchar_t* dir, int depth, DubPackSearch* s)
{
    if (depth > 12) return;
    if (DubValidPackFolder(dir)) {
        int matches = DubMetadataMatchCount(dir, s->clips, s->clipCount);
        long score = (long)matches * 1000;
        /* a complete metadata-name match is overwhelmingly likely to be the right nested pack */
        if (s->clipCount > 0 && matches == s->clipCount) score += 100000;
        /* reward an exact folder-name match with any ancestor under the recording collection */
        const wchar_t* base = DubBaseName(dir);
        for (int i = 0; i < s->parts.count; i++) {
            if (_wcsicmp(base, s->parts.part[i]) == 0) score += 500 + i;
        }
        if (s->n == s->cap) {
            int nc = s->cap ? s->cap * 2 : 16;
            DubPackCand* na = (DubPackCand*)realloc(s->a, nc * sizeof(DubPackCand));
            if (!na) return;
            s->a = na; s->cap = nc;
        }
        DubCopy(s->a[s->n].path, DUB_PATH, dir);
        s->a[s->n].score = score;
        s->n++;
        return;     /* a valid pack doesn't contain another independent pack */
    }
    int n = 0;
    DubEnt* e = DubListDir(dir, &n);
    for (int i = 0; i < n; i++) {
        if (!DUB_ENT_IS_DIR(&e[i]) || (e[i].attrs & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        wchar_t sub[DUB_PATH];
        DubJoin(sub, DUB_PATH, dir, e[i].name);
        DubSearchPacks(sub, depth + 1, s);
    }
    free(e);
}

static int DubPackCandCmp(const void* a, const void* b)
{
    const DubPackCand* x = (const DubPackCand*)a;
    const DubPackCand* y = (const DubPackCand*)b;
    if (x->score != y->score) return x->score > y->score ? -1 : 1;
    return _wcsicmp(x->path, y->path);
}

/* finds the original pack for an export folder; returns FALSE if none */
static BOOL DubFindMatchingPack(const wchar_t* recordingFolder, wchar_t* out, size_t cap)
{
    wchar_t groupName[MAX_PATH];
    DubPackNameFromExport(recordingFolder, groupName, MAX_PATH);
    if (!groupName[0]) return FALSE;

    wchar_t packsVoice[DUB_PATH], groupRoot[DUB_PATH];
    if (!DubAppPath(packsVoice, DUB_PATH, L"packs_voice")) return FALSE;
    DubJoin(groupRoot, DUB_PATH, packsVoice, groupName);

    /* some packs keep all content directly inside the top-level pack folder */
    if (DubValidPackFolder(groupRoot)) { DubCopy(out, cap, groupRoot); return TRUE; }
    if (!DubIsDir(groupRoot)) return FALSE;

    /* nested packs normally mirror the recordings hierarchy:
         recordings\dub_recordings\Big Pack\Episode\2026-...\_dubrecord_*.wav
         packs_voice\Big Pack\Episode\dub_video.ogv
       try that mirrored path from deepest to shallowest first */
    wchar_t recRoot[DUB_PATH], recGroupRoot[DUB_PATH];
    DubAppPath(recRoot, DUB_PATH, L"recordings\\dub_recordings");
    DubJoin(recGroupRoot, DUB_PATH, recRoot, groupName);
    DubParts parts;
    DubPartsBelow(recGroupRoot, recordingFolder, &parts);
    for (int count = parts.count; count >= 1; count--) {
        wchar_t cand[DUB_PATH];
        DubCopy(cand, DUB_PATH, groupRoot);
        for (int i = 0; i < count; i++) {
            wchar_t next[DUB_PATH];
            DubJoin(next, DUB_PATH, cand, parts.part[i]);
            DubCopy(cand, DUB_PATH, next);
        }
        if (DubValidPackFolder(cand)) { DubCopy(out, cap, cand); return TRUE; }
    }

    /* layouts differ: search every nested valid pack and score by how many
       exported clip names have matching metadata files */
    DubPackSearch s;
    memset(&s, 0, sizeof(s));
    s.parts = parts;
    s.clips = DubClipBaseNames(recordingFolder, &s.clipCount);
    DubSearchPacks(groupRoot, 0, &s);
    BOOL found = FALSE;
    if (s.n > 0) {
        qsort(s.a, s.n, sizeof(DubPackCand), DubPackCandCmp);
        if (s.a[0].score > 0 || s.n == 1) { DubCopy(out, cap, s.a[0].path); found = TRUE; }
    }
    free(s.a);
    DubFreeNames(s.clips, s.clipCount);
    return found;
}

/* ---------------- export scan (worker thread) ---------------- */

typedef struct { DubExport* a; int n, cap; } DubExportList;

static void DubWalkForExports(const wchar_t* dir, int depth, DubExportList* L)
{
    if (depth > 24) return;
    int n = 0;
    DubEnt* e = DubListDir(dir, &n);
    if (!e) return;
    BOOL hasWav = FALSE;
    for (int i = 0; i < n; i++) {
        if (!DUB_ENT_IS_DIR(&e[i]) && DubIsRecordingWav(e[i].name)) { hasWav = TRUE; break; }
    }
    if (hasWav) {
        if (L->n == L->cap) {
            int nc = L->cap ? L->cap * 2 : 64;
            DubExport* na = (DubExport*)realloc(L->a, nc * sizeof(DubExport));
            if (na) { L->a = na; L->cap = nc; }
        }
        if (L->n < L->cap) {
            DubExport* x = &L->a[L->n++];
            memset(x, 0, sizeof(*x));
            DubCopy(x->path, DUB_PATH, dir);
            DubClean(x->path);
        }
    }
    for (int i = 0; i < n; i++) {
        if (!DUB_ENT_IS_DIR(&e[i]) || (e[i].attrs & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        wchar_t sub[DUB_PATH];
        DubJoin(sub, DUB_PATH, dir, e[i].name);
        DubWalkForExports(sub, depth + 1, L);
    }
    free(e);
}

static int DubExportCmp(const void* a, const void* b)
{
    const DubExport* x = (const DubExport*)a;
    const DubExport* y = (const DubExport*)b;
    LONG c = CompareFileTime(&x->mod, &y->mod);
    if (c != 0) return c > 0 ? -1 : 1;     /* newest first */
    return _wcsicmp(x->path, y->path);
}

static DWORD WINAPI DubScanThread(LPVOID unused)
{
    (void)unused;
    DubExportList L = { NULL, 0, 0 };
    wchar_t root[DUB_PATH];
    if (DubAppPath(root, DUB_PATH, L"recordings\\dub_recordings")) DubWalkForExports(root, 0, &L);

    for (int i = 0; i < L.n; i++) {
        WIN32_FILE_ATTRIBUTE_DATA d;
        if (GetFileAttributesExW(L.a[i].path, GetFileExInfoStandard, &d)) L.a[i].mod = d.ftLastWriteTime;
        wchar_t pack[MAX_PATH];
        DubPackNameFromExport(L.a[i].path, pack, MAX_PATH);
        FILETIME lft; SYSTEMTIME st;
        FileTimeToLocalFileTime(&L.a[i].mod, &lft);
        FileTimeToSystemTime(&lft, &st);
        _snwprintf(L.a[i].display, DUB_PATH + 255, L"%ls  |  %ls  |  %04d-%02d-%02d %02d:%02d:%02d",
            pack, DubBaseName(L.a[i].path), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        L.a[i].display[DUB_PATH + 255] = 0;
    }
    if (L.n > 1) qsort(L.a, L.n, sizeof(DubExport), DubExportCmp);

    DubScanResult* r = (DubScanResult*)malloc(sizeof(*r));
    if (r) { r->items = L.a; r->count = L.n; }
    else free(L.a);
    DubPost(WM_DUB_SCANDONE, 0, (LPARAM)r, FALSE);
    if (!r && g_hDubWnd) PostMessageW(g_hDubWnd, WM_DUB_SCANDONE, 0, 0);
    return 0;
}

/* ---------------- running child processes ---------------- */

/* turns raw process output bytes into a log line (UTF-8, falling back to the OEM code page) */
static void DubEmitLine(const char* bytes, size_t n, BOOL log, BOOL statusUpdates,
                        wchar_t* capture, size_t captureCap, DWORD* lastStatTick)
{
    if (n == 0) return;
    wchar_t w[4096];
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, (int)n, w, 4095);
    if (len <= 0) len = MultiByteToWideChar(CP_OEMCP, 0, bytes, (int)n, w, 4095);
    if (len <= 0) return;
    w[len] = 0;
    /* trim */
    wchar_t* s = w;
    while (*s && iswspace(*s)) s++;
    size_t sl = wcslen(s);
    while (sl > 0 && iswspace(s[sl - 1])) s[--sl] = 0;
    if (sl <= 1) return;        /* spinner fragments and blanks */

    if (capture && captureCap > 1) {
        size_t cl = wcslen(capture);
        if (cl + 3 < captureCap) {
            if (cl) { capture[cl++] = L' '; capture[cl++] = L'|'; capture[cl++] = L' '; }
            size_t room = captureCap - 1 - cl;
            wcsncpy(capture + cl, s, room);
            capture[cl + (sl < room ? sl : room)] = 0;
        }
    }

    BOOL isStat;
    isStat = (wcsncmp(s, L"frame=", 6) == 0 || wcsncmp(s, L"size=", 5) == 0 ||
              wcschr(s, 0x2588) != NULL || wcschr(s, 0x2592) != NULL);

    if (statusUpdates) {
        const wchar_t* t = wcsstr(s, L"time=");
        if (t) {
            wchar_t tm[16];
            _snwprintf(tm, 15, L"%.11ls", t + 5);
            tm[15] = 0;
            wchar_t msg[160];
            _snwprintf(msg, 159, L"Rendering in the background...  %ls  (%ls encoded)", g_dubStage, tm);
            msg[159] = 0;
            DubPostText(WM_DUB_STATUS, DUBC_ACCENT, msg);
        }
    }
    if (!log) return;
    if (isStat) {
        /* progress lines arrive many times a second - keep the log readable */
        DWORD now = GetTickCount();
        if (lastStatTick && now - *lastStatTick < 2000) return;
        if (lastStatTick) *lastStatTick = now;
    }
    DubLog(s);
}

/* Runs exe with the given (already quoted) argument string, hidden, merging
   stdout+stderr. Returns the exit code, or (DWORD)-1 if it couldn't start.
   cancelable: registers the process so the Cancel button can kill it. */
static DWORD DubRunProcess(const wchar_t* exe, const wchar_t* args, BOOL log, BOOL cancelable,
                           wchar_t* capture, size_t captureCap, BOOL statusUpdates)
{
    if (capture && captureCap) capture[0] = 0;
    SECURITY_ATTRIBUTES sa; sa.nLength = sizeof(sa); sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = TRUE;
    HANDLE hRead = NULL, hWrite = NULL;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return (DWORD)-1;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
    HANDLE hNul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);

    DubSb cmd; DubSbInit(&cmd);
    DubSbAppendCh(&cmd, L'"'); DubSbAppend(&cmd, exe); DubSbAppendCh(&cmd, L'"');
    if (args && *args) { DubSbAppendCh(&cmd, L' '); DubSbAppend(&cmd, args); }

    STARTUPINFOW si; memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = (hNul != INVALID_HANDLE_VALUE) ? hNul : NULL;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));

    BOOL ok = cmd.p && CreateProcessW(NULL, cmd.p, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    DubSbFree(&cmd);
    CloseHandle(hWrite);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
    if (!ok) { CloseHandle(hRead); return (DWORD)-1; }
    CloseHandle(pi.hThread);

    if (cancelable) {
        EnterCriticalSection(&g_dubProcCs);
        g_dubProc = pi.hProcess;
        LeaveCriticalSection(&g_dubProcCs);
        if (InterlockedCompareExchange(&g_dubCancel, 0, 0)) TerminateProcess(pi.hProcess, 1);
    }

    char buf[4096];
    char line[8192];
    size_t lineLen = 0;
    DWORD lastStat = 0;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(hRead, NULL, 0, NULL, &avail, NULL)) break;      /* all writers closed */
        if (avail) {
            DWORD got = 0;
            if (!ReadFile(hRead, buf, sizeof(buf), &got, NULL) || got == 0) break;
            for (DWORD i = 0; i < got; i++) {
                char c = buf[i];
                if (c == '\n' || c == '\r') {
                    DubEmitLine(line, lineLen, log, statusUpdates, capture, captureCap, &lastStat);
                    lineLen = 0;
                } else {
                    line[lineLen++] = c;
                    if (lineLen == sizeof(line)) { DubEmitLine(line, lineLen, log, statusUpdates, capture, captureCap, &lastStat); lineLen = 0; }
                }
            }
        } else if (WaitForSingleObject(pi.hProcess, 40) == WAIT_OBJECT_0) {
            DWORD more = 0;
            if (!PeekNamedPipe(hRead, NULL, 0, NULL, &more, NULL) || more == 0) break;
        }
    }
    DubEmitLine(line, lineLen, log, statusUpdates, capture, captureCap, &lastStat);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (cancelable) {
        EnterCriticalSection(&g_dubProcCs);
        g_dubProc = NULL;
        LeaveCriticalSection(&g_dubProcCs);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(hRead);
    return code;
}

static void DubKillRunningProcess(void)
{
    if (!g_dubProcCsInit) return;
    EnterCriticalSection(&g_dubProcCs);
    if (g_dubProc) TerminateProcess(g_dubProc, 1);
    LeaveCriticalSection(&g_dubProcCs);
}

/* ---------------- FFmpeg discovery ---------------- */

typedef struct { wchar_t path[DUB_PATH]; wchar_t source[48]; int priority; } DubFfCand;
typedef struct { DubFfCand* a; int n, cap; } DubFfList;

/* resolves symlinks (WinGet "Links" are symlinks) and returns FALSE if the file doesn't exist */
static BOOL DubCanonicalExe(const wchar_t* path, wchar_t* out, size_t cap)
{
    if (!path || !*path) return FALSE;
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return FALSE;
    wchar_t full[DUB_PATH];
    DWORD fl = GetFullPathNameW(path, DUB_PATH, full, NULL);
    if (fl == 0 || fl >= DUB_PATH) DubCopy(full, DUB_PATH, path);
    HANDLE h = CreateFileW(full, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        wchar_t fin[DUB_PATH];
        DWORD n = GetFinalPathNameByHandleW(h, fin, DUB_PATH, VOLUME_NAME_DOS);
        CloseHandle(h);
        if (n > 0 && n < DUB_PATH) {
            if (wcsncmp(fin, L"\\\\?\\UNC\\", 8) == 0) _snwprintf(full, DUB_PATH - 1, L"\\\\%ls", fin + 8);
            else if (wcsncmp(fin, L"\\\\?\\", 4) == 0) DubCopy(full, DUB_PATH, fin + 4);
            else DubCopy(full, DUB_PATH, fin);
            full[DUB_PATH - 1] = 0;
        }
    }
    DubClean(full);
    DubCopy(out, cap, full);
    return TRUE;
}

static void DubAddFfCand(DubFfList* L, const wchar_t* path, const wchar_t* source, int priority)
{
    wchar_t clean[DUB_PATH];
    if (!DubCanonicalExe(path, clean, DUB_PATH)) return;
    for (int i = 0; i < L->n; i++) {
        if (_wcsicmp(L->a[i].path, clean) == 0) {
            if (priority < L->a[i].priority) { L->a[i].priority = priority; DubCopy(L->a[i].source, 48, source); }
            return;
        }
    }
    if (L->n == L->cap) {
        int nc = L->cap ? L->cap * 2 : 16;
        DubFfCand* na = (DubFfCand*)realloc(L->a, nc * sizeof(DubFfCand));
        if (!na) return;
        L->a = na; L->cap = nc;
    }
    DubCopy(L->a[L->n].path, DUB_PATH, clean);
    DubCopy(L->a[L->n].source, 48, source);
    L->a[L->n].priority = priority;
    L->n++;
}

static void DubFindFfmpegExeIn(const wchar_t* dir, int depth, DubFfList* L, const wchar_t* source, int priority)
{
    if (depth > 10) return;
    int n = 0;
    DubEnt* e = DubListDir(dir, &n);
    for (int i = 0; i < n; i++) {
        wchar_t p[DUB_PATH];
        DubJoin(p, DUB_PATH, dir, e[i].name);
        if (DUB_ENT_IS_DIR(&e[i])) {
            if (!(e[i].attrs & FILE_ATTRIBUTE_REPARSE_POINT)) DubFindFfmpegExeIn(p, depth + 1, L, source, priority);
        } else if (_wcsicmp(e[i].name, L"ffmpeg.exe") == 0) {
            DubAddFfCand(L, p, source, priority);
        }
    }
    free(e);
}

static void DubCollectFfPackages(const wchar_t* root, DubFfList* L)
{
    if (!root || !*root) return;
    int n = 0;
    DubEnt* e = DubListDir(root, &n);
    for (int i = 0; i < n; i++) {
        if (!DUB_ENT_IS_DIR(&e[i])) continue;
        wchar_t lower[MAX_PATH];
        DubCopy(lower, MAX_PATH, e[i].name);
        _wcslwr(lower);
        if (!wcsstr(lower, L"ffmpeg")) continue;
        int priority = 14;
        const wchar_t* source = L"WinGet FFmpeg package";
        if (wcsstr(lower, L"gyan.ffmpeg")) { priority = 10; source = L"WinGet Gyan.FFmpeg package"; }
        wchar_t dir[DUB_PATH];
        DubJoin(dir, DUB_PATH, root, e[i].name);
        DubFindFfmpegExeIn(dir, 0, L, source, priority);
    }
    free(e);
}

static int DubFfCandCmp(const void* a, const void* b)
{
    const DubFfCand* x = (const DubFfCand*)a;
    const DubFfCand* y = (const DubFfCand*)b;
    if (x->priority != y->priority) return x->priority < y->priority ? -1 : 1;
    return _wcsicmp(x->path, y->path);
}

static void DubEnvPath(const wchar_t* var, wchar_t* out, size_t cap)
{
    out[0] = 0;
    DWORD n = GetEnvironmentVariableW(var, out, (DWORD)cap);
    if (n == 0 || n >= cap) out[0] = 0;
}

static void DubCollectFfmpeg(DubFfList* L)
{
    wchar_t exeDir[DUB_PATH] = L"";
    wchar_t self[DUB_PATH];
    DWORD sl = GetModuleFileNameW(NULL, self, DUB_PATH);
    if (sl > 0 && sl < DUB_PATH) DubDirName(self, exeDir, DUB_PATH);

    wchar_t p[DUB_PATH], q[DUB_PATH];
    /* highest priority: a portable FFmpeg intentionally placed beside the launcher */
    if (exeDir[0]) {
        DubJoin(p, DUB_PATH, exeDir, L"ffmpeg.exe");
        DubAddFfCand(L, p, L"Beside launcher", 0);
        DubJoin(q, DUB_PATH, exeDir, L"bin");
        DubJoin(p, DUB_PATH, q, L"ffmpeg.exe");
        DubAddFfCand(L, p, L"Launcher bin folder", 1);
    }

    wchar_t localAppData[DUB_PATH], programFiles[DUB_PATH], programFilesX86[DUB_PATH], userProfile[DUB_PATH], programData[DUB_PATH];
    DubEnvPath(L"LOCALAPPDATA", localAppData, DUB_PATH);
    DubEnvPath(L"ProgramFiles", programFiles, DUB_PATH);
    DubEnvPath(L"ProgramFiles(x86)", programFilesX86, DUB_PATH);
    DubEnvPath(L"USERPROFILE", userProfile, DUB_PATH);
    DubEnvPath(L"ProgramData", programData, DUB_PATH);

    /* prefer compatible WinGet/Gyan copies over arbitrary PATH installations */
    const wchar_t* roots[3] = { localAppData, programFiles, programFilesX86 };
    const wchar_t* linkNames[3] = { L"WinGet user link", L"WinGet machine link", L"WinGet x86 link" };
    for (int i = 0; i < 3; i++) {
        if (!roots[i][0]) continue;
        wchar_t base[DUB_PATH];
        if (i == 0) { DubJoin(q, DUB_PATH, roots[i], L"Microsoft"); DubJoin(base, DUB_PATH, q, L"WinGet"); }
        else DubJoin(base, DUB_PATH, roots[i], L"WinGet");
        DubJoin(q, DUB_PATH, base, L"Links");
        DubJoin(p, DUB_PATH, q, L"ffmpeg.exe");
        DubAddFfCand(L, p, linkNames[i], 10);
    }

    /* every PATH entry, not just the first hit */
    wchar_t* pathVar = (wchar_t*)malloc(32768 * sizeof(wchar_t));
    if (pathVar) {
        DubEnvPath(L"PATH", pathVar, 32768);
        wchar_t* cur = pathVar;
        while (cur && *cur) {
            wchar_t* semi = wcschr(cur, L';');
            if (semi) *semi = 0;
            wchar_t* tok = cur;
            cur = semi ? semi + 1 : NULL;
            while (*tok == L' ' || *tok == L'"') tok++;
            size_t tl = wcslen(tok);
            while (tl > 0 && (tok[tl - 1] == L' ' || tok[tl - 1] == L'"')) tok[--tl] = 0;
            if (!*tok) continue;
            DubJoin(p, DUB_PATH, tok, L"ffmpeg.exe");
            DubAddFfCand(L, p, L"Windows PATH", 30);
        }
        free(pathVar);
    }

    if (userProfile[0]) {
        wchar_t a1[DUB_PATH], a2[DUB_PATH], a3[DUB_PATH], a4[DUB_PATH];
        DubJoin(a1, DUB_PATH, userProfile, L"scoop");
        DubJoin(a2, DUB_PATH, a1, L"apps");
        DubJoin(a3, DUB_PATH, a2, L"ffmpeg");
        DubJoin(a4, DUB_PATH, a3, L"current");
        DubJoin(a1, DUB_PATH, a4, L"bin");
        DubJoin(p, DUB_PATH, a1, L"ffmpeg.exe");
        DubAddFfCand(L, p, L"Scoop", 20);
    }
    if (programData[0]) {
        DubJoin(q, DUB_PATH, programData, L"chocolatey");
        DubJoin(p, DUB_PATH, q, L"bin");
        DubJoin(q, DUB_PATH, p, L"ffmpeg.exe");
        DubAddFfCand(L, q, L"Chocolatey", 20);
    }

    for (int i = 0; i < 3; i++) {
        if (!roots[i][0]) continue;
        wchar_t pk[DUB_PATH], base[DUB_PATH];
        if (i == 0) { DubJoin(q, DUB_PATH, roots[i], L"Microsoft"); DubJoin(base, DUB_PATH, q, L"WinGet"); }
        else DubJoin(base, DUB_PATH, roots[i], L"WinGet");
        DubJoin(pk, DUB_PATH, base, L"Packages");
        DubCollectFfPackages(pk, L);
    }

    if (L->n > 1) qsort(L->a, L->n, sizeof(DubFfCand), DubFfCandCmp);
}

/* runs a tiny amix job (the renderer needs amix's normalize option, which old FFmpeg builds lack) */
static BOOL DubVerifyFfmpeg(const wchar_t* path, wchar_t* errOut, size_t errCap)
{
    if (errOut && errCap) errOut[0] = 0;
    DubSb a; DubSbInit(&a);
    const wchar_t* args[] = {
        L"-hide_banner", L"-loglevel", L"error",
        L"-f", L"lavfi", L"-i", L"anullsrc=r=48000:cl=stereo",
        L"-f", L"lavfi", L"-i", L"anullsrc=r=48000:cl=stereo",
        L"-filter_complex", L"[0:a][1:a]amix=inputs=2:duration=shortest:normalize=0[a]",
        L"-map", L"[a]", L"-t", L"0.02", L"-f", L"null", L"NUL"
    };
    for (size_t i = 0; i < sizeof(args) / sizeof(args[0]); i++) DubAppendArg(&a, args[i]);
    wchar_t cap[512];
    DWORD code = a.p ? DubRunProcess(path, a.p, FALSE, FALSE, cap, 512, FALSE) : (DWORD)-1;
    DubSbFree(&a);
    if (code == 0) return TRUE;
    if (errOut && errCap) {
        if (code == (DWORD)-1) DubCopy(errOut, errCap, L"FFmpeg could not be started");
        else if (cap[0]) _snwprintf(errOut, errCap - 1, L"%ls", cap);
        else _snwprintf(errOut, errCap - 1, L"exit code %lu", (unsigned long)code);
        errOut[errCap - 1] = 0;
    }
    return FALSE;
}

/* returns 1 and fills outPath if a compatible FFmpeg exists; 0 if none were
   found at all; 2 if some were found but none are compatible */
static int DubScanAndSelectFfmpeg(BOOL logDetails, wchar_t* outPath, size_t cap)
{
    DubFfList L = { NULL, 0, 0 };
    DubCollectFfmpeg(&L);
    if (logDetails) {
        if (L.n == 0) DubLog(L"No FFmpeg installations were detected.");
        else DubLogF(L"Detected %d FFmpeg installation%ls.%ls", L.n, L.n == 1 ? L"" : L"s", L.n > 1 ? L" Checking each copy." : L"");
    }
    int result = L.n == 0 ? 0 : 2;
    for (int i = 0; i < L.n; i++) {
        wchar_t err[512];
        BOOL ok = DubVerifyFfmpeg(L.a[i].path, err, 512);
        if (logDetails) {
            if (ok) { DubLogF(L"Compatible: %ls", L.a[i].path); DubLogF(L"Source: %ls", L.a[i].source); }
            else    { DubLogF(L"Rejected: %ls", L.a[i].path); DubLogF(L"Reason: %ls", err); }
        }
        if (ok) { DubCopy(outPath, cap, L.a[i].path); result = 1; break; }
    }
    free(L.a);
    return result;
}

static DWORD WINAPI DubFfCheckThread(LPVOID unused)
{
    (void)unused;
    wchar_t path[DUB_PATH] = L"";
    int r = DubScanAndSelectFfmpeg(TRUE, path, DUB_PATH);
    DubPostText(WM_DUB_FFCHECK, (WPARAM)r, r == 1 ? path : L"");
    return 0;
}

static BOOL DubFindWinget(wchar_t* out, size_t cap)
{
    wchar_t found[DUB_PATH];
    if (SearchPathW(NULL, L"winget.exe", NULL, DUB_PATH, found, NULL)) { DubCopy(out, cap, found); return TRUE; }
    wchar_t lad[DUB_PATH], pf[DUB_PATH], a[DUB_PATH], b[DUB_PATH];
    DubEnvPath(L"LOCALAPPDATA", lad, DUB_PATH);
    DubEnvPath(L"ProgramFiles", pf, DUB_PATH);
    if (lad[0]) {
        DubJoin(a, DUB_PATH, lad, L"Microsoft");
        DubJoin(b, DUB_PATH, a, L"WindowsApps");
        DubJoin(a, DUB_PATH, b, L"winget.exe");
        if (DubCanonicalExe(a, found, DUB_PATH)) { DubCopy(out, cap, a); return TRUE; }
        DubJoin(a, DUB_PATH, lad, L"Microsoft");
        DubJoin(b, DUB_PATH, a, L"WinGet");
        DubJoin(a, DUB_PATH, b, L"winget.exe");
        if (DubCanonicalExe(a, found, DUB_PATH)) { DubCopy(out, cap, a); return TRUE; }
    }
    if (pf[0]) {
        DubJoin(a, DUB_PATH, pf, L"WindowsApps");
        DubJoin(b, DUB_PATH, a, L"Microsoft.DesktopAppInstaller_8wekyb3d8bbwe");
        DubJoin(a, DUB_PATH, b, L"winget.exe");
        if (DubCanonicalExe(a, found, DUB_PATH)) { DubCopy(out, cap, a); return TRUE; }
    }
    return FALSE;
}

static BOOL DubRunLogged(const wchar_t* stage, const wchar_t* exe, const wchar_t* const* argv, int argc)
{
    DubSb a; DubSbInit(&a);
    for (int i = 0; i < argc; i++) DubAppendArg(&a, argv[i]);
    DubLogF(L"\r\n========== %ls ==========", stage);
    DubLogF(L"%ls %ls", exe, a.p ? a.p : L"");
    DWORD code = a.p ? DubRunProcess(exe, a.p, TRUE, FALSE, NULL, 0, FALSE) : (DWORD)-1;
    DubSbFree(&a);
    if (code != 0) DubLogF(L"%ls did not complete (exit code %ld).", stage, (long)(int)code);
    return code == 0;
}

/* Replaces WinGet's Gyan FFmpeg packages with the current compatible build. Only
   ever touches the two known Gyan package IDs, never arbitrary FFmpeg installs. */
static DWORD WINAPI DubFfInstallThread(LPVOID unused)
{
    (void)unused;
    wchar_t found[DUB_PATH] = L"";
    if (DubScanAndSelectFfmpeg(FALSE, found, DUB_PATH) == 1) { DubPostText(WM_DUB_FFINSTALL, 1, found); return 0; }

    wchar_t winget[DUB_PATH];
    if (!DubFindWinget(winget, DUB_PATH)) {
        DubPostText(WM_DUB_FFINSTALL, 0,
            L"Windows Package Manager (winget) was not found. Install or update \"App Installer\" from the Microsoft Store, "
            L"or place ffmpeg.exe next to the launcher.");
        return 0;
    }
    DubLog(L"No compatible FFmpeg installation was found.");
    DubLog(L"Replacing WinGet Gyan FFmpeg packages with the latest compatible build...");

    const wchar_t* un1[] = { L"uninstall", L"--id", L"Gyan.FFmpeg", L"--exact", L"--purge", L"--force", L"--silent", L"--disable-interactivity" };
    const wchar_t* un2[] = { L"uninstall", L"--id", L"Gyan.FFmpeg.Shared", L"--exact", L"--purge", L"--force", L"--silent", L"--disable-interactivity" };
    const wchar_t* inst[] = { L"install", L"--id", L"Gyan.FFmpeg", L"--exact", L"--source", L"winget",
        L"--accept-package-agreements", L"--accept-source-agreements", L"--silent", L"--disable-interactivity", L"--force" };
    DubRunLogged(L"Remove old Gyan.FFmpeg", winget, un1, 8);
    DubRunLogged(L"Remove old Gyan.FFmpeg.Shared", winget, un2, 8);
    BOOL installed = DubRunLogged(L"Install compatible FFmpeg", winget, inst, 11);

    for (int attempt = 0; attempt < (installed ? 20 : 1); attempt++) {
        if (DubScanAndSelectFfmpeg(FALSE, found, DUB_PATH) == 1) { DubPostText(WM_DUB_FFINSTALL, 1, found); return 0; }
        if (installed) Sleep(500);
    }
    DubPostText(WM_DUB_FFINSTALL, 0, installed
        ? L"winget finished, but a compatible FFmpeg was not detected afterwards."
        : L"winget could not install FFmpeg. See the activity log for details.");
    return 0;
}

/* ---------------- voice clip analysis (mono-mic centering) ---------------- */

static double DubDecodeSample(const unsigned char* d, size_t off, size_t len, unsigned fmt, unsigned bits, BOOL* ok)
{
    *ok = TRUE;
    if (fmt == 1) {
        if (bits == 8  && off + 1 <= len) return ((double)d[off] - 128.0) / 128.0;
        if (bits == 16 && off + 2 <= len) return (double)(short)(d[off] | (d[off + 1] << 8)) / 32768.0;
        if (bits == 24 && off + 3 <= len) {
            int v = d[off] | (d[off + 1] << 8) | (d[off + 2] << 16);
            if (v & 0x800000) v |= ~0xFFFFFF;
            return (double)v / 8388608.0;
        }
        if (bits == 32 && off + 4 <= len) {
            int v = (int)((unsigned)d[off] | ((unsigned)d[off + 1] << 8) | ((unsigned)d[off + 2] << 16) | ((unsigned)d[off + 3] << 24));
            return (double)v / 2147483648.0;
        }
    } else if (fmt == 3) {
        if (bits == 32 && off + 4 <= len) {
            unsigned u = (unsigned)d[off] | ((unsigned)d[off + 1] << 8) | ((unsigned)d[off + 2] << 16) | ((unsigned)d[off + 3] << 24);
            float f; memcpy(&f, &u, 4);
            return (double)f;
        }
        if (bits == 64 && off + 8 <= len) {
            double f; memcpy(&f, d + off, 8);
            return f;
        }
    }
    *ok = FALSE;
    return 0;
}

static double DubAmplitudeDb(double v) { return v <= 0.000001 ? -120.0 : 20.0 * log10(v); }

/* RMS level of each side of a 2-channel PCM/float WAV; FALSE for anything else */
static BOOL DubWavSideLevels(const wchar_t* path, double* leftDb, double* rightDb)
{
    size_t len = 0;
    unsigned char* c = (unsigned char*)DubReadFile(path, &len);
    if (!c) return FALSE;
    BOOL result = FALSE;
    if (len < 12 || memcmp(c, "RIFF", 4) != 0 || memcmp(c + 8, "WAVE", 4) != 0) { free(c); return FALSE; }

    unsigned fmt = 0, channels = 0, bits = 0, blockAlign = 0;
    const unsigned char* audio = NULL;
    size_t audioLen = 0;
    for (size_t off = 12; off + 8 <= len; ) {
        size_t csz = (size_t)c[off + 4] | ((size_t)c[off + 5] << 8) | ((size_t)c[off + 6] << 16) | ((size_t)c[off + 7] << 24);
        size_t start = off + 8;
        size_t end = start + csz;
        if (end > len) end = len;
        if (memcmp(c + off, "fmt ", 4) == 0) {
            if (end - start < 16) goto done;
            fmt = c[start] | (c[start + 1] << 8);
            channels = c[start + 2] | (c[start + 3] << 8);
            blockAlign = c[start + 12] | (c[start + 13] << 8);
            bits = c[start + 14] | (c[start + 15] << 8);
            if (fmt == 0xFFFE && end - start >= 26) fmt = c[start + 24] | (c[start + 25] << 8);
        } else if (memcmp(c + off, "data", 4) == 0) {
            audio = c + start; audioLen = end - start;
        }
        off = start + csz;
        if (csz % 2) off++;
    }
    if (channels != 2 || blockAlign == 0 || !audio || audioLen == 0) goto done;
    {
        unsigned bps = bits / 8;
        if (bps == 0 || blockAlign < bps * 2) goto done;
        size_t frames = audioLen / blockAlign;
        if (frames == 0) goto done;
        double sq[2] = { 0, 0 };
        for (size_t i = 0; i < frames; i++) {
            for (unsigned ch = 0; ch < 2; ch++) {
                BOOL ok;
                double v = DubDecodeSample(audio, i * blockAlign + ch * bps, audioLen, fmt, bits, &ok);
                if (!ok) goto done;
                sq[ch] += v * v;
            }
        }
        *leftDb = DubAmplitudeDb(sqrt(sq[0] / (double)frames));
        *rightDb = DubAmplitudeDb(sqrt(sq[1] / (double)frames));
        result = TRUE;
    }
done:
    free(c);
    return result;
}

/* a pan-filter suffix only when a stereo WAV is strongly one-sided (a mono mic recorded
   into one channel); normal stereo recordings keep their image */
static const wchar_t* DubCenteringSuffix(const wchar_t* path, BOOL* corrected)
{
    *corrected = FALSE;
    double l, r;
    if (!DubWavSideLevels(path, &l, &r)) return L"";
    const double minUseful = -90.0, imbalance = 18.0;
    if (l > minUseful && l - r >= imbalance) { *corrected = TRUE; return L",pan=stereo|c0=c0|c1=c0"; }
    if (r > minUseful && r - l >= imbalance) { *corrected = TRUE; return L",pan=stereo|c0=c1|c1=c1"; }
    return L"";
}

/* ---------------- render ---------------- */

typedef struct { wchar_t* wav; double ts; } DubPlacement;

typedef struct {
    wchar_t recording[DUB_PATH], pack[DUB_PATH], output[DUB_PATH], ffmpeg[DUB_PATH];
} DubRenderJob;

/* finds <base>.<ext> in folder, preferring the given extensions in order */
static BOOL DubFindFileByBase(const wchar_t* folder, const wchar_t* base, const wchar_t* const* prefExts, int prefCount,
                              wchar_t* out, size_t cap)
{
    out[0] = 0;
    int n = 0;
    DubEnt* e = DubListDir(folder, &n);
    if (!e) return FALSE;
    int firstMatch = -1;
    for (int i = 0; i < n; i++) {
        if (DUB_ENT_IS_DIR(&e[i])) continue;
        wchar_t b[MAX_PATH];
        DubBaseNoExt(e[i].name, b, MAX_PATH);
        if (_wcsicmp(b, base) == 0 && firstMatch < 0) firstMatch = i;
    }
    int pick = -1;
    for (int p = 0; p < prefCount && pick < 0; p++) {
        for (int i = 0; i < n; i++) {
            if (DUB_ENT_IS_DIR(&e[i])) continue;
            wchar_t b[MAX_PATH];
            DubBaseNoExt(e[i].name, b, MAX_PATH);
            if (_wcsicmp(b, base) == 0 && DubEndsWithI(e[i].name, prefExts[p])) { pick = i; break; }
        }
    }
    if (pick < 0) pick = firstMatch;
    if (pick >= 0) DubJoin(out, cap, folder, e[pick].name);
    free(e);
    return pick >= 0;
}

/* reads each _dubrecord_*.wav in the export, looks up its timestamps in the pack's
   .txt/.ini, and returns one placement per timestamp. Returns count, or -1 on error
   (message in err). */
static int DubCollectPlacements(const wchar_t* recording, const wchar_t* pack, DubPlacement** outP,
                                wchar_t*** outUnmatched, int* outUnmatchedCount, wchar_t* err, size_t errCap)
{
    *outP = NULL; *outUnmatched = NULL; *outUnmatchedCount = 0;
    int n = 0;
    DubEnt* e = DubListDir(recording, &n);
    if (!e) { _snwprintf(err, errCap - 1, L"Could not read the recording folder."); return -1; }

    DubPlacement* pl = NULL; int plN = 0, plCap = 0;
    wchar_t** um = (wchar_t**)calloc(n > 0 ? n : 1, sizeof(wchar_t*));
    int umN = 0;
    int rc = 0;

    for (int i = 0; i < n && rc == 0; i++) {
        if (DUB_ENT_IS_DIR(&e[i]) || !DubIsRecordingWav(e[i].name)) continue;
        wchar_t base[MAX_PATH];
        DubBaseNoExt(e[i].name, base, MAX_PATH);
        const wchar_t* clip = base + wcslen(L"_dubrecord_");

        wchar_t meta[DUB_PATH] = L"";
        const wchar_t* exts[2] = { L".txt", L".ini" };
        for (int x = 0; x < 2; x++) {
            wchar_t fn[MAX_PATH + 8], p[DUB_PATH];
            _snwprintf(fn, MAX_PATH + 7, L"%ls%ls", clip, exts[x]);
            DubJoin(p, DUB_PATH, pack, fn);
            if (DubPathExists(p)) { DubCopy(meta, DUB_PATH, p); break; }
        }
        BOOL matched = FALSE;
        if (meta[0]) {
            size_t len = 0;
            char* b = DubReadFile(meta, &len);
            size_t bs, be;
            if (b && DubFindTsBlock(b, len, &bs, &be)) {
                /* every number in the block: -?\d+(\.\d+)? */
                for (size_t k = bs; k < be; ) {
                    BOOL neg = (b[k] == '-' && k + 1 < be && isdigit((unsigned char)b[k + 1]));
                    if (!neg && !isdigit((unsigned char)b[k])) { k++; continue; }
                    size_t t0 = k;
                    if (neg) k++;
                    while (k < be && isdigit((unsigned char)b[k])) k++;
                    if (k + 1 < be && b[k] == '.' && isdigit((unsigned char)b[k + 1])) {
                        k++;
                        while (k < be && isdigit((unsigned char)b[k])) k++;
                    }
                    char tok[64];
                    size_t tl = k - t0;
                    if (tl >= sizeof(tok)) tl = sizeof(tok) - 1;
                    memcpy(tok, b + t0, tl); tok[tl] = 0;
                    double ts = strtod(tok, NULL);
                    if (ts < 0) {
                        _snwprintf(err, errCap - 1, L"invalid timestamp in %ls", meta);
                        rc = -1;
                        break;
                    }
                    if (plN == plCap) {
                        int nc = plCap ? plCap * 2 : 64;
                        DubPlacement* np = (DubPlacement*)realloc(pl, nc * sizeof(DubPlacement));
                        if (!np) { rc = -1; _snwprintf(err, errCap - 1, L"Out of memory."); break; }
                        pl = np; plCap = nc;
                    }
                    wchar_t full[DUB_PATH];
                    DubJoin(full, DUB_PATH, recording, e[i].name);
                    pl[plN].wav = _wcsdup(full);
                    pl[plN].ts = ts;
                    plN++;
                    matched = TRUE;
                }
            }
            free(b);
        }
        if (!matched && rc == 0) um[umN++] = _wcsdup(e[i].name);
    }
    free(e);

    if (rc == 0 && plN == 0) {
        _snwprintf(err, errCap - 1, L"none of the recordings could be matched to timestamp metadata");
        rc = -1;
    }
    err[errCap - 1] = 0;
    *outP = pl; *outUnmatched = um; *outUnmatchedCount = umN;
    return rc == 0 ? plN : -1;
}

static int DubRunStage(const wchar_t* ffmpeg, const wchar_t* stage, const wchar_t* argLine, wchar_t* err, size_t errCap)
{
    if (InterlockedCompareExchange(&g_dubCancel, 0, 0)) return 2;
    DubLogF(L"\r\n========== %ls ==========", stage);
    DubCopy(g_dubStage, 64, stage);
    DWORD code = DubRunProcess(ffmpeg, argLine, TRUE, TRUE, NULL, 0, TRUE);
    if (InterlockedCompareExchange(&g_dubCancel, 0, 0)) return 2;
    if (code == (DWORD)-1) { _snwprintf(err, errCap - 1, L"Could not start FFmpeg:\r\n%ls", ffmpeg); err[errCap - 1] = 0; return 1; }
    if (code != 0) {
        _snwprintf(err, errCap - 1, L"FFmpeg failed during %ls (exit code %lu). See the activity log.", stage, (unsigned long)code);
        err[errCap - 1] = 0;
        return 1;
    }
    return 0;
}

static void DubDeleteTempDir(const wchar_t* dir)
{
    int n = 0;
    DubEnt* e = DubListDir(dir, &n);
    for (int i = 0; i < n; i++) {
        wchar_t p[DUB_PATH];
        DubJoin(p, DUB_PATH, dir, e[i].name);
        if (!DUB_ENT_IS_DIR(&e[i])) DeleteFileW(p);
    }
    free(e);
    RemoveDirectoryW(dir);
}

/* the actual dub render: voice batches -> final MP4. Returns 0 ok, 1 error, 2 cancelled. */
static int DubRenderMain(const DubRenderJob* job, wchar_t* err, size_t errCap)
{
    static const wchar_t* videoExts[4]   = { L".ogv", L".mp4", L".mkv", L".webm" };
    static const wchar_t* backingExts[6] = { L".mp3", L".wav", L".ogg", L".flac", L".m4a", L".aac" };

    wchar_t video[DUB_PATH], backing[DUB_PATH];
    if (!DubFindFileByBase(job->pack, L"dub_video", videoExts, 4, video, DUB_PATH)) {
        _snwprintf(err, errCap - 1, L"no dub_video.* file was found in the pack");
        return 1;
    }
    DubFindFileByBase(job->pack, L"_backing_track", backingExts, 6, backing, DUB_PATH);

    DubPlacement* pl = NULL; wchar_t** um = NULL; int umN = 0;
    int plN = DubCollectPlacements(job->recording, job->pack, &pl, &um, &umN, err, errCap);
    int rc = 1;
    wchar_t tempRoot[DUB_PATH] = L"";
    wchar_t batchPaths[512][64];     /* names only, joined with tempRoot when used */
    int batchCount = 0;
    BOOL finalStarted = FALSE;

    if (plN < 0) goto cleanup;

    DubLogF(L"Matched timed voice placements: %d", plN);
    if (umN > 0) {
        DubLogF(L"Warning: %d recording(s) could not be matched.", umN);
        for (int i = 0; i < umN; i++) {
            if (i >= 20) { DubLogF(L"...and %d more.", umN - 20); break; }
            DubLogF(L"  %ls", um[i]);
        }
    }

    {   /* make sure the output folder exists */
        wchar_t outDir[DUB_PATH];
        DubDirName(job->output, outDir, DUB_PATH);
        if (outDir[0] && wcscmp(outDir, L".") != 0 && !DubIsDir(outDir)) {
            int sc = SHCreateDirectoryExW(NULL, outDir, NULL);
            if (sc != ERROR_SUCCESS && sc != ERROR_ALREADY_EXISTS) {
                _snwprintf(err, errCap - 1, L"Could not create the output folder:\r\n%ls", outDir);
                goto cleanup;
            }
        }
    }

    {   /* private temp folder for the intermediate voice mixes */
        wchar_t tmp[DUB_PATH];
        DWORD tl = GetTempPathW(DUB_PATH, tmp);
        if (tl == 0 || tl >= DUB_PATH) { _snwprintf(err, errCap - 1, L"Could not find the Windows temp folder."); goto cleanup; }
        wchar_t nm[96];
        _snwprintf(nm, 95, L"ChoicerVoicer_Render_%lu_%lu", (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
        nm[95] = 0;
        DubJoin(tempRoot, DUB_PATH, tmp, nm);
        if (!CreateDirectoryW(tempRoot, NULL)) {
            tempRoot[0] = 0;
            _snwprintf(err, errCap - 1, L"Could not create a temporary folder for the render.");
            goto cleanup;
        }
    }

    {
        const int batchSize = 20;
        int totalBatches = (plN + batchSize - 1) / batchSize;
        int centered = 0;
        if (totalBatches > 512) { _snwprintf(err, errCap - 1, L"Too many voice clips to render (%d).", plN); goto cleanup; }

        for (int start = 0, b = 0; start < plN; start += batchSize, b++) {
            int end = start + batchSize;
            if (end > plN) end = plN;
            int count = end - start;

            _snwprintf(batchPaths[b], 63, L"voice_batch_%03d.wav", b);
            batchPaths[b][63] = 0;
            wchar_t batchOut[DUB_PATH];
            DubJoin(batchOut, DUB_PATH, tempRoot, batchPaths[b]);
            batchCount = b + 1;

            DubSb args; DubSbInit(&args);
            DubSb filter; DubSbInit(&filter);
            DubSb labels; DubSbInit(&labels);
            DubAppendArg(&args, L"-hide_banner");
            DubAppendArg(&args, L"-y");
            for (int i = 0; i < count; i++) {
                const DubPlacement* p = &pl[start + i];
                DubAppendArg(&args, L"-i");
                DubAppendArg(&args, p->wav);
                BOOL corrected;
                const wchar_t* centering = DubCenteringSuffix(p->wav, &corrected);
                if (corrected) centered++;
                wchar_t piece[512];
                _snwprintf(piece, 511, L"[%d:a]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo%ls,adelay=%I64d:all=1[v%d];",
                    i, centering, (long long)(p->ts * 1000.0 + 0.5), i);
                piece[511] = 0;
                DubSbAppend(&filter, piece);
                _snwprintf(piece, 511, L"[v%d]", i);
                DubSbAppend(&labels, piece);
            }
            wchar_t tail[256];
            _snwprintf(tail, 255, L"amix=inputs=%d:duration=longest:dropout_transition=0:normalize=0,alimiter=limit=0.95[batch]", count);
            tail[255] = 0;
            if (labels.p) DubSbAppend(&filter, labels.p);
            DubSbAppend(&filter, tail);
            DubAppendArg(&args, L"-filter_complex");
            DubAppendArg(&args, filter.p ? filter.p : L"");
            const wchar_t* rest[] = { L"-map", L"[batch]", L"-c:a", L"pcm_s16le", L"-ar", L"48000" };
            for (int i = 0; i < 6; i++) DubAppendArg(&args, rest[i]);
            DubAppendArg(&args, batchOut);

            wchar_t stage[64];
            _snwprintf(stage, 63, L"Voice batch %d of %d", b + 1, totalBatches);
            stage[63] = 0;
            int sr = args.p ? DubRunStage(job->ffmpeg, stage, args.p, err, errCap) : 1;
            if (!args.p) _snwprintf(err, errCap - 1, L"Out of memory.");
            DubSbFree(&args); DubSbFree(&filter); DubSbFree(&labels);
            if (sr != 0) { rc = sr; goto cleanup; }
            if (!DubFileExists(batchOut)) {
                _snwprintf(err, errCap - 1, L"temporary voice batch was not created:\r\n%ls", batchOut);
                goto cleanup;
            }
        }
        if (centered > 0)
            DubLogF(L"Centered %d single-sided voice clip(s) so mono microphone recordings play in both ears.", centered);
    }

    {   /* final mux: dub_video + backing track + all voice batches -> MP4 */
        DubSb args; DubSbInit(&args);
        DubSb filter; DubSbInit(&filter);
        DubSb labels; DubSbInit(&labels);
        DubAppendArg(&args, L"-hide_banner");
        DubAppendArg(&args, L"-y");
        DubAppendArg(&args, L"-i");
        DubAppendArg(&args, video);
        DubSbAppend(&labels, L"[bg]");
        int nextInput = 1;
        wchar_t piece[512];
        if (backing[0]) {
            DubAppendArg(&args, L"-i");
            DubAppendArg(&args, backing);
            _snwprintf(piece, 511, L"[%d:a]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo[bg];", nextInput);
            piece[511] = 0;
            DubSbAppend(&filter, piece);
            nextInput++;
        } else {
            DubLog(L"Warning: no _backing_track.* found; rendering voices over silence.");
            DubSbAppend(&filter, L"anullsrc=r=48000:cl=stereo[bg];");
        }
        for (int i = 0; i < batchCount; i++) {
            wchar_t bp[DUB_PATH];
            DubJoin(bp, DUB_PATH, tempRoot, batchPaths[i]);
            DubAppendArg(&args, L"-i");
            DubAppendArg(&args, bp);
            _snwprintf(piece, 511, L"[%d:a]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo[b%d];", nextInput, i);
            piece[511] = 0;
            DubSbAppend(&filter, piece);
            _snwprintf(piece, 511, L"[b%d]", i);
            DubSbAppend(&labels, piece);
            nextInput++;
        }
        int totalLabels = batchCount + 1;
        wchar_t tail[256];
        _snwprintf(tail, 255, L"amix=inputs=%d:duration=longest:dropout_transition=0:normalize=0,alimiter=limit=0.95,apad[mixed]", totalLabels);
        tail[255] = 0;
        if (labels.p) DubSbAppend(&filter, labels.p);
        DubSbAppend(&filter, tail);
        DubAppendArg(&args, L"-filter_complex");
        DubAppendArg(&args, filter.p ? filter.p : L"");
        const wchar_t* rest[] = {
            L"-map", L"0:v:0", L"-map", L"[mixed]",
            L"-c:v", L"libx264", L"-preset", L"medium", L"-crf", L"18", L"-pix_fmt", L"yuv420p",
            L"-c:a", L"aac", L"-b:a", L"320k", L"-ar", L"48000",
            L"-movflags", L"+faststart", L"-shortest"
        };
        for (int i = 0; i < (int)(sizeof(rest) / sizeof(rest[0])); i++) DubAppendArg(&args, rest[i]);
        DubAppendArg(&args, job->output);

        finalStarted = TRUE;
        int sr = args.p ? DubRunStage(job->ffmpeg, L"Final MP4", args.p, err, errCap) : 1;
        if (!args.p) _snwprintf(err, errCap - 1, L"Out of memory.");
        DubSbFree(&args); DubSbFree(&filter); DubSbFree(&labels);
        if (sr != 0) { rc = sr; goto cleanup; }
        if (!DubFileExists(job->output)) {
            _snwprintf(err, errCap - 1, L"FFmpeg returned success, but the output MP4 does not exist or is empty");
            goto cleanup;
        }
    }
    rc = 0;

cleanup:
    err[errCap - 1] = 0;
    if (rc == 2 && finalStarted) DeleteFileW(job->output);      /* don't leave a half-written MP4 behind */
    if (tempRoot[0]) DubDeleteTempDir(tempRoot);
    for (int i = 0; pl && i < plN; i++) free(pl[i].wav);
    free(pl);
    for (int i = 0; um && i < umN; i++) free(um[i]);
    free(um);
    return rc;
}

static DWORD WINAPI DubRenderThread(LPVOID p)
{
    DubRenderJob* job = (DubRenderJob*)p;
    wchar_t err[1024] = L"";
    int rc = DubRenderMain(job, err, 1024);
    if (rc == 0) DubPostText(WM_DUB_RENDERDONE, 0, job->output);
    else if (rc == 2) DubPostText(WM_DUB_RENDERDONE, 2, L"Render cancelled.");
    else DubPostText(WM_DUB_RENDERDONE, 1, err[0] ? err : L"The render failed.");
    free(job);
    return 0;
}

/* ---------------- UI ---------------- */

static HWND DubMsgOwner(void) { return g_hDubWnd ? GetAncestor(g_hDubWnd, GA_ROOT) : NULL; }

static void DubSetStatus(int which, int kind, const wchar_t* text)
{
    if (!g_hDubStatus[which]) return;
    g_dubStatusKind[which] = kind;
    SetWindowTextW(g_hDubStatus[which], text);
    InvalidateRect(g_hDubStatus[which], NULL, TRUE);
}

static COLORREF DubKindColor(int kind)
{
    BOOL d = g_darkMode;
    switch (kind) {
        case DUBC_ACCENT:  return Pal()->accent;
        case DUBC_SUCCESS: return d ? RGB(110, 215, 140) : RGB(25, 125, 55);
        case DUBC_WARNING: return d ? RGB(245, 200, 90)  : RGB(170, 105, 0);
        case DUBC_DANGER:  return d ? RGB(250, 125, 125) : RGB(190, 40, 40);
        default:           return d ? RGB(165, 165, 172) : RGB(118, 118, 126);
    }
}

static BOOL DubBusy(void) { return g_dubRendering || g_dubFfInstalling; }

static void DubUpdateButtons(void)
{
    BOOL idle = !g_dubRendering;
    EnableWindow(g_hDubCombo, idle && !g_dubScanning);
    EnableWindow(g_hDubRefreshBtn, idle && !g_dubScanning);
    EnableWindow(g_hDubBrowseRecBtn, idle);
    EnableWindow(g_hDubBrowsePackBtn, idle);
    EnableWindow(g_hDubBrowseOutBtn, idle);
    EnableWindow(g_hDubRenderBtn, idle && !g_dubFfInstalling && !g_dubFfChecking);
    EnableWindow(g_hDubCancelBtn, g_dubRendering);
    EnableWindow(g_hDubOpenBtn, idle && g_dubLastOutput[0] && DubFileExists(g_dubLastOutput));
    InvalidateRect(g_hDubWnd, &g_dubBarRect, FALSE);
}

static void DubUpdateTimer(void)
{
    if (DubBusy()) SetTimer(g_hDubWnd, DUB_TIMER_ID, 30, NULL);
    else { KillTimer(g_hDubWnd, DUB_TIMER_ID); InvalidateRect(g_hDubWnd, &g_dubBarRect, FALSE); }
}

static void DubAppendLog(const wchar_t* text)
{
    if (!g_hDubLog) return;
    int len = GetWindowTextLengthW(g_hDubLog);
    if (len > 300000) {                         /* keep the log from growing forever */
        SendMessageW(g_hDubLog, EM_SETSEL, 0, 150000);
        SendMessageW(g_hDubLog, EM_REPLACESEL, FALSE, (LPARAM)L"");
        len = GetWindowTextLengthW(g_hDubLog);
    }
    SendMessageW(g_hDubLog, EM_SETSEL, len, len);
    SendMessageW(g_hDubLog, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(g_hDubLog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(g_hDubLog, EM_SCROLLCARET, 0, 0);
}

static void DubClearOutputState(void)
{
    SetWindowTextW(g_hDubOutEdit, L"");
    g_dubLastOutput[0] = 0;
}

static void DubStartScan(void)
{
    if (g_dubScanning) return;
    g_dubScanning = TRUE;
    DubClearOutputState();
    DubSetStatus(DST_SCAN, DUBC_ACCENT, L"Scanning exports in the background...");
    DubUpdateButtons();
    HANDLE t = CreateThread(NULL, 0, DubScanThread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else { g_dubScanning = FALSE; DubUpdateButtons(); }
}

static void DubStartFfCheck(void)
{
    if (g_dubFfChecking || g_dubFfInstalling) return;
    g_dubFfChecking = TRUE;
    g_dubFfReady = FALSE;
    DubSetStatus(DST_FF, DUBC_WARNING, L"Checking FFmpeg...");
    DubUpdateButtons();
    HANDLE t = CreateThread(NULL, 0, DubFfCheckThread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else { g_dubFfChecking = FALSE; DubUpdateButtons(); }
}

static void DubOnTabShown(void)
{
    if (g_dubStarted) return;
    g_dubStarted = TRUE;
    DubStartScan();
    DubStartFfCheck();
}

/* asks first, then installs FFmpeg through winget on a worker thread */
static void DubOfferFfmpegInstall(void)
{
    const wchar_t* why = (g_dubFfDetected == 2)
        ? L"An FFmpeg was found on this PC, but it is too old for the dub renderer."
        : L"No FFmpeg was found on this PC, and the dub renderer needs one to build the MP4.";
    wchar_t msg[1024];
    _snwprintf(msg, 1023,
        L"%ls\r\n\r\n"
        L"Install a compatible build now with Windows Package Manager (winget)? This installs the Gyan.FFmpeg package "
        L"and replaces any older WinGet Gyan.FFmpeg packages. Other FFmpeg copies are left alone. It can take a few minutes.\r\n\r\n"
        L"(You can also just place ffmpeg.exe next to the launcher.)", why);
    msg[1023] = 0;
    if (MessageBoxW(DubMsgOwner(), msg, L"FFmpeg needed", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    g_dubFfInstalling = TRUE;
    DubSetStatus(DST_FF, DUBC_WARNING, L"Installing FFmpeg...");
    DubSetStatus(DST_RENDER, DUBC_ACCENT, L"Installing a compatible FFmpeg build. This can take a few minutes...");
    DubAppendLog(L"Starting FFmpeg installation through winget.");
    DubUpdateButtons();
    DubUpdateTimer();
    HANDLE t = CreateThread(NULL, 0, DubFfInstallThread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else { g_dubFfInstalling = FALSE; DubUpdateButtons(); DubUpdateTimer(); }
}

/* ---- pickers ---- */

static int CALLBACK DubBrowseCb(HWND hwnd, UINT msg, LPARAM lp, LPARAM data)
{
    (void)lp;
    if (msg == BFFM_INITIALIZED && data) SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

static void DubNearestExistingDir(const wchar_t* path, wchar_t* out)
{
    out[0] = 0;
    wchar_t cur[DUB_PATH];
    DubCopy(cur, DUB_PATH, path);
    DubClean(cur);
    for (int guard = 0; guard < 64 && cur[0] && wcscmp(cur, L".") != 0; guard++) {
        if (DubIsDir(cur)) { DubCopy(out, DUB_PATH, cur); return; }
        wchar_t parent[DUB_PATH];
        DubDirName(cur, parent, DUB_PATH);
        if (wcscmp(parent, cur) == 0) break;
        DubCopy(cur, DUB_PATH, parent);
    }
}

static BOOL DubBrowseFolder(const wchar_t* title, const wchar_t* initial, wchar_t* out, size_t cap)
{
    wchar_t init[DUB_PATH];
    DubNearestExistingDir(initial, init);
    wchar_t display[MAX_PATH];
    BROWSEINFOW bi; memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = DubMsgOwner();
    bi.pszDisplayName = display;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = DubBrowseCb;
    bi.lParam = init[0] ? (LPARAM)init : 0;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return FALSE;
    wchar_t path[MAX_PATH];
    BOOL ok = SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    if (!ok) return FALSE;
    DubCopy(out, cap, path);
    return TRUE;
}

static void DubSanitizeFileName(const wchar_t* in, wchar_t* out, size_t cap)
{
    wchar_t tmp[MAX_PATH];
    DubCopy(tmp, MAX_PATH, in);
    for (wchar_t* c = tmp; *c; c++) if (*c < 32 || wcschr(L"<>:\"/\\|?*", *c)) *c = L'-';
    size_t n = wcslen(tmp);
    while (n > 0 && (tmp[n - 1] == L'.' || tmp[n - 1] == L' ')) tmp[--n] = 0;
    wchar_t* s = tmp;
    while (*s == L' ') s++;
    if (!*s) DubCopy(s = tmp, MAX_PATH, L"ChoicerVoicer Dub");
    if (!DubEndsWithI(s, L".mp4")) _snwprintf(out, cap - 1, L"%ls.mp4", s);
    else DubCopy(out, cap, s);
    out[cap - 1] = 0;
}

static void DubSuggestedOutputName(wchar_t* out, size_t cap)
{
    wchar_t rec[DUB_PATH];
    GetWindowTextW(g_hDubRecEdit, rec, DUB_PATH);
    if (rec[0]) {
        DubClean(rec);
        wchar_t parent[DUB_PATH];
        DubDirName(rec, parent, DUB_PATH);
        const wchar_t* nm = DubBaseName(parent);
        if (*nm && wcscmp(nm, L".") != 0) {
            wchar_t t[MAX_PATH];
            _snwprintf(t, MAX_PATH - 1, L"%ls - Dubbed.mp4", nm); t[MAX_PATH - 1] = 0;
            DubSanitizeFileName(t, out, cap);
            return;
        }
    }
    wchar_t pack[DUB_PATH];
    GetWindowTextW(g_hDubPackEdit, pack, DUB_PATH);
    if (pack[0]) {
        DubClean(pack);
        const wchar_t* nm = DubBaseName(pack);
        if (*nm && wcscmp(nm, L".") != 0) {
            wchar_t t[MAX_PATH];
            _snwprintf(t, MAX_PATH - 1, L"%ls - Dubbed.mp4", nm); t[MAX_PATH - 1] = 0;
            DubSanitizeFileName(t, out, cap);
            return;
        }
    }
    DubCopy(out, cap, L"ChoicerVoicer Dub.mp4");
}

static BOOL DubBrowseOutput(const wchar_t* current, wchar_t* out, size_t cap)
{
    wchar_t* fileBuf = (wchar_t*)calloc(32768, sizeof(wchar_t));
    if (!fileBuf) return FALSE;
    if (current && *current) DubCopy(fileBuf, 32768, current);
    else DubSuggestedOutputName(fileBuf, 32768);

    wchar_t initDir[DUB_PATH] = L"";
    if (current && *current) {
        wchar_t d[DUB_PATH];
        DubCopy(d, DUB_PATH, current); DubClean(d);
        DubDirName(d, initDir, DUB_PATH);
        if (!DubIsDir(initDir)) initDir[0] = 0;
    }
    if (!initDir[0]) {
        wchar_t up[DUB_PATH], c[DUB_PATH];
        DubEnvPath(L"USERPROFILE", up, DUB_PATH);
        if (up[0]) {
            DubJoin(c, DUB_PATH, up, L"Videos");
            if (DubIsDir(c)) DubCopy(initDir, DUB_PATH, c);
            else {
                DubJoin(c, DUB_PATH, up, L"Documents");
                DubCopy(initDir, DUB_PATH, DubIsDir(c) ? c : up);
            }
        }
    }

    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = DubMsgOwner();
    ofn.lpstrFilter = L"MP4 Video (*.mp4)\0*.mp4\0All Files (*.*)\0*.*\0\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = 32768;
    ofn.lpstrInitialDir = initDir[0] ? initDir : NULL;
    ofn.lpstrTitle = L"Choose output MP4";
    ofn.lpstrDefExt = L"mp4";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    BOOL ok = GetSaveFileNameW(&ofn);
    if (ok) {
        DubCopy(out, cap, fileBuf);
        if (!wcsrchr(DubBaseName(out), L'.')) {
            size_t n = wcslen(out);
            if (n + 5 < cap) wcscat(out, L".mp4");
        }
    }
    free(fileBuf);
    return ok;
}

/* ---- actions ---- */

static void DubApplyRecording(const wchar_t* path)
{
    if (!DubContainsRecordingWavs(path)) {
        MessageBoxW(DubMsgOwner(), L"That folder does not contain any _dubrecord_*.wav files.",
            L"Invalid export folder", MB_OK | MB_ICONWARNING);
        return;
    }
    wchar_t abs[DUB_PATH];
    DWORD n = GetFullPathNameW(path, DUB_PATH, abs, NULL);
    if (n == 0 || n >= DUB_PATH) DubCopy(abs, DUB_PATH, path);
    DubClean(abs);

    DubClearOutputState();
    SetWindowTextW(g_hDubRecEdit, abs);
    wchar_t msg[512];
    _snwprintf(msg, 511, L"Selected export contains %d recording clip(s).", DubCountRecordingWavs(abs));
    msg[511] = 0;
    DubSetStatus(DST_SCAN, DUBC_SUCCESS, msg);

    wchar_t pack[DUB_PATH];
    if (DubFindMatchingPack(abs, pack, DUB_PATH)) {
        SetWindowTextW(g_hDubPackEdit, pack);
        wchar_t groupName[MAX_PATH], groupRoot[DUB_PATH], packs[DUB_PATH], a[DUB_PATH], b[DUB_PATH];
        DubPackNameFromExport(abs, groupName, MAX_PATH);
        DubAppPath(packs, DUB_PATH, L"packs_voice");
        DubJoin(groupRoot, DUB_PATH, packs, groupName);
        DubCopy(a, DUB_PATH, pack); DubClean(a);
        DubCopy(b, DUB_PATH, groupRoot); DubClean(b);
        if (_wcsicmp(a, b) == 0) {
            DubSetStatus(DST_PACK, DUBC_SUCCESS, L"Matching pack detected automatically (.txt or .ini metadata).");
        } else {
            _snwprintf(msg, 511, L"Matching nested pack detected automatically: %ls", DubBaseName(a));
            msg[511] = 0;
            DubSetStatus(DST_PACK, DUBC_SUCCESS, msg);
        }
    } else {
        SetWindowTextW(g_hDubPackEdit, L"");
        DubSetStatus(DST_PACK, DUBC_WARNING, L"Matching pack not found automatically. Choose the original pack folder below.");
    }
    DubUpdateButtons();
}

static void DubSetRendering(BOOL on)
{
    g_dubRendering = on;
    if (on) DubSetStatus(DST_RENDER, DUBC_ACCENT, L"Rendering in the background...");
    DubUpdateButtons();
    DubUpdateTimer();
}

static void DubStartRender(void)
{
    if (g_dubFfInstalling) {
        MessageBoxW(DubMsgOwner(), L"FFmpeg is currently being installed. Wait for the status to change to \"FFmpeg ready\".",
            L"Installing FFmpeg", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!g_dubFfReady) { DubOfferFfmpegInstall(); return; }

    wchar_t recording[DUB_PATH], pack[DUB_PATH], output[DUB_PATH];
    GetWindowTextW(g_hDubRecEdit, recording, DUB_PATH);
    GetWindowTextW(g_hDubPackEdit, pack, DUB_PATH);
    GetWindowTextW(g_hDubOutEdit, output, DUB_PATH);
    if (!DubContainsRecordingWavs(recording)) {
        MessageBoxW(DubMsgOwner(), L"Choose a valid exported recording folder first.", L"Missing recordings", MB_OK | MB_ICONWARNING);
        return;
    }
    if (!DubValidPackFolder(pack)) {
        MessageBoxW(DubMsgOwner(), L"Choose a valid original pack folder containing dub_video.* and timestamp metadata in .txt or .ini files.",
            L"Missing pack", MB_OK | MB_ICONWARNING);
        return;
    }
    if (!output[0]) {
        MessageBoxW(DubMsgOwner(), L"Choose an output MP4 path.", L"Missing output", MB_OK | MB_ICONWARNING);
        return;
    }
    if (!DubEndsWithI(output, L".mp4")) {
        if (wcslen(output) + 5 < DUB_PATH) wcscat(output, L".mp4");
        SetWindowTextW(g_hDubOutEdit, output);
    }

    DubRenderJob* job = (DubRenderJob*)calloc(1, sizeof(*job));
    if (!job) return;
    DubCopy(job->recording, DUB_PATH, recording);
    DubCopy(job->pack, DUB_PATH, pack);
    DubCopy(job->output, DUB_PATH, output);
    DubCopy(job->ffmpeg, DUB_PATH, g_dubFfPath);

    SetWindowTextW(g_hDubLog, L"");
    DubAppendLog(L"Recording folder: ");
    DubAppendLog(recording);
    DubAppendLog(L"Pack folder:      ");
    DubAppendLog(pack);
    DubAppendLog(L"Output:           ");
    DubAppendLog(output);

    g_dubLastOutput[0] = 0;
    InterlockedExchange(&g_dubCancel, 0);
    DubSetRendering(TRUE);
    HANDLE t = CreateThread(NULL, 0, DubRenderThread, job, 0, NULL);
    if (t) CloseHandle(t);
    else {
        free(job);
        DubSetRendering(FALSE);
        DubSetStatus(DST_RENDER, DUBC_DANGER, L"Could not start the render thread.");
    }
}

static void DubCancelRender(void)
{
    if (!g_dubRendering) return;
    InterlockedExchange(&g_dubCancel, 1);
    DubSetStatus(DST_RENDER, DUBC_WARNING, L"Cancelling...");
    DubKillRunningProcess();
}

static void DubOpenOutput(void)
{
    if (!g_dubLastOutput[0] || !DubFileExists(g_dubLastOutput)) {
        MessageBoxW(DubMsgOwner(), L"The rendered output file could not be found.", L"Output missing", MB_OK | MB_ICONWARNING);
        return;
    }
    wchar_t abs[DUB_PATH];
    DWORD n = GetFullPathNameW(g_dubLastOutput, DUB_PATH, abs, NULL);
    if (n == 0 || n >= DUB_PATH) DubCopy(abs, DUB_PATH, g_dubLastOutput);
    wchar_t params[DUB_PATH + 16], dir[DUB_PATH];
    _snwprintf(params, DUB_PATH + 15, L"/select,\"%ls\"", abs);
    params[DUB_PATH + 15] = 0;
    DubDirName(abs, dir, DUB_PATH);
    HINSTANCE r = ShellExecuteW(DubMsgOwner(), L"open", L"explorer.exe", params, dir, SW_SHOW);
    if ((INT_PTR)r <= 32) ShellExecuteW(DubMsgOwner(), L"open", dir, NULL, NULL, SW_SHOW);
}

/* ---- message handlers for results coming back from threads ---- */

static void DubFinishScan(DubScanResult* r)
{
    free(g_dubExports);
    g_dubExports = NULL;
    g_dubExportCount = 0;
    if (r) { g_dubExports = r->items; g_dubExportCount = r->count; free(r); }
    g_dubScanning = FALSE;

    SendMessageW(g_hDubCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_hDubCombo, CB_ADDSTRING, 0, (LPARAM)(g_dubExportCount ? L"Select an export..." : L"No exports found"));
    for (int i = 0; i < g_dubExportCount; i++) SendMessageW(g_hDubCombo, CB_ADDSTRING, 0, (LPARAM)g_dubExports[i].display);
    SendMessageW(g_hDubCombo, CB_SETCURSEL, 0, 0);
    SendMessageW(g_hDubCombo, CB_SETDROPPEDWIDTH, 720, 0);

    SetWindowTextW(g_hDubRecEdit, L"");
    SetWindowTextW(g_hDubPackEdit, L"");
    DubClearOutputState();
    DubSetStatus(DST_PACK, DUBC_MUTED, L"No export selected yet.");
    DubSetStatus(DST_RENDER, DUBC_MUTED, L"Select an export, then choose the output MP4 location.");
    if (g_dubExportCount == 0) {
        DubSetStatus(DST_SCAN, DUBC_WARNING, L"No exports found. Use Choose folder to select one manually.");
    } else {
        wchar_t msg[160];
        _snwprintf(msg, 159, L"Found %d export(s). Choose the exact one you want.", g_dubExportCount);
        msg[159] = 0;
        DubSetStatus(DST_SCAN, DUBC_MUTED, msg);
    }
    DubUpdateButtons();
}

static void DubFinishFfCheck(int result, wchar_t* path)
{
    g_dubFfChecking = FALSE;
    if (result == 1 && path && *path) {
        DubCopy(g_dubFfPath, DUB_PATH, path);
        g_dubFfReady = TRUE;
        g_dubFfDetected = 1;
        DubSetStatus(DST_FF, DUBC_SUCCESS, L"FFmpeg ready");
        DubAppendLog(L"Using compatible FFmpeg:");
        DubAppendLog(path);
    } else {
        g_dubFfReady = FALSE;
        g_dubFfDetected = result;
        DubSetStatus(DST_FF, DUBC_DANGER, result == 2 ? L"FFmpeg too old" : L"FFmpeg not found");
        DubSetStatus(DST_RENDER, DUBC_WARNING, L"FFmpeg is required. Click Render MP4 and the launcher will offer to install it.");
    }
    DubUpdateButtons();
}

static void DubFinishFfInstall(BOOL ok, wchar_t* text)
{
    g_dubFfInstalling = FALSE;
    if (ok) {
        DubCopy(g_dubFfPath, DUB_PATH, text);
        g_dubFfReady = TRUE;
        g_dubFfDetected = 1;
        DubSetStatus(DST_FF, DUBC_SUCCESS, L"FFmpeg ready");
        DubAppendLog(L"Compatible FFmpeg installed and verified successfully:");
        DubAppendLog(text);
        wchar_t rec[DUB_PATH];
        GetWindowTextW(g_hDubRecEdit, rec, DUB_PATH);
        DubSetStatus(DST_RENDER, DUBC_SUCCESS, rec[0] ? L"FFmpeg installed. Ready to render."
                                                      : L"FFmpeg installed. Select an export and choose the output MP4 location.");
    } else {
        g_dubFfReady = FALSE;
        DubSetStatus(DST_FF, DUBC_DANGER, L"FFmpeg install failed");
        DubSetStatus(DST_RENDER, DUBC_DANGER, L"Automatic FFmpeg installation failed. See the activity log.");
        DubAppendLog(L"ERROR:");
        DubAppendLog(text);
        wchar_t msg[1200];
        _snwprintf(msg, 1199, L"A compatible FFmpeg build could not be installed automatically.\r\n\r\n%ls", text);
        msg[1199] = 0;
        MessageBoxW(DubMsgOwner(), msg, L"FFmpeg install failed", MB_OK | MB_ICONERROR);
    }
    DubUpdateButtons();
    DubUpdateTimer();
}

static void DubFinishRender(int code, wchar_t* text)
{
    DubSetRendering(FALSE);
    if (code == 0) {
        WIN32_FILE_ATTRIBUTE_DATA d;
        double mb = 0;
        if (GetFileAttributesExW(text, GetFileExInfoStandard, &d))
            mb = ((double)d.nFileSizeHigh * 4294967296.0 + (double)d.nFileSizeLow) / 1024.0 / 1024.0;
        DubCopy(g_dubLastOutput, DUB_PATH, text);
        wchar_t msg[160];
        _snwprintf(msg, 159, L"Render complete  \u2022  %.2f MB", mb);
        msg[159] = 0;
        DubSetStatus(DST_RENDER, DUBC_SUCCESS, msg);
        DubUpdateButtons();
        MessageBoxW(DubMsgOwner(), L"The dubbed MP4 was created successfully.", L"Render complete", MB_OK | MB_ICONINFORMATION);
    } else if (code == 2) {
        DubSetStatus(DST_RENDER, DUBC_WARNING, L"Render cancelled.");
        DubAppendLog(L"Render cancelled.");
    } else {
        DubSetStatus(DST_RENDER, DUBC_DANGER, L"Render failed. See the activity log below.");
        DubAppendLog(L"ERROR:");
        DubAppendLog(text);
        MessageBoxW(DubMsgOwner(), text, L"Render failed", MB_OK | MB_ICONERROR);
    }
}

/* ---- layout, painting and the window itself ---- */

/* Anton's line box is ~1.7x its font size, so controls are sized from the real font
   metrics rather than fixed pixel heights (fixed heights were clipping the text) */
static int DubFontH(HFONT f)
{
    HDC dc = GetDC(NULL);
    HGDIOBJ old = SelectObject(dc, f);
    TEXTMETRICW tm;
    int h = GetTextMetricsW(dc, &tm) ? tm.tmHeight : 20;
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    return h;
}

static void DubLayout(HWND hwnd)
{
    if (!g_hDubCombo) return;
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    const int pad = 14, gap = 8, btnW = 140, infoW = 32;
    int iw = W - 2 * pad;

    int hTitle = DubFontH(g_fontTitle) + 2;
    int hSmall = DubFontH(g_fontSmall) + 2;
    int hHdr   = DubFontH(g_fontGroupHeader) + 2;
    int hEdit  = DubFontH(g_fontRegular) + 8;
    if (hEdit < 30) hEdit = 30;
    int hRow = hEdit + 2;                  /* combo / button row height */

    int y = 4;
    MoveWindow(g_hDubTitle, pad, y, iw - infoW - gap, hTitle, TRUE);
    MoveWindow(g_hDubInfoBtn, W - pad - infoW, y + (hTitle - infoW) / 2, infoW, infoW, TRUE);
    y += hTitle;
    MoveWindow(g_hDubSubtitle, pad, y, iw - infoW - gap, hSmall, TRUE);
    y += hSmall + 6;

    /* section 1: export */
    SetRect(&g_dubHdrRect[0], pad, y, W - pad, y + hHdr); y += hHdr + 2;
    MoveWindow(g_hDubCombo, pad, y, iw - 100 - gap, 300, TRUE);
    MoveWindow(g_hDubRefreshBtn, W - pad - 100, y, 100, hRow, TRUE);
    y += hRow + 2;
    MoveWindow(g_hDubStatus[DST_SCAN], pad, y, iw, hSmall, TRUE); y += hSmall + 2;
    MoveWindow(g_hDubCapRec, pad, y, iw, hSmall, TRUE); y += hSmall;
    MoveWindow(g_hDubRecEdit, pad, y, iw - btnW - gap, hEdit, TRUE);
    MoveWindow(g_hDubBrowseRecBtn, W - pad - btnW, y, btnW, hEdit, TRUE);
    y += hEdit + 6;

    /* section 2: pack + output */
    SetRect(&g_dubHdrRect[1], pad, y, W - pad, y + hHdr); y += hHdr + 2;
    MoveWindow(g_hDubCapPack, pad, y, iw, hSmall, TRUE); y += hSmall;
    MoveWindow(g_hDubPackEdit, pad, y, iw - btnW - gap, hEdit, TRUE);
    MoveWindow(g_hDubBrowsePackBtn, W - pad - btnW, y, btnW, hEdit, TRUE);
    y += hEdit;
    MoveWindow(g_hDubStatus[DST_PACK], pad, y, iw, hSmall, TRUE); y += hSmall + 2;
    MoveWindow(g_hDubCapOut, pad, y, iw, hSmall, TRUE); y += hSmall;
    MoveWindow(g_hDubOutEdit, pad, y, iw - btnW - gap, hEdit, TRUE);
    MoveWindow(g_hDubBrowseOutBtn, W - pad - btnW, y, btnW, hEdit, TRUE);
    y += hEdit + 6;

    /* section 3: render */
    SetRect(&g_dubHdrRect[2], pad, y, W - pad, y + hHdr); y += hHdr + 4;
    int bh = hEdit + 4;
    MoveWindow(g_hDubRenderBtn, pad, y, 150, bh, TRUE);
    MoveWindow(g_hDubCancelBtn, pad + 158, y, 100, bh, TRUE);
    MoveWindow(g_hDubOpenBtn, pad + 266, y, 130, bh, TRUE);
    MoveWindow(g_hDubStatus[DST_FF], pad + 404, y + (bh - hSmall) / 2, W - pad - (pad + 404), hSmall, TRUE);
    y += bh + 4;
    MoveWindow(g_hDubStatus[DST_RENDER], pad, y, iw, hSmall, TRUE); y += hSmall + 4;

    SetRect(&g_dubBarRect, pad, y, W - pad, y + 8); y += 8 + 8;
    int logH = H - y - 10;
    if (logH < 50) logH = 50;
    MoveWindow(g_hDubLog, pad, y, iw, logH, TRUE);
}

static void DubPaintHeader(HDC hdc, const RECT* r, const wchar_t* text)
{
    HGDIOBJ old = SelectObject(hdc, g_fontGroupHeader);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, DubKindColor(DUBC_ACCENT));
    RECT tr = *r;
    DrawTextW(hdc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SIZE sz; GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &sz);
    SelectObject(hdc, old);
    int y = (r->top + r->bottom) / 2;
    HPEN pen = CreatePen(PS_SOLID, 1, g_darkMode ? COLOR_DARK_BORDER : COLOR_LIGHT_BORDER);
    HGDIOBJ op = SelectObject(hdc, pen);
    MoveToEx(hdc, r->left + sz.cx + 12, y, NULL);
    LineTo(hdc, r->right, y);
    SelectObject(hdc, op);
    DeleteObject(pen);
}

static void DubPaintBar(HDC hdc)
{
    RECT r = g_dubBarRect;
    HBRUSH track = CreateSolidBrush(g_darkMode ? COLOR_DARK_PANEL : RGB(226, 226, 230));
    HPEN pen = CreatePen(PS_SOLID, 1, g_darkMode ? COLOR_DARK_BORDER : COLOR_LIGHT_BORDER);
    HGDIOBJ op = SelectObject(hdc, pen);
    HGDIOBJ ob = SelectObject(hdc, track);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, 8, 8);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(track);
    DeleteObject(pen);

    if (DubBusy()) {
        int w = r.right - r.left;
        int seg = w / 4;
        int pos = (g_dubBarTick * 7) % (w + seg) - seg;
        HRGN clip = CreateRoundRectRgn(r.left + 1, r.top + 1, r.right, r.bottom, 8, 8);
        SelectClipRgn(hdc, clip);
        HBRUSH fill = CreateSolidBrush(DubKindColor(DUBC_ACCENT));
        RECT s = { r.left + pos, r.top + 1, r.left + pos + seg, r.bottom };
        FillRect(hdc, &s, fill);
        DeleteObject(fill);
        SelectClipRgn(hdc, NULL);
        DeleteObject(clip);
    }
}

static HWND DubLabel(HWND parent, HINSTANCE hInst, const wchar_t* text, HFONT font, DWORD extraStyle)
{
    HWND h = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS | extraStyle,
        0, 0, 10, 10, parent, NULL, hInst, NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

static HWND DubEdit(HWND parent, HINSTANCE hInst, DWORD style, HFONT font)
{
    HWND h = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_READONLY | style,
        0, 0, 10, 10, parent, NULL, hInst, NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

/* ---- "About" credit popup, opened by the (i) in the top right ---- */

static HWND g_hDubCreditPopup = NULL, g_hDubCreditOwner = NULL, g_hDubCreditLink = NULL;
static HFONT g_dubLinkFont = NULL;

static void DubCloseCredits(HWND hwnd)
{
    HWND owner = g_hDubCreditOwner;
    g_hDubCreditPopup = NULL;
    g_hDubCreditLink = NULL;
    g_hDubCreditOwner = NULL;
    if (owner) EnableWindow(owner, TRUE);
    DestroyWindow(hwnd);
    if (owner) SetForegroundWindow(owner);
}

static LRESULT CALLBACK DubCreditWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH b = CreateSolidBrush(g_darkMode ? COLOR_DARK_BG : COLOR_LIGHT_BG);
        FillRect(hdc, &rc, b);
        DeleteObject(b);
        return 1;
    }
    case WM_DRAWITEM:
        if (((LPDRAWITEMSTRUCT)lParam)->CtlType == ODT_BUTTON) {
            DrawThemedOwnerButton((LPDRAWITEMSTRUCT)lParam);
            return TRUE;
        }
        return FALSE;
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, (HWND)lParam == g_hDubCreditLink ? DubKindColor(DUBC_ACCENT)
                                                            : (g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT));
        return (LRESULT)(g_hBrushBg ? g_hBrushBg : GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_SETCURSOR:
        if ((HWND)wParam == g_hDubCreditLink && g_hDubCreditLink) {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_DUB_CREDIT_LINK:
            ShellExecuteW(hwnd, L"open", DUB_CREDIT_URL, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        case ID_DUB_CREDIT_CLOSE:
            DubCloseCredits(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DubCloseCredits(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void DubShowCredits(HWND hDubWnd)
{
    if (g_hDubCreditPopup) { SetForegroundWindow(g_hDubCreditPopup); return; }
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hDubWnd, GWLP_HINSTANCE);
    HWND hOwner = GetAncestor(hDubWnd, GA_ROOT);

    static BOOL classRegistered = FALSE;
    const wchar_t CLASS_NAME[] = L"CVLauncherDubCreditWnd";
    if (!classRegistered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc   = DubCreditWndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = CLASS_NAME;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
        RegisterClassW(&wc);
        classRegistered = TRUE;
    }

    const int w = 400, h = 250;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    RECT rc = {0, 0, w, h};
    AdjustWindowRect(&rc, style, FALSE);
    int winW = rc.right - rc.left, winH = rc.bottom - rc.top;
    RECT prc; GetWindowRect(hOwner, &prc);
    int posX = prc.left + ((prc.right - prc.left) - winW) / 2;
    int posY = prc.top + ((prc.bottom - prc.top) - winH) / 2;

    g_hDubCreditOwner = hOwner;
    HWND hPop = CreateWindowExW(WS_EX_DLGMODALFRAME, CLASS_NAME, L"About the Dub Renderer",
        style, posX, posY, winW, winH, hOwner, NULL, hInst, NULL);
    if (!hPop) { g_hDubCreditOwner = NULL; return; }
    g_hDubCreditPopup = hPop;
    ApplyDarkTitlebar(hPop, g_darkMode);

    HWND hMsg = CreateWindowW(L"STATIC",
        L"The code used to make this section of the application was made possible by "
        L"TDTLuv's Choicer Voicer Dub Renderer's source code.",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 20, w - 40, 80, hPop, NULL, hInst, NULL);
    SendMessageW(hMsg, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);

    HWND hLabel = CreateWindowW(L"STATIC", L"Original page:",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 108, w - 40, 20, hPop, NULL, hInst, NULL);
    SendMessageW(hLabel, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);

    if (!g_dubLinkFont) {
        LOGFONTW lf;
        if (GetObjectW(g_fontRegular, sizeof(lf), &lf)) {
            lf.lfUnderline = TRUE;
            g_dubLinkFont = CreateFontIndirectW(&lf);
        }
    }
    g_hDubCreditLink = CreateWindowW(L"STATIC", DUB_CREDIT_URL,
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY, 20, 130, w - 40, 22, hPop,
        (HMENU)(INT_PTR)ID_DUB_CREDIT_LINK, hInst, NULL);
    SendMessageW(g_hDubCreditLink, WM_SETFONT, (WPARAM)(g_dubLinkFont ? g_dubLinkFont : g_fontRegular), TRUE);

    HWND hClose = MakeButton(hPop, hInst, L"Close", w - 20 - 100, h - 60, 100, 32, ID_DUB_CREDIT_CLOSE, g_fontRegular);

    EnableWindow(hOwner, FALSE);
    ShowWindow(hPop, SW_SHOW);
    UpdateWindow(hPop);
    SetForegroundWindow(hPop);
    SetFocus(hClose);
}

/* The sidebar's (i) art is a 192px image; shrunk to this button's size it loses the thin
   stem and only the dot survives. So draw it from scratch instead: render at 4x into a
   memory bitmap, then halftone-shrink it for smooth edges. */
static void DubDrawInfoIcon(HDC hdc, RECT rc, BOOL hovered, BOOL pressed)
{
    const int S = 4;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int W = w * S, H = h * S;
    COLORREF bgc    = g_darkMode ? COLOR_DARK_BG : COLOR_LIGHT_BG;
    BtnColors bc = ThemeBtnColors();
    COLORREF fill = bc.fill, border = bc.border, glyph = bc.glyph;
    if (pressed) border = bc.accent;
    else if (hovered) border = bc.hoverBorder;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    HBRUSH bgBrush = CreateSolidBrush(bgc);
    RECT all = { 0, 0, W, H };
    FillRect(mem, &all, bgBrush);
    DeleteObject(bgBrush);

    int bw = (int)(W * 0.075);                       /* outer ring thickness */
    HPEN borderPen = CreatePen(PS_SOLID, bw, border);
    HBRUSH fillBrush = CreateSolidBrush(fill);
    HGDIOBJ op = SelectObject(mem, borderPen);
    HGDIOBJ ob = SelectObject(mem, fillBrush);
    Ellipse(mem, bw / 2, bw / 2, W - bw / 2, H - bw / 2);

    int cx = W / 2, cy = H / 2;
    int inset = (int)(W * 0.20);
    HPEN glyphPen = CreatePen(PS_SOLID, (int)(W * 0.07), glyph);
    SelectObject(mem, glyphPen);
    SelectObject(mem, GetStockObject(NULL_BRUSH));
    Ellipse(mem, inset, inset, W - inset, H - inset);     /* inner ring */

    HBRUSH glyphBrush = CreateSolidBrush(glyph);
    SelectObject(mem, GetStockObject(NULL_PEN));
    SelectObject(mem, glyphBrush);
    int dotR = (int)(W * 0.05);
    int dotY = cy - (int)(W * 0.13);
    Ellipse(mem, cx - dotR, dotY - dotR, cx + dotR, dotY + dotR);        /* dot */
    int stemHalf = (int)(W * 0.035);
    RoundRect(mem, cx - stemHalf, cy - (int)(W * 0.04), cx + stemHalf, cy + (int)(W * 0.20),
        stemHalf, stemHalf);                                              /* stem */

    SelectObject(mem, ob);
    SelectObject(mem, op);
    DeleteObject(borderPen); DeleteObject(glyphPen); DeleteObject(fillBrush); DeleteObject(glyphBrush);

    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);
    StretchBlt(hdc, rc.left, rc.top, w, h, mem, 0, 0, W, H, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static BOOL DubIsEditCtl(HWND h)
{
    return h && (h == g_hDubRecEdit || h == g_hDubPackEdit || h == g_hDubOutEdit || h == g_hDubLog);
}

static LRESULT CALLBACK DubWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
        if (!g_dubProcCsInit) { InitializeCriticalSection(&g_dubProcCs); g_dubProcCsInit = TRUE; }
        memset(g_dubStatusKind, 0, sizeof(g_dubStatusKind));

        g_hDubTitle = DubLabel(hwnd, hInst, L"Dub Renderer", g_fontTitle, 0);
        g_hDubInfoBtn = MakeButton(hwnd, hInst, L"", 0, 0, 32, 32, ID_DUB_INFO, g_fontRegular);
        g_hDubSubtitle = DubLabel(hwnd, hInst, L"Turn an exported Choicer Voicer recording into a finished, dubbed MP4.", g_fontSmall, 0);

        g_hDubCombo = CreateWindowW(L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_DUB_COMBO, hInst, NULL);
        SendMessageW(g_hDubCombo, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);
        SetWindowTheme(g_hDubCombo, g_darkMode ? L"DarkMode_Explorer" : L"Explorer", NULL);
        SendMessageW(g_hDubCombo, CB_ADDSTRING, 0, (LPARAM)L"Scanning exports...");
        SendMessageW(g_hDubCombo, CB_SETCURSEL, 0, 0);

        g_hDubRefreshBtn = MakeButton(hwnd, hInst, L"Refresh", 0, 0, 10, 10, ID_DUB_REFRESH, g_fontRegular);
        g_hDubStatus[DST_SCAN] = DubLabel(hwnd, hInst, L"Scanning exports...", g_fontSmall, 0);

        g_hDubCapRec = DubLabel(hwnd, hInst, L"Exported recording folder", g_fontSmall, 0);
        g_hDubRecEdit = DubEdit(hwnd, hInst, ES_AUTOHSCROLL, g_fontRegular);
        g_hDubBrowseRecBtn = MakeButton(hwnd, hInst, L"Choose folder", 0, 0, 10, 10, ID_DUB_BROWSE_REC, g_fontRegular);

        g_hDubCapPack = DubLabel(hwnd, hInst, L"Original pack folder  (.txt or .ini timestamps)", g_fontSmall, 0);
        g_hDubPackEdit = DubEdit(hwnd, hInst, ES_AUTOHSCROLL, g_fontRegular);
        g_hDubBrowsePackBtn = MakeButton(hwnd, hInst, L"Choose folder", 0, 0, 10, 10, ID_DUB_BROWSE_PACK, g_fontRegular);
        g_hDubStatus[DST_PACK] = DubLabel(hwnd, hInst, L"No export selected yet.", g_fontSmall, 0);

        g_hDubCapOut = DubLabel(hwnd, hInst, L"Output MP4", g_fontSmall, 0);
        g_hDubOutEdit = DubEdit(hwnd, hInst, ES_AUTOHSCROLL, g_fontRegular);
        g_hDubBrowseOutBtn = MakeButton(hwnd, hInst, L"Choose file", 0, 0, 10, 10, ID_DUB_BROWSE_OUT, g_fontRegular);

        g_hDubRenderBtn = MakeButton(hwnd, hInst, L"Render MP4", 0, 0, 10, 10, ID_DUB_RENDER, g_fontRegular);
        g_hDubCancelBtn = MakeButton(hwnd, hInst, L"Cancel", 0, 0, 10, 10, ID_DUB_CANCEL, g_fontRegular);
        g_hDubOpenBtn = MakeButton(hwnd, hInst, L"Open output", 0, 0, 10, 10, ID_DUB_OPEN, g_fontRegular);
        g_hDubStatus[DST_FF] = DubLabel(hwnd, hInst, L"FFmpeg: not checked yet", g_fontSmall, SS_RIGHT);
        g_hDubStatus[DST_RENDER] = DubLabel(hwnd, hInst, L"Select an export to begin.", g_fontSmall, 0);

        /* the log is plain monospace on purpose - FFmpeg's output is hard to read in a display face */
        static HFONT logFont = NULL;
        if (!logFont) logFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        g_hDubLog = DubEdit(hwnd, hInst, ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, logFont ? logFont : g_fontSmall);
        SendMessageW(g_hDubLog, EM_SETLIMITTEXT, 0x7FFFFFF0, 0);
        SetWindowTheme(g_hDubLog, g_darkMode ? L"DarkMode_Explorer" : L"Explorer", NULL);

        g_dubStatusKind[DST_SCAN] = DUBC_MUTED;
        g_dubStatusKind[DST_PACK] = DUBC_MUTED;
        g_dubStatusKind[DST_RENDER] = DUBC_MUTED;
        g_dubStatusKind[DST_FF] = DUBC_WARNING;
        g_dubLastOutput[0] = 0;
        DubLayout(hwnd);
        DubUpdateButtons();
        return 0;
    }

    case WM_SIZE:
        DubLayout(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(g_darkMode ? COLOR_DARK_BG : COLOR_LIGHT_BG);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        for (int i = 0; i < 3; i++) DubPaintHeader(hdc, &g_dubHdrRect[i], g_dubHdrText[i]);
        DubPaintBar(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wParam == DUB_TIMER_ID) {
            g_dubBarTick++;
            InvalidateRect(hwnd, &g_dubBarRect, FALSE);
        }
        return 0;

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
        if (dis->CtlType != ODT_BUTTON) return FALSE;
        if (dis->CtlID == ID_DUB_INFO) {
            DubDrawInfoIcon(dis->hDC, dis->rcItem, GetPropW(dis->hwndItem, L"CVHover") != NULL,
                (dis->itemState & ODS_SELECTED) != 0);
            return TRUE;
        }
        DrawThemedOwnerButton(dis);
        return TRUE;
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND ctl = (HWND)lParam;
        if (msg == WM_CTLCOLOREDIT || DubIsEditCtl(ctl)) {
            /* read-only edits report as "static"; give every text box the panel colors */
            SetBkColor(hdc, g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL);
            SetTextColor(hdc, g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT);
            return (LRESULT)(g_hBrushPanel ? g_hBrushPanel : GetSysColorBrush(COLOR_WINDOW));
        }
        SetBkMode(hdc, TRANSPARENT);
        COLORREF col = g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT;
        if (ctl == g_hDubSubtitle || ctl == g_hDubCapRec || ctl == g_hDubCapPack || ctl == g_hDubCapOut) col = DubKindColor(DUBC_MUTED);
        for (int i = 0; i < DST_COUNT; i++) if (ctl == g_hDubStatus[i]) col = DubKindColor(g_dubStatusKind[i]);
        SetTextColor(hdc, col);
        return (LRESULT)(g_hBrushBg ? g_hBrushBg : GetSysColorBrush(COLOR_BTNFACE));
    }

    case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT);
        return (LRESULT)(g_hBrushPanel ? g_hBrushPanel : GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_DUB_COMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(g_hDubCombo, CB_GETCURSEL, 0, 0);
                if (sel >= 1 && sel - 1 < g_dubExportCount) DubApplyRecording(g_dubExports[sel - 1].path);
            }
            return 0;
        case ID_DUB_REFRESH: DubStartScan(); return 0;
        case ID_DUB_BROWSE_REC: {
            wchar_t init[DUB_PATH], picked[DUB_PATH];
            if (!DubAppPath(init, DUB_PATH, L"recordings\\dub_recordings")) init[0] = 0;
            if (DubBrowseFolder(L"Choose the folder containing _dubrecord_*.wav files", init, picked, DUB_PATH))
                DubApplyRecording(picked);
            return 0;
        }
        case ID_DUB_BROWSE_PACK: {
            wchar_t init[DUB_PATH], picked[DUB_PATH];
            if (!DubAppPath(init, DUB_PATH, L"packs_voice")) init[0] = 0;
            if (DubBrowseFolder(L"Choose the matching original Choicer Voicer pack folder", init, picked, DUB_PATH)) {
                if (DubValidPackFolder(picked)) {
                    SetWindowTextW(g_hDubPackEdit, picked);
                    DubSetStatus(DST_PACK, DUBC_SUCCESS, L"Valid pack selected (.txt or .ini metadata).");
                } else {
                    MessageBoxW(DubMsgOwner(), L"That folder must contain dub_video.* and matching timestamp metadata in .txt or .ini files.",
                        L"Invalid pack folder", MB_OK | MB_ICONWARNING);
                }
            }
            return 0;
        }
        case ID_DUB_BROWSE_OUT: {
            wchar_t cur[DUB_PATH], picked[DUB_PATH];
            GetWindowTextW(g_hDubOutEdit, cur, DUB_PATH);
            if (DubBrowseOutput(cur, picked, DUB_PATH)) SetWindowTextW(g_hDubOutEdit, picked);
            return 0;
        }
        case ID_DUB_RENDER: DubStartRender(); return 0;
        case ID_DUB_CANCEL: DubCancelRender(); return 0;
        case ID_DUB_OPEN:   DubOpenOutput(); return 0;
        case ID_DUB_INFO:   DubShowCredits(hwnd); return 0;
        }
        break;

    case WM_DUB_LOG:
        if (lParam) { DubAppendLog((const wchar_t*)lParam); free((void*)lParam); }
        return 0;

    case WM_DUB_STATUS:
        if (lParam) {
            /* progress updates can arrive just after a render ends - ignore those */
            if (g_dubRendering) DubSetStatus(DST_RENDER, (int)wParam, (const wchar_t*)lParam);
            free((void*)lParam);
        }
        return 0;

    case WM_DUB_SCANDONE:
        DubFinishScan((DubScanResult*)lParam);
        return 0;

    case WM_DUB_FFCHECK:
        DubFinishFfCheck((int)wParam, (wchar_t*)lParam);
        free((void*)lParam);
        return 0;

    case WM_DUB_FFINSTALL:
        DubFinishFfInstall((BOOL)wParam, (wchar_t*)lParam);
        free((void*)lParam);
        return 0;

    case WM_DUB_RENDERDONE:
        DubFinishRender((int)wParam, (wchar_t*)lParam);
        free((void*)lParam);
        return 0;

    case WM_DESTROY:
        InterlockedExchange(&g_dubCancel, 1);
        DubKillRunningProcess();
        KillTimer(hwnd, DUB_TIMER_ID);
        free(g_dubExports);
        g_dubExports = NULL;
        g_dubExportCount = 0;
        g_hDubWnd = NULL;       /* worker threads stop posting once this is NULL */
        g_hDubCombo = NULL;
        g_hDubLog = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static HWND CreateDubRenderWnd(HWND parent, HINSTANCE hInst, int x, int y, int w, int h)
{
    static BOOL classRegistered = FALSE;
    const wchar_t CLASS_NAME[] = L"CVLauncherDubRenderWnd";
    if (!classRegistered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc   = DubWndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = CLASS_NAME;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        RegisterClassW(&wc);
        classRegistered = TRUE;
    }
    HWND hwnd = CreateWindowExW(0, CLASS_NAME, NULL,
        WS_CHILD | WS_CLIPCHILDREN,
        x, y, w, h, parent, NULL, hInst, NULL);
    g_hDubWnd = hwnd;
    return hwnd;
}

/* hand-drawn microphone for the sidebar button (used until custom icon art is supplied) */
static void DrawMicGlyph(HDC hdc, RECT rc, COLORREF color)
{
    int cx = (rc.left + rc.right) / 2;
    int cy = (rc.top + rc.bottom) / 2;
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HGDIOBJ ob = SelectObject(hdc, brush);
    HGDIOBJ op = SelectObject(hdc, pen);
    RoundRect(hdc, cx - 6, cy - 15, cx + 6, cy + 3, 12, 12);          /* capsule */
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Arc(hdc, cx - 11, cy - 9, cx + 11, cy + 9, cx - 11, cy, cx + 11, cy);   /* cradle */
    MoveToEx(hdc, cx, cy + 9, NULL); LineTo(hdc, cx, cy + 14);        /* stem */
    MoveToEx(hdc, cx - 6, cy + 14, NULL); LineTo(hdc, cx + 7, cy + 14);   /* base */
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(pen);
    DeleteObject(brush);
}
