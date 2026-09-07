#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <oleidl.h>
#include <wininet.h>
#include <dwmapi.h>
#include <uxtheme.h>
/* forces the WIC GUIDs (CLSID_WICImagingFactory etc.) used for decoding
   downloaded mod thumbnails to be defined directly in this object file,
   rather than relying on them being present in the MinGW distribution's
   libuuid.a - some older/leaner distributions don't carry them, and this
   avoids that link-time gamble entirely. Scoped to just before wincodec.h
   so it doesn't touch how the already-working COM GUIDs used elsewhere
   in this file (IID_IDropTarget, etc.) get resolved. */
#include <initguid.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wchar.h>
#include <wctype.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "miniz.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

/* ---- control / resource IDs ---- */
#define ID_LAUNCH_NORMAL   1001
#define ID_LAUNCH_COMPAT   1002
#define ID_MODDING          1005
#define ID_GET_MODS         1006
#define IDC_DARKMODE_CHECK  1007

#define IDC_NORMAL_COMBO  3001
#define IDC_COMPAT_COMBO  3002
#define IDC_PACK_COMBO    2001
#define IDC_DROPZONE      2002

#define ID_README_OPEN    4001
#define ID_README_CLOSE   4002
#define ID_README_INFO    4003

#define ID_TAB_LAUNCHER    5001
#define ID_TAB_BROWSE      5002

#define IDB_SIDEIMAGE   100
#define IDI_APPICON     101
#define IDB_BGLIGHT     102
#define IDB_BGDARK      103

/* ---- folders (relative to the launcher's own exe) that hold versions ---- */
static const wchar_t* NORMAL_DIR_NAME = L"Normal";
static const wchar_t* COMPAT_DIR_NAME = L"Compatibility";

static const wchar_t* GAME_DIR_SUBPATH = L"\\YeahMaybe\\ChoicerVoicer\\game";
static const wchar_t* GAMEBANANA_URL = L"https://gamebanana.com/games/20674";
/* must stay in sync with the numeric suffix of GAMEBANANA_URL above */
static const long GAMEBANANA_GAME_ID = 20674;
/* "Dub Mode" - confirmed via two real captured requests (both sort
   variants) where every single returned record, 30 for 30, had
   _aGame._idRow == GAMEBANANA_GAME_ID. Unlike _aFilters[Generic_Game],
   which never reliably scoped results in testing, this actually works -
   this game's mods all live under this one root category. Used for the
   Mod/Index browse/sort fetch; Util/Search/Results still uses
   _idGameRow directly, since that's the parameter its own real captured
   request used and there's no evidence yet either way on whether it'd
   accept a category filter too. */
static const long GAMEBANANA_CATEGORY_ID = 44064;
static const wchar_t* README_URL =
    L"https://github.com/notverzee/The-Choicer-Voicer-Launcher/blob/main/README.md";

static const wchar_t* PACK_FOLDERS[] = {
    L"packs_chatter", L"packs_host", L"packs_judges",
    L"packs_menu", L"packs_player", L"packs_studio", L"packs_voice"
};
#define NUM_PACK_FOLDERS 7

/* ---- dark / light palette ---- */
#define COLOR_DARK_BG      RGB(32,32,32)
#define COLOR_DARK_PANEL   RGB(45,45,48)
#define COLOR_DARK_PRESSED RGB(60,60,64)
#define COLOR_DARK_TEXT    RGB(230,230,230)
#define COLOR_DARK_BORDER  RGB(75,75,75)
#define COLOR_LIGHT_BG     RGB(240,240,240)
#define COLOR_LIGHT_PANEL  RGB(255,255,255)
#define COLOR_LIGHT_PRESSED RGB(222,222,222)
#define COLOR_LIGHT_TEXT   RGB(20,20,20)
#define COLOR_LIGHT_BORDER RGB(170,170,170)

static HFONT g_fontTitle    = NULL;
static HFONT g_fontRegular  = NULL;
static HFONT g_fontSmall    = NULL;
static HBITMAP g_hBitmap    = NULL;
static HWND   g_hPackCombo  = NULL;
static HWND   g_hDarkModeCheck = NULL;
static RECT   g_dropZoneRect = {0, 0, 0, 0};

static HBRUSH g_hBrushBg  = NULL;
static HBRUSH g_hBrushPanel = NULL;
static BOOL   g_darkMode = FALSE;
static BOOL   g_readmeShown = FALSE;
static HINSTANCE g_hInstance = NULL;

/* ---- tabs: "Launcher" (the existing UI) vs "Browse Mods" (new) ---- */
typedef enum { TAB_LAUNCHER = 0, TAB_BROWSE = 1 } AppTab;
static AppTab g_activeTab = TAB_LAUNCHER;
static HWND   g_hTabLauncherBtn = NULL;
static HWND   g_hTabBrowseBtn = NULL;

/* every real child window that belongs to the Launcher tab's content (not
   the header/tabs, which stay visible on both tabs) gets tracked here so
   switching tabs is just a show/hide loop over this list */
#define MAX_LAUNCHER_PAGE_WNDS 24
static HWND g_launcherPageWnds[MAX_LAUNCHER_PAGE_WNDS];
static int  g_launcherPageWndCount = 0;
static void TrackLauncherPageWnd(HWND h)
{
    if (h && g_launcherPageWndCount < MAX_LAUNCHER_PAGE_WNDS) {
        g_launcherPageWnds[g_launcherPageWndCount++] = h;
    }
}

/* ---- animated backdrop: tinted image + slow pulsing/drifting stars ---- */
#define NUM_STARS 40
#define ANIM_TIMER_ID 1
#define ANIM_INTERVAL_MS 50

typedef struct {
    float x, y;
    float vx, vy;
    float phase;
    float phaseSpeed;
    float baseSize;
} Star;

static Star g_stars[NUM_STARS];
static HBITMAP g_hBgLightBitmap = NULL;
static HBITMAP g_hBgDarkBitmap  = NULL;
static HDC     g_hMemDC = NULL;
static HBITMAP g_hMemBitmap = NULL;
static int     g_bufW = 0, g_bufH = 0;

/* Plain text captions (title/subtitle/labels/tip text - anything that
   isn't inside a bordered box) are drawn directly onto the animated
   backdrop buffer instead of being separate child STATIC controls. This
   is what gives them a genuinely transparent background (the animated
   stars show straight through, no solid highlight rectangle behind the
   text) - bordered "boxes" like the drop zone and the mode cards are
   untouched, real child windows, and keep their solid panel fill. */
typedef struct {
    const wchar_t* text;
    RECT rect;
    HFONT font;
    BOOL visible;
} OverlayLabel;

#define NUM_OVERLAY_LABELS 4
static OverlayLabel g_overlayLabels[NUM_OVERLAY_LABELS];

/* ---- dynamically discovered game versions ---- */
typedef struct {
    wchar_t displayName[256];
    wchar_t exePath[MAX_PATH];
} VersionEntry;

typedef struct {
    VersionEntry* items;
    int count;
    int capacity;
} VersionList;

static VersionList g_normalVersions;
static VersionList g_compatVersions;
static HWND g_hNormalCombo = NULL;
static HWND g_hCompatCombo = NULL;
static HWND g_hLaunchNormalBtn = NULL;
static HWND g_hLaunchCompatBtn = NULL;

/* ---- persisted settings (last version played per mode, dark mode) ---- */
static wchar_t g_lastNormalExe[MAX_PATH] = L"";
static wchar_t g_lastCompatExe[MAX_PATH] = L"";
static wchar_t g_lastPackFolder[64] = L"";

/* ===================== small path/file helpers ===================== */

static void GetExeDir(wchar_t* buf, DWORD bufSize)
{
    GetModuleFileNameW(NULL, buf, bufSize);
    wchar_t* lastSlash = wcsrchr(buf, L'\\');
    if (lastSlash) *lastSlash = L'\0';
}

static void GetParentDirW(const wchar_t* path, wchar_t* outDir)
{
    wcscpy(outDir, path);
    wchar_t* lastSlash = wcsrchr(outDir, L'\\');
    if (lastSlash) *lastSlash = 0;
}

static BOOL GetAppDataGameDir(wchar_t* out)
{
    wchar_t appdata[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return FALSE;
    wsprintfW(out, L"%s%s", appdata, GAME_DIR_SUBPATH);
    return TRUE;
}

static BOOL PathExistsW(const wchar_t* path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static BOOL PathIsDirW(const wchar_t* path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL EnsureDirW(const wchar_t* path)
{
    int r = SHCreateDirectoryExW(NULL, path, NULL);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS;
}

static BOOL DeleteDirectoryRecursiveW(const wchar_t* path)
{
    wchar_t search[MAX_PATH];
    wsprintfW(search, L"%s\\*", path);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return RemoveDirectoryW(path) || GetLastError() == ERROR_FILE_NOT_FOUND;
    }

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        wchar_t full[MAX_PATH];
        wsprintfW(full, L"%s\\%s", path, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            DeleteDirectoryRecursiveW(full);
        } else {
            SetFileAttributesW(full, FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    return RemoveDirectoryW(path);
}

static BYTE* ReadEntireFileW(const wchar_t* path, DWORD* outSize)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > 0x40000000LL) {
        CloseHandle(h);
        return NULL;
    }

    DWORD sz = (DWORD)size.QuadPart;
    BYTE* buf = (BYTE*)malloc(sz);
    if (!buf) { CloseHandle(h); return NULL; }

    DWORD readBytes = 0;
    BOOL ok = ReadFile(h, buf, sz, &readBytes, NULL);
    CloseHandle(h);
    if (!ok || readBytes != sz) { free(buf); return NULL; }

    *outSize = sz;
    return buf;
}

static void MakeUniquePathW(const wchar_t* basePath, wchar_t* outPath)
{
    wcscpy(outPath, basePath);
    int n = 2;
    while (PathExistsW(outPath)) {
        wsprintfW(outPath, L"%s (%d)", basePath, n++);
    }
}

static BOOL DecodeUtf8ToWideBuf(const BYTE* buf, DWORD size, wchar_t* out, int outCap)
{
    const BYTE* p = buf;
    DWORD n = size;
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { p += 3; n -= 3; }
    int wn = MultiByteToWideChar(CP_UTF8, 0, (const char*)p, (int)n, out, outCap - 1);
    if (wn <= 0) wn = MultiByteToWideChar(CP_ACP, 0, (const char*)p, (int)n, out, outCap - 1);
    if (wn <= 0) return FALSE;
    out[wn] = 0;
    return TRUE;
}

/* ===================== settings persistence ===================== */

static void GetSettingsPath(wchar_t* out)
{
    wchar_t exeDir[MAX_PATH];
    GetExeDir(exeDir, MAX_PATH);
    wsprintfW(out, L"%s\\launcher_settings.ini", exeDir);
}

static void LoadSettings(void)
{
    g_lastNormalExe[0] = 0;
    g_lastCompatExe[0] = 0;
    g_lastPackFolder[0] = 0;
    g_darkMode = FALSE;
    g_readmeShown = FALSE;

    wchar_t path[MAX_PATH];
    GetSettingsPath(path);
    DWORD size = 0;
    BYTE* buf = ReadEntireFileW(path, &size);
    if (!buf) return;

    wchar_t* text = (wchar_t*)malloc(((size_t)size + 1) * sizeof(wchar_t));
    if (!text) { free(buf); return; }
    BOOL ok = DecodeUtf8ToWideBuf(buf, size, text, (int)size + 1);
    free(buf);
    if (!ok) { free(text); return; }

    wchar_t* p = text;
    while (*p) {
        wchar_t* lineStart = p;
        while (*p && *p != L'\n') p++;
        wchar_t* lineEnd = p;
        if (*p == L'\n') p++;
        while (lineEnd > lineStart && (lineEnd[-1] == L'\r' || lineEnd[-1] == L' ')) lineEnd--;
        *lineEnd = 0;

        if (_wcsnicmp(lineStart, L"NormalLastExe=", 14) == 0) {
            wcsncpy(g_lastNormalExe, lineStart + 14, MAX_PATH - 1);
            g_lastNormalExe[MAX_PATH - 1] = 0;
        } else if (_wcsnicmp(lineStart, L"CompatLastExe=", 14) == 0) {
            wcsncpy(g_lastCompatExe, lineStart + 14, MAX_PATH - 1);
            g_lastCompatExe[MAX_PATH - 1] = 0;
        } else if (_wcsnicmp(lineStart, L"DarkMode=", 9) == 0) {
            g_darkMode = (lineStart[9] == L'1');
        } else if (_wcsnicmp(lineStart, L"ReadmeShown=", 12) == 0) {
            g_readmeShown = (lineStart[12] == L'1');
        } else if (_wcsnicmp(lineStart, L"PackFolder=", 11) == 0) {
            wcsncpy(g_lastPackFolder, lineStart + 11, 63);
            g_lastPackFolder[63] = 0;
        }
    }
    free(text);
}

static void SaveSettings(void)
{
    wchar_t path[MAX_PATH];
    GetSettingsPath(path);

    wchar_t content[3 * MAX_PATH + 128];
    wsprintfW(content, L"NormalLastExe=%s\r\nCompatLastExe=%s\r\nDarkMode=%d\r\nReadmeShown=%d\r\nPackFolder=%s\r\n",
        g_lastNormalExe, g_lastCompatExe, g_darkMode ? 1 : 0, g_readmeShown ? 1 : 0, g_lastPackFolder);

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    int len = (int)wcslen(content);
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, content, len, NULL, 0, NULL, NULL);
    char* utf8 = (char*)malloc((size_t)utf8Len);
    if (utf8) {
        WideCharToMultiByte(CP_UTF8, 0, content, len, utf8, utf8Len, NULL, NULL);
        DWORD written = 0;
        WriteFile(h, utf8, (DWORD)utf8Len, &written, NULL);
        free(utf8);
    }
    CloseHandle(h);
}

static void SelectRememberedVersion(HWND combo, VersionList* list, const wchar_t* rememberedExe)
{
    if (!rememberedExe[0] || list->count == 0) return;
    for (int i = 0; i < list->count; i++) {
        if (_wcsicmp(list->items[i].exePath, rememberedExe) == 0) {
            SendMessageW(combo, CB_SETCURSEL, (WPARAM)i, 0);
            return;
        }
    }
}

/* ===================== animated backdrop ===================== */

static void InitStars(int clientW, int clientH)
{
    srand(GetTickCount());
    for (int i = 0; i < NUM_STARS; i++) {
        g_stars[i].x = (float)(rand() % (clientW > 0 ? clientW : 1));
        g_stars[i].y = (float)(rand() % (clientH > 0 ? clientH : 1));
        g_stars[i].vx = ((float)(rand() % 200) - 100.0f) / 4000.0f;
        g_stars[i].vy = ((float)(rand() % 200) - 100.0f) / 4000.0f;
        g_stars[i].phase = (float)(rand() % 628) / 100.0f;
        g_stars[i].phaseSpeed = 0.015f + (float)(rand() % 25) / 1000.0f;
        g_stars[i].baseSize = 1.8f + (float)(rand() % 30) / 10.0f;
    }
}

static void UpdateStars(int clientW, int clientH)
{
    for (int i = 0; i < NUM_STARS; i++) {
        g_stars[i].x += g_stars[i].vx;
        g_stars[i].y += g_stars[i].vy;
        g_stars[i].phase += g_stars[i].phaseSpeed;
        if (g_stars[i].x < -10) g_stars[i].x = (float)clientW + 10;
        if (g_stars[i].x > clientW + 10) g_stars[i].x = -10;
        if (g_stars[i].y < -10) g_stars[i].y = (float)clientH + 10;
        if (g_stars[i].y > clientH + 10) g_stars[i].y = -10;
    }
}

static COLORREF BlendColor(COLORREF a, COLORREF b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int r = (int)(GetRValue(a) * (1.0f - t) + GetRValue(b) * t);
    int g = (int)(GetGValue(a) * (1.0f - t) + GetGValue(b) * t);
    int bl = (int)(GetBValue(a) * (1.0f - t) + GetBValue(b) * t);
    return RGB(r, g, bl);
}

/* Draws the tinted backdrop + all stars into g_hMemDC, then blits it once
   to the real DC - single blit avoids the flicker that direct per-shape
   drawing would cause. */
static void PaintAnimatedBackdrop(HDC hdc, int w, int h)
{
    if (!g_hMemDC || g_bufW != w || g_bufH != h) {
        if (g_hMemDC) { DeleteDC(g_hMemDC); g_hMemDC = NULL; }
        if (g_hMemBitmap) { DeleteObject(g_hMemBitmap); g_hMemBitmap = NULL; }
        g_hMemDC = CreateCompatibleDC(hdc);
        g_hMemBitmap = CreateCompatibleBitmap(hdc, w, h);
        SelectObject(g_hMemDC, g_hMemBitmap);
        g_bufW = w; g_bufH = h;
    }

    HBITMAP bgBmp = g_darkMode ? g_hBgDarkBitmap : g_hBgLightBitmap;
    if (bgBmp) {
        BITMAP bm;
        GetObjectW(bgBmp, sizeof(bm), &bm);
        HDC bgDC = CreateCompatibleDC(hdc);
        HGDIOBJ oldBgBmp = SelectObject(bgDC, bgBmp);
        /* Stretch the (possibly smaller/differently-sized) source bitmap to
           fully cover the current client area - without this, resizing the
           window (or the window simply being larger than the bitmap's
           original dimensions) leaves the backdrop cut off / not covering
           the full window. HALFTONE gives much better scaling quality than
           the default for a blurred backdrop image. */
        SetStretchBltMode(g_hMemDC, HALFTONE);
        SetBrushOrgEx(g_hMemDC, 0, 0, NULL);
        StretchBlt(g_hMemDC, 0, 0, w, h, bgDC, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
        SelectObject(bgDC, oldBgBmp);
        DeleteDC(bgDC);
    } else {
        RECT full = {0, 0, w, h};
        FillRect(g_hMemDC, &full, g_darkMode ? g_hBrushBg : g_hBrushBg);
    }

    COLORREF bgTint = g_darkMode ? COLOR_DARK_BG : COLOR_LIGHT_BG;
    COLORREF starCore = g_darkMode ? RGB(220,225,255) : RGB(130,142,182);

    HGDIOBJ oldPen = SelectObject(g_hMemDC, GetStockObject(NULL_PEN));
    for (int i = 0; i < NUM_STARS; i++) {
        float size = g_stars[i].baseSize + sinf(g_stars[i].phase) * g_stars[i].baseSize * 0.6f;
        if (size < 0.8f) size = 0.8f;
        int x = (int)g_stars[i].x, y = (int)g_stars[i].y;

        HBRUSH hHalo = CreateSolidBrush(BlendColor(starCore, bgTint, 0.7f));
        SelectObject(g_hMemDC, hHalo);
        Ellipse(g_hMemDC, (int)(x - size * 1.8f), (int)(y - size * 1.8f),
                          (int)(x + size * 1.8f), (int)(y + size * 1.8f));
        DeleteObject(hHalo);

        HBRUSH hCore = CreateSolidBrush(BlendColor(starCore, bgTint, 0.25f));
        SelectObject(g_hMemDC, hCore);
        Ellipse(g_hMemDC, (int)(x - size), (int)(y - size), (int)(x + size), (int)(y + size));
        DeleteObject(hCore);
    }
    SelectObject(g_hMemDC, oldPen);

    /* draw the plain text captions straight onto the backdrop buffer, with
       a transparent background - no boxed highlight behind them */
    SetBkMode(g_hMemDC, TRANSPARENT);
    SetTextColor(g_hMemDC, g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT);
    for (int i = 0; i < NUM_OVERLAY_LABELS; i++) {
        OverlayLabel* lbl = &g_overlayLabels[i];
        if (!lbl->text || !lbl->visible) continue;
        HGDIOBJ oldLblFont = lbl->font ? SelectObject(g_hMemDC, lbl->font) : NULL;
        RECT r = lbl->rect;
        DrawTextW(g_hMemDC, lbl->text, -1, &r, DT_LEFT | DT_TOP | DT_WORDBREAK);
        if (oldLblFont) SelectObject(g_hMemDC, oldLblFont);
    }

    BitBlt(hdc, 0, 0, w, h, g_hMemDC, 0, 0, SRCCOPY);
}

/* ===================== version discovery (Normal\ / Compatibility\) ===================== */

static void VersionList_Init(VersionList* list)
{
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void VersionList_Add(VersionList* list, const wchar_t* displayName, const wchar_t* exePath)
{
    if (list->count >= list->capacity) {
        int newCap = list->capacity == 0 ? 8 : list->capacity * 2;
        VersionEntry* newItems = (VersionEntry*)realloc(list->items, (size_t)newCap * sizeof(VersionEntry));
        if (!newItems) return;
        list->items = newItems;
        list->capacity = newCap;
    }
    wcsncpy(list->items[list->count].displayName, displayName, 255);
    list->items[list->count].displayName[255] = 0;
    wcsncpy(list->items[list->count].exePath, exePath, MAX_PATH - 1);
    list->items[list->count].exePath[MAX_PATH - 1] = 0;
    list->count++;
}

static void VersionList_Free(VersionList* list)
{
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static BOOL ReadDisplayNameTxt(const wchar_t* txtPath, wchar_t* out, int outCount)
{
    DWORD size = 0;
    BYTE* buf = ReadEntireFileW(txtPath, &size);
    if (!buf) return FALSE;

    wchar_t decoded[512];
    BOOL ok = DecodeUtf8ToWideBuf(buf, size, decoded, 512);
    free(buf);
    if (!ok) return FALSE;

    int len = (int)wcslen(decoded);
    while (len > 0 && (decoded[len-1] == L'\r' || decoded[len-1] == L'\n' ||
                       decoded[len-1] == L' '  || decoded[len-1] == L'\t')) {
        decoded[--len] = 0;
    }
    int start = 0;
    while (decoded[start] == L' ' || decoded[start] == L'\t') start++;

    wcsncpy(out, decoded + start, (size_t)outCount - 1);
    out[outCount - 1] = 0;
    return out[0] != 0;
}

static void ScanVersionFolder(const wchar_t* versionDir, const wchar_t* folderName, VersionList* outList)
{
    wchar_t exePath[MAX_PATH] = {0};
    wchar_t txtPath[MAX_PATH] = {0};

    wchar_t search[MAX_PATH];
    wsprintfW(search, L"%s\\*", versionDir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            size_t len = wcslen(fd.cFileName);
            if (!exePath[0] && len > 4 && _wcsicmp(fd.cFileName + len - 4, L".exe") == 0) {
                wsprintfW(exePath, L"%s\\%s", versionDir, fd.cFileName);
            } else if (!txtPath[0] && len > 4 && _wcsicmp(fd.cFileName + len - 4, L".txt") == 0) {
                wsprintfW(txtPath, L"%s\\%s", versionDir, fd.cFileName);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    if (!exePath[0]) return;

    wchar_t displayName[256];
    if (!txtPath[0] || !ReadDisplayNameTxt(txtPath, displayName, 256)) {
        wcsncpy(displayName, folderName, 255);
        displayName[255] = 0;
    }

    VersionList_Add(outList, displayName, exePath);
}

static void ScanModeFolder(const wchar_t* modeDir, VersionList* outList)
{
    VersionList_Init(outList);
    if (!PathIsDirW(modeDir)) return;

    wchar_t search[MAX_PATH];
    wsprintfW(search, L"%s\\*", modeDir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;

        wchar_t versionDir[MAX_PATH];
        wsprintfW(versionDir, L"%s\\%s", modeDir, fd.cFileName);
        ScanVersionFolder(versionDir, fd.cFileName, outList);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void PopulateVersionCombo(HWND combo, VersionList* list, HWND launchButton)
{
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    if (list->count == 0) {
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"(no versions found)");
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        EnableWindow(combo, FALSE);
        if (launchButton) EnableWindow(launchButton, FALSE);
        return;
    }
    for (int i = 0; i < list->count; i++) {
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)list->items[i].displayName);
    }
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
    EnableWindow(combo, TRUE);
    if (launchButton) EnableWindow(launchButton, TRUE);
}

/* ===================== game launching ===================== */

static void LaunchSelectedVersion(HWND hwnd, HWND combo, VersionList* list, BOOL isNormalMode)
{
    int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= list->count) {
        MessageBoxW(hwnd, L"No version selected.", L"The Choicer Voicer - Launcher", MB_ICONWARNING | MB_OK);
        return;
    }
    const wchar_t* exePath = list->items[sel].exePath;

    if (!PathExistsW(exePath)) {
        wchar_t msg[600];
        wsprintfW(msg, L"Couldn't find:\n%s", exePath);
        MessageBoxW(hwnd, msg, L"The Choicer Voicer - Launcher", MB_ICONERROR | MB_OK);
        return;
    }

    wchar_t workDir[MAX_PATH];
    GetParentDirW(exePath, workDir);

    HINSTANCE result = ShellExecuteW(hwnd, L"open", exePath, NULL, workDir, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32) {
        wchar_t msg[600];
        wsprintfW(msg, L"Couldn't launch:\n%s\n\nError code: %d", exePath, (int)(INT_PTR)result);
        MessageBoxW(hwnd, msg, L"The Choicer Voicer - Launcher", MB_ICONERROR | MB_OK);
        return;
    }

    /* remember this as the last-opened version for this mode */
    if (isNormalMode) wcscpy(g_lastNormalExe, exePath);
    else wcscpy(g_lastCompatExe, exePath);
    SaveSettings();

    /* launched fine -> close the launcher */
    PostQuitMessage(0);
}

static void OpenModdingFolder(HWND hwnd)
{
    wchar_t fullPath[MAX_PATH];
    if (!GetAppDataGameDir(fullPath)) {
        MessageBoxW(hwnd, L"Couldn't resolve %APPDATA%.",
            L"The Choicer Voicer - Launcher", MB_ICONERROR | MB_OK);
        return;
    }

    if (!PathIsDirW(fullPath)) {
        wchar_t msg[512];
        wsprintfW(msg,
            L"This folder doesn't exist yet:\n%s\n\n"
            L"It's created by the game itself \u2014 run the game at least once first.",
            fullPath);
        MessageBoxW(hwnd, msg, L"The Choicer Voicer - Launcher", MB_ICONINFORMATION | MB_OK);
        return;
    }

    ShellExecuteW(hwnd, L"open", fullPath, NULL, NULL, SW_SHOWNORMAL);
}

static void OpenGetMods(HWND hwnd)
{
    ShellExecuteW(hwnd, L"open", GAMEBANANA_URL, NULL, NULL, SW_SHOWNORMAL);
}

/* ===================== zip extraction (miniz, in-memory) ===================== */

static BOOL ZipEntryPathIsUnsafe(const char* name)
{
    if (name[0] == '/' || name[0] == '\\') return TRUE;
    if (strlen(name) >= 2 && name[1] == ':') return TRUE;
    const char* p = name;
    while (*p) {
        if (p[0] == '.' && p[1] == '.' &&
            (p[2] == '/' || p[2] == '\\' || p[2] == '\0') &&
            (p == name || p[-1] == '/' || p[-1] == '\\')) {
            return TRUE;
        }
        p++;
    }
    return FALSE;
}

static BOOL ExtractZipToDirW(const wchar_t* zipPath, const wchar_t* destDir, wchar_t* errMsg)
{
    DWORD fileSize = 0;
    BYTE* fileBuf = ReadEntireFileW(zipPath, &fileSize);
    if (!fileBuf) {
        wsprintfW(errMsg, L"Couldn't read the archive file.");
        return FALSE;
    }

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, fileBuf, fileSize, 0)) {
        wsprintfW(errMsg, L"That .zip file looks corrupted.");
        free(fileBuf);
        return FALSE;
    }

    int numFiles = (int)mz_zip_reader_get_num_files(&zip);
    BOOL ok = TRUE;

    for (int i = 0; i < numFiles && ok; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) { ok = FALSE; break; }
        if (ZipEntryPathIsUnsafe(st.m_filename)) continue;

        wchar_t wname[512];
        UINT cp = (st.m_bit_flag & (1 << 11)) ? CP_UTF8 : CP_OEMCP;
        int n = MultiByteToWideChar(cp, 0, st.m_filename, -1, wname, 512);
        if (n <= 0) continue;
        for (wchar_t* p = wname; *p; p++) if (*p == L'/') *p = L'\\';

        wchar_t full[MAX_PATH];
        wsprintfW(full, L"%s\\%s", destDir, wname);

        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            EnsureDirW(full);
            continue;
        }

        wchar_t parent[MAX_PATH];
        wcscpy(parent, full);
        wchar_t* lastSlash = wcsrchr(parent, L'\\');
        if (lastSlash) { *lastSlash = 0; EnsureDirW(parent); }

        size_t outSize = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &outSize, 0);
        if (!data) { ok = FALSE; wsprintfW(errMsg, L"Failed to extract:\n%s", wname); break; }

        HANDLE hOut = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hOut == INVALID_HANDLE_VALUE) {
            mz_free(data);
            ok = FALSE;
            wsprintfW(errMsg, L"Couldn't write:\n%s", wname);
            break;
        }
        DWORD written = 0;
        WriteFile(hOut, data, (DWORD)outSize, &written, NULL);
        CloseHandle(hOut);
        mz_free(data);
    }

    mz_zip_reader_end(&zip);
    free(fileBuf);
    return ok;
}

/* ===================== rar extraction (best-effort, via 7-Zip/WinRAR if present) ===================== */

static BOOL FindRarExtractor(wchar_t* outExe, BOOL* outIsSevenZip)
{
    wchar_t pf[MAX_PATH] = {0}, pf86[MAX_PATH] = {0}, candidate[MAX_PATH];
    GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
    GetEnvironmentVariableW(L"ProgramFiles(x86)", pf86, MAX_PATH);

    if (pf[0]) {
        wsprintfW(candidate, L"%s\\7-Zip\\7z.exe", pf);
        if (PathExistsW(candidate)) { wcscpy(outExe, candidate); *outIsSevenZip = TRUE; return TRUE; }
    }
    if (pf86[0]) {
        wsprintfW(candidate, L"%s\\7-Zip\\7z.exe", pf86);
        if (PathExistsW(candidate)) { wcscpy(outExe, candidate); *outIsSevenZip = TRUE; return TRUE; }
    }
    if (pf[0]) {
        wsprintfW(candidate, L"%s\\WinRAR\\UnRAR.exe", pf);
        if (PathExistsW(candidate)) { wcscpy(outExe, candidate); *outIsSevenZip = FALSE; return TRUE; }
    }
    if (pf86[0]) {
        wsprintfW(candidate, L"%s\\WinRAR\\UnRAR.exe", pf86);
        if (PathExistsW(candidate)) { wcscpy(outExe, candidate); *outIsSevenZip = FALSE; return TRUE; }
    }
    if (pf[0]) {
        wsprintfW(candidate, L"%s\\WinRAR\\Rar.exe", pf);
        if (PathExistsW(candidate)) { wcscpy(outExe, candidate); *outIsSevenZip = FALSE; return TRUE; }
    }
    if (pf86[0]) {
        wsprintfW(candidate, L"%s\\WinRAR\\Rar.exe", pf86);
        if (PathExistsW(candidate)) { wcscpy(outExe, candidate); *outIsSevenZip = FALSE; return TRUE; }
    }
    return FALSE;
}

static BOOL ExtractRarToDirW(const wchar_t* rarPath, const wchar_t* destDir, wchar_t* errMsg)
{
    wchar_t exePath[MAX_PATH];
    BOOL isSevenZip = FALSE;
    if (!FindRarExtractor(exePath, &isSevenZip)) {
        wsprintfW(errMsg,
            L"RAR extraction needs 7-Zip or WinRAR installed on this PC.\n"
            L"Install 7-Zip (free, 7-zip.org) or re-zip the mod as .zip.");
        return FALSE;
    }

    wchar_t cmdLine[2048];
    if (isSevenZip) {
        wsprintfW(cmdLine, L"\"%s\" x -y -o\"%s\" \"%s\"", exePath, destDir, rarPath);
    } else {
        wsprintfW(cmdLine, L"\"%s\" x -y \"%s\" \"%s\\\"", exePath, rarPath, destDir);
    }

    STARTUPINFOW si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));

    if (!CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        wsprintfW(errMsg, L"Couldn't start the RAR extractor.");
        return FALSE;
    }

    WaitForSingleObject(pi.hProcess, 60000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0) {
        wsprintfW(errMsg, L"The RAR extractor reported an error (code %lu).", exitCode);
        return FALSE;
    }
    return TRUE;
}

/* ===================== flatten single top-level folder + move into place ===================== */

static BOOL StagingHasSingleRootDirW(const wchar_t* stagingDir, wchar_t* outName)
{
    wchar_t search[MAX_PATH];
    wsprintfW(search, L"%s\\*", stagingDir);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    int count = 0;
    BOOL isDir = FALSE;
    wchar_t name[MAX_PATH] = {0};

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        count++;
        if (count > 1) break;
        wcscpy(name, fd.cFileName);
        isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (count == 1 && isDir) {
        wcscpy(outName, name);
        return TRUE;
    }
    return FALSE;
}

static BOOL FinalizeIntoPackFolder(const wchar_t* stagingDir, const wchar_t* destPackDir,
                                    const wchar_t* archiveBaseName, wchar_t* outFinalPath)
{
    wchar_t singleDirName[MAX_PATH];
    EnsureDirW(destPackDir);

    if (StagingHasSingleRootDirW(stagingDir, singleDirName)) {
        wchar_t srcPath[MAX_PATH], dstBase[MAX_PATH], dstFinal[MAX_PATH];
        wsprintfW(srcPath, L"%s\\%s", stagingDir, singleDirName);
        wsprintfW(dstBase, L"%s\\%s", destPackDir, singleDirName);
        MakeUniquePathW(dstBase, dstFinal);

        if (!MoveFileExW(srcPath, dstFinal, MOVEFILE_COPY_ALLOWED)) return FALSE;
        wcscpy(outFinalPath, dstFinal);
        return TRUE;
    }

    wchar_t wrapBase[MAX_PATH], wrapFinal[MAX_PATH];
    wsprintfW(wrapBase, L"%s\\%s", destPackDir, archiveBaseName);
    MakeUniquePathW(wrapBase, wrapFinal);
    if (!EnsureDirW(wrapFinal)) return FALSE;

    wchar_t search[MAX_PATH];
    wsprintfW(search, L"%s\\*", stagingDir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    BOOL ok = TRUE;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            wchar_t src[MAX_PATH], dst[MAX_PATH];
            wsprintfW(src, L"%s\\%s", stagingDir, fd.cFileName);
            wsprintfW(dst, L"%s\\%s", wrapFinal, fd.cFileName);
            if (!MoveFileExW(src, dst, MOVEFILE_COPY_ALLOWED)) ok = FALSE;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    wcscpy(outFinalPath, wrapFinal);
    return ok;
}

static void GetArchiveBaseName(const wchar_t* path, wchar_t* out)
{
    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* base = slash ? slash + 1 : path;
    wcscpy(out, base);
    wchar_t* dot = wcsrchr(out, L'.');
    if (dot) *dot = 0;
}

/* ===================== HTTP download (WinINet) ===================== */

typedef void (*HttpProgressFn)(DWORD downloaded, DWORD total, void* userData);

static BYTE* HttpGetToMemoryEx(const wchar_t* url, DWORD* outSize, wchar_t* errMsg,
                                HttpProgressFn onProgress, void* progressUserData)
{
    HINTERNET hInternet = InternetOpenW(L"ChoicerVoicerLauncher/1.0",
        INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInternet) {
        wsprintfW(errMsg, L"Couldn't initialize network access.");
        return NULL;
    }

    HINTERNET hUrl = InternetOpenUrlW(hInternet, url, NULL, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
    if (!hUrl) {
        wsprintfW(errMsg, L"Couldn't connect to:\n%s", url);
        InternetCloseHandle(hInternet);
        return NULL;
    }

    /* best-effort - if the server doesn't report Content-Length (e.g.
       chunked transfer), this just stays 0 and callers fall back to an
       indeterminate/byte-count progress display instead of a percentage */
    DWORD totalSize = 0;
    DWORD totalSizeLen = sizeof(totalSize);
    if (!HttpQueryInfoW(hUrl, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER,
            &totalSize, &totalSizeLen, NULL)) {
        totalSize = 0;
    }

    DWORD capacity = 1 << 20;
    DWORD size = 0;
    BYTE* buf = (BYTE*)malloc(capacity);
    if (!buf) {
        wsprintfW(errMsg, L"Out of memory.");
        InternetCloseHandle(hUrl); InternetCloseHandle(hInternet);
        return NULL;
    }

    for (;;) {
        DWORD avail = 0;
        if (!InternetQueryDataAvailable(hUrl, &avail, 0, 0)) break;
        if (avail == 0) break;
        if (size + avail > capacity) {
            DWORD newCap = capacity * 2;
            while (newCap < size + avail) newCap *= 2;
            BYTE* newBuf = (BYTE*)realloc(buf, newCap);
            if (!newBuf) {
                free(buf);
                wsprintfW(errMsg, L"Out of memory.");
                InternetCloseHandle(hUrl); InternetCloseHandle(hInternet);
                return NULL;
            }
            buf = newBuf; capacity = newCap;
        }
        DWORD readBytes = 0;
        if (!InternetReadFile(hUrl, buf + size, avail, &readBytes) || readBytes == 0) break;
        size += readBytes;
        if (onProgress) onProgress(size, totalSize, progressUserData);
        if (size > 500u * 1024u * 1024u) break; /* 500MB safety cap */
    }

    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInternet);

    if (size == 0) {
        free(buf);
        wsprintfW(errMsg, L"Downloaded 0 bytes from:\n%s", url);
        return NULL;
    }

    *outSize = size;
    return buf;
}

static BYTE* HttpGetToMemory(const wchar_t* url, DWORD* outSize, wchar_t* errMsg)
{
    return HttpGetToMemoryEx(url, outSize, errMsg, NULL, NULL);
}

/* ===================== GameBanana link resolution ===================== */

typedef struct { const wchar_t* segment; const wchar_t* itemType; } GBTypeMap;
static const GBTypeMap GB_TYPE_MAP[] = {
    {L"mods", L"Mod"}, {L"wips", L"Wip"}, {L"sounds", L"Sound"},
    {L"tools", L"Tool"}, {L"skins", L"Skin"}, {L"maps", L"Map"},
    {L"textures", L"Texture"}, {L"scripts", L"Script"}, {L"guis", L"Gui"},
    {L"effects", L"Effect"}, {L"models", L"Model"}, {L"gamefiles", L"Gamefile"},
};
#define GB_TYPE_MAP_COUNT (sizeof(GB_TYPE_MAP)/sizeof(GB_TYPE_MAP[0]))

/* Recognizes GameBanana mod-PAGE links like gamebanana.com/mods/123456 and
   splits out the API itemtype ("Mod") and numeric id. Returns FALSE for
   anything else (including gamebanana.com/dl/... which is already direct). */
static BOOL IsGameBananaModPageUrl(const wchar_t* url, wchar_t* outItemType, wchar_t* outItemId)
{
    const wchar_t* host = wcsstr(url, L"gamebanana.com/");
    if (!host) return FALSE;
    const wchar_t* p = host + wcslen(L"gamebanana.com/");

    wchar_t segment[32];
    int si = 0;
    while (*p && *p != L'/' && si < 31) { segment[si++] = *p; p++; }
    segment[si] = 0;
    if (*p != L'/') return FALSE;
    p++;

    const wchar_t* itemType = NULL;
    for (size_t i = 0; i < GB_TYPE_MAP_COUNT; i++) {
        if (_wcsicmp(segment, GB_TYPE_MAP[i].segment) == 0) { itemType = GB_TYPE_MAP[i].itemType; break; }
    }
    if (!itemType) return FALSE;

    wchar_t idbuf[32];
    int di = 0;
    while (*p && *p >= L'0' && *p <= L'9' && di < 31) { idbuf[di++] = *p; p++; }
    idbuf[di] = 0;
    if (di == 0) return FALSE;

    wcscpy(outItemType, itemType);
    wcscpy(outItemId, idbuf);
    return TRUE;
}

/* Scans raw API JSON text for the first quoted all-numeric key immediately
   followed by a colon - matches the file-id key in a Files().aFiles()
   response, e.g. finds "1234567" in [{"1234567":"SomeMod.zip"}], without
   needing a full JSON parser. */
static BOOL ExtractFirstJsonNumericKey(const char* json, wchar_t* outId)
{
    const char* p = json;
    while (*p) {
        if (*p == '"') {
            const char* start = p + 1;
            const char* q = start;
            BOOL allDigits = (*q != '\0');
            while (*q && *q != '"') {
                if (*q < '0' || *q > '9') allDigits = FALSE;
                q++;
            }
            if (*q == '"' && allDigits && q > start) {
                const char* after = q + 1;
                while (*after == ' ' || *after == '\t') after++;
                if (*after == ':') {
                    int len = (int)(q - start);
                    if (len > 0 && len < 64) {
                        MultiByteToWideChar(CP_UTF8, 0, start, len, outId, 64);
                        outId[len] = 0;
                        return TRUE;
                    }
                }
            }
            p = q + (*q ? 1 : 0);
        } else {
            p++;
        }
    }
    return FALSE;
}

static int SniffArchiveType(const BYTE* data, DWORD size)
{
    if (size >= 4 && data[0]=='P' && data[1]=='K' && (data[2]==3||data[2]==5||data[2]==7)) return 1; /* zip */
    if (size >= 6 && data[0]=='R' && data[1]=='a' && data[2]=='r' && data[3]=='!' &&
        data[4]==0x1A && data[5]==0x07) return 2; /* rar */
    return 0;
}

/* ===================== install pipeline: download/extract/install with
   a progress window, used by both the drop zone and the mod browser's
   Install button ===================== */

/* forward declaration - defined further down near WM_CREATE/theme
   handling, but needed here by the progress/destination-picker popups */
static void ApplyDarkTitlebar(HWND hwnd, BOOL dark);

#define WM_APP_INSTALL_PROGRESS (WM_APP + 2)
#define WM_APP_INSTALL_DONE     (WM_APP + 3)

#define ID_DESTPICKER_COMBO   6001
#define ID_DESTPICKER_INSTALL 6002
#define ID_DESTPICKER_CANCEL  6003

typedef struct {
    wchar_t status[128];
    int percent; /* -1 = indeterminate */
} InstallProgressMsg;

typedef struct {
    BOOL success;
    wchar_t resultMsg[600];
} InstallDoneMsg;

typedef struct {
    HWND mainWnd;
    HWND progressWnd;
    BOOL fromUrl;
    wchar_t url[2048];
    wchar_t localFile[MAX_PATH];
    wchar_t destPackFolder[64];
} InstallJobParam;

static HWND g_hInstallProgressWnd = NULL;
static HWND g_hInstallProgressBar = NULL;
static HWND g_hInstallProgressLabel = NULL;
static HWND g_hInstallProgressOwner = NULL;

static void GetCurrentPackFolder(wchar_t* out, int outCap)
{
    int sel = (int)SendMessageW(g_hPackCombo, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= NUM_PACK_FOLDERS) sel = 0;
    wcsncpy(out, PACK_FOLDERS[sel], outCap - 1);
    out[outCap - 1] = 0;
}

static void PostInstallProgress(HWND progressWnd, const wchar_t* status, int percent)
{
    if (!IsWindow(progressWnd)) return;
    InstallProgressMsg* p = (InstallProgressMsg*)malloc(sizeof(InstallProgressMsg));
    if (!p) return;
    wcsncpy(p->status, status, 127); p->status[127] = 0;
    p->percent = percent;
    if (!PostMessageW(progressWnd, WM_APP_INSTALL_PROGRESS, 0, (LPARAM)p)) free(p);
}

typedef struct { HWND progressWnd; DWORD lastPercentPosted; } DlProgressCtx;

static void DownloadProgressCb(DWORD downloaded, DWORD total, void* userData)
{
    DlProgressCtx* ctx = (DlProgressCtx*)userData;
    wchar_t status[128];
    if (total > 0) {
        DWORD pct = (DWORD)(((double)downloaded / (double)total) * 100.0);
        if (pct > 100) pct = 100;
        if (pct == ctx->lastPercentPosted) return;
        ctx->lastPercentPosted = pct;
        wsprintfW(status, L"Downloading\u2026 %lu%%", pct);
        PostInstallProgress(ctx->progressWnd, status, (int)pct);
    } else {
        wsprintfW(status, L"Downloading\u2026 %lu KB", downloaded / 1024);
        PostInstallProgress(ctx->progressWnd, status, -1);
    }
}

static DWORD WINAPI InstallJobThreadProc(LPVOID lpParam)
{
    InstallJobParam* job = (InstallJobParam*)lpParam;
    HWND progressWnd = job->progressWnd;

    InstallDoneMsg* done = (InstallDoneMsg*)calloc(1, sizeof(InstallDoneMsg));
    if (!done) { free(job); return 0; }

    wchar_t filePath[MAX_PATH] = L"";
    BOOL ownsTempFile = FALSE;

    if (job->fromUrl) {
        wchar_t downloadUrl[2048];
        wchar_t itemType[32], itemId[32];

        if (IsGameBananaModPageUrl(job->url, itemType, itemId)) {
            PostInstallProgress(progressWnd, L"Looking up download link\u2026", -1);
            wchar_t apiUrl[1024];
            wsprintfW(apiUrl,
                L"https://api.gamebanana.com/Core/Item/Data?itemtype=%s&itemid=%s&fields=Files().aFiles()&format=json_min",
                itemType, itemId);
            wchar_t errMsg[512] = L"";
            DWORD apiSize = 0;
            BYTE* apiData = HttpGetToMemoryEx(apiUrl, &apiSize, errMsg, NULL, NULL);
            if (!apiData) {
                done->success = FALSE;
                wcsncpy(done->resultMsg, errMsg[0] ? errMsg : L"Couldn't reach GameBanana's API.", 599);
                goto finish;
            }
            char* jsonText = (char*)malloc((size_t)apiSize + 1);
            if (!jsonText) {
                free(apiData);
                done->success = FALSE;
                wcscpy(done->resultMsg, L"Out of memory.");
                goto finish;
            }
            memcpy(jsonText, apiData, apiSize); jsonText[apiSize] = 0;
            free(apiData);
            wchar_t fileId[64];
            BOOL found = ExtractFirstJsonNumericKey(jsonText, fileId);
            free(jsonText);
            if (!found) {
                done->success = FALSE;
                wcscpy(done->resultMsg, L"Couldn't find a downloadable file on that GameBanana page.");
                goto finish;
            }
            wsprintfW(downloadUrl, L"https://gamebanana.com/dl/%s", fileId);
        } else {
            wcsncpy(downloadUrl, job->url, 2047); downloadUrl[2047] = 0;
        }

        DlProgressCtx ctx = { progressWnd, (DWORD)-1 };
        wchar_t errMsg2[512] = L"";
        DWORD fileSize = 0;
        BYTE* fileData = HttpGetToMemoryEx(downloadUrl, &fileSize, errMsg2, DownloadProgressCb, &ctx);
        if (!fileData) {
            done->success = FALSE;
            wcsncpy(done->resultMsg, errMsg2[0] ? errMsg2 : L"Download failed.", 599);
            goto finish;
        }

        int archiveType = SniffArchiveType(fileData, fileSize);
        if (archiveType == 0) {
            free(fileData);
            done->success = FALSE;
            wcscpy(done->resultMsg, L"The downloaded file doesn't look like a .zip or .rar archive.");
            goto finish;
        }

        wchar_t tempDir[MAX_PATH];
        GetTempPathW(MAX_PATH, tempDir);
        wsprintfW(filePath, L"%scvlauncher_dl_%lu.%s", tempDir, GetTickCount(),
            archiveType == 1 ? L"zip" : L"rar");
        HANDLE hOut = CreateFileW(filePath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hOut == INVALID_HANDLE_VALUE) {
            free(fileData);
            done->success = FALSE;
            wcscpy(done->resultMsg, L"Couldn't save the downloaded file.");
            goto finish;
        }
        DWORD written = 0;
        WriteFile(hOut, fileData, fileSize, &written, NULL);
        CloseHandle(hOut);
        free(fileData);
        ownsTempFile = TRUE;
    } else {
        wcsncpy(filePath, job->localFile, MAX_PATH - 1);
        filePath[MAX_PATH - 1] = 0;
    }

    {
        size_t len = wcslen(filePath);
        BOOL isZip = (len > 4 && _wcsicmp(filePath + len - 4, L".zip") == 0);
        BOOL isRar = (len > 4 && _wcsicmp(filePath + len - 4, L".rar") == 0);
        if (!isZip && !isRar) {
            done->success = FALSE;
            wcscpy(done->resultMsg, L"Only .zip and .rar files are supported.");
            goto finish;
        }

        PostInstallProgress(progressWnd, L"Preparing\u2026", -1);
        wchar_t gameDir[MAX_PATH];
        if (!GetAppDataGameDir(gameDir) || !PathIsDirW(gameDir)) {
            done->success = FALSE;
            wsprintfW(done->resultMsg,
                L"The game's data folder doesn't exist yet:\n%s\n\nRun the game at least once first.", gameDir);
            goto finish;
        }

        wchar_t destPackDir[MAX_PATH];
        wsprintfW(destPackDir, L"%s\\%s", gameDir, job->destPackFolder);
        EnsureDirW(destPackDir);

        wchar_t tempDir2[MAX_PATH];
        GetTempPathW(MAX_PATH, tempDir2);
        wchar_t stagingDir[MAX_PATH];
        static LONG s_stagingCounter = 0;
        wsprintfW(stagingDir, L"%scvlauncher_staging_%lu_%ld",
            tempDir2, GetTickCount(), InterlockedIncrement(&s_stagingCounter));
        EnsureDirW(stagingDir);

        PostInstallProgress(progressWnd, L"Extracting\u2026", -1);
        wchar_t errMsg3[512] = L"";
        BOOL extractOk = isZip
            ? ExtractZipToDirW(filePath, stagingDir, errMsg3)
            : ExtractRarToDirW(filePath, stagingDir, errMsg3);

        if (!extractOk) {
            done->success = FALSE;
            wcsncpy(done->resultMsg, errMsg3[0] ? errMsg3 : L"Extraction failed.", 599);
            DeleteDirectoryRecursiveW(stagingDir);
            goto finish;
        }

        wchar_t archiveBase[MAX_PATH];
        GetArchiveBaseName(filePath, archiveBase);

        wchar_t finalPath[MAX_PATH];
        PostInstallProgress(progressWnd, L"Installing\u2026", -1);
        BOOL finalizeOk = FinalizeIntoPackFolder(stagingDir, destPackDir, archiveBase, finalPath);
        DeleteDirectoryRecursiveW(stagingDir);

        if (!finalizeOk) {
            done->success = FALSE;
            wcscpy(done->resultMsg, L"Extracted, but couldn't move the mod into place.");
            goto finish;
        }

        done->success = TRUE;
        wsprintfW(done->resultMsg, L"Mod installed to:\n%s", finalPath);
    }

finish:
    if (ownsTempFile && filePath[0]) DeleteFileW(filePath);
    free(job);
    if (IsWindow(progressWnd)) {
        PostMessageW(progressWnd, WM_APP_INSTALL_DONE, 0, (LPARAM)done);
    } else {
        free(done);
    }
    return 0;
}

static LRESULT CALLBACK InstallProgressWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
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
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT);
        return (LRESULT)(g_hBrushBg ? g_hBrushBg : GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_APP_INSTALL_PROGRESS: {
        InstallProgressMsg* p = (InstallProgressMsg*)lParam;
        if (p) {
            if (g_hInstallProgressLabel) SetWindowTextW(g_hInstallProgressLabel, p->status);
            if (g_hInstallProgressBar) {
                if (p->percent < 0) {
                    SendMessageW(g_hInstallProgressBar, PBM_SETMARQUEE, TRUE, 50);
                } else {
                    SendMessageW(g_hInstallProgressBar, PBM_SETMARQUEE, FALSE, 0);
                    SendMessageW(g_hInstallProgressBar, PBM_SETPOS, (WPARAM)p->percent, 0);
                }
            }
            free(p);
        }
        return 0;
    }
    case WM_APP_INSTALL_DONE: {
        InstallDoneMsg* d = (InstallDoneMsg*)lParam;
        HWND owner = g_hInstallProgressOwner;
        g_hInstallProgressWnd = NULL;
        g_hInstallProgressBar = NULL;
        g_hInstallProgressLabel = NULL;
        DestroyWindow(hwnd);
        if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
        if (d) {
            MessageBoxW(owner, d->resultMsg, L"The Choicer Voicer - Launcher",
                d->success ? (MB_ICONINFORMATION | MB_OK) : (MB_ICONERROR | MB_OK));
            free(d);
        }
        return 0;
    }
    case WM_CLOSE:
        /* no cancel support yet - ignore attempts to close mid-install */
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static HWND CreateInstallProgressWnd(HWND hOwner, HINSTANCE hInst)
{
    static BOOL classRegistered = FALSE;
    const wchar_t CLASS_NAME[] = L"CVLauncherInstallProgressWnd";
    if (!classRegistered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc   = InstallProgressWndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = CLASS_NAME;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
        RegisterClassW(&wc);
        classRegistered = TRUE;
    }

    const int w = 360, h = 130;
    DWORD style = WS_POPUP | WS_CAPTION;
    RECT rc = {0, 0, w, h};
    AdjustWindowRect(&rc, style, FALSE);
    int winW = rc.right - rc.left, winH = rc.bottom - rc.top;

    RECT prc; GetWindowRect(hOwner, &prc);
    int posX = prc.left + ((prc.right - prc.left) - winW) / 2;
    int posY = prc.top + ((prc.bottom - prc.top) - winH) / 2;

    g_hInstallProgressOwner = hOwner;

    HWND hPop = CreateWindowExW(WS_EX_DLGMODALFRAME, CLASS_NAME, L"Installing Mod\u2026",
        style, posX, posY, winW, winH, hOwner, NULL, hInst, NULL);
    if (!hPop) return NULL;
    g_hInstallProgressWnd = hPop;
    ApplyDarkTitlebar(hPop, g_darkMode);

    g_hInstallProgressLabel = CreateWindowW(L"STATIC", L"Starting\u2026",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 20, w - 40, 20, hPop, NULL, hInst, NULL);
    SendMessageW(g_hInstallProgressLabel, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);

    g_hInstallProgressBar = CreateWindowW(PROGRESS_CLASSW, NULL,
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH | PBS_MARQUEE, 20, 50, w - 40, 22, hPop, NULL, hInst, NULL);
    SendMessageW(g_hInstallProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(g_hInstallProgressBar, PBM_SETMARQUEE, TRUE, 50);

    EnableWindow(hOwner, FALSE);
    ShowWindow(hPop, SW_SHOW);
    UpdateWindow(hPop);
    return hPop;
}

static void StartInstallJob(HWND mainWnd, BOOL fromUrl, const wchar_t* urlOrPath, const wchar_t* destPackFolder)
{
    HWND progressWnd = CreateInstallProgressWnd(mainWnd, g_hInstance);
    if (!progressWnd) {
        MessageBoxW(mainWnd, L"Couldn't open the progress window.",
            L"The Choicer Voicer - Launcher", MB_ICONERROR | MB_OK);
        return;
    }

    InstallJobParam* job = (InstallJobParam*)calloc(1, sizeof(InstallJobParam));
    if (!job) { DestroyWindow(progressWnd); return; }
    job->mainWnd = mainWnd;
    job->progressWnd = progressWnd;
    job->fromUrl = fromUrl;
    if (fromUrl) { wcsncpy(job->url, urlOrPath, 2047); job->url[2047] = 0; }
    else { wcsncpy(job->localFile, urlOrPath, MAX_PATH - 1); job->localFile[MAX_PATH - 1] = 0; }
    wcsncpy(job->destPackFolder, destPackFolder, 63); job->destPackFolder[63] = 0;

    HANDLE hThread = CreateThread(NULL, 0, InstallJobThreadProc, job, 0, NULL);
    if (hThread) CloseHandle(hThread);
    else { free(job); DestroyWindow(progressWnd); }
}

/* ---- destination picker: which pack_### folder to install a mod-browser
   pick into, shown before StartInstallJob runs ---- */

static wchar_t g_destPickerUrl[2048] = L"";
static HWND g_hDestPickerCombo = NULL;
static HWND g_hDestPickerOwner = NULL;

static void CloseDestPicker(HWND hwnd, BOOL doInstall)
{
    wchar_t chosen[64] = L"";
    if (doInstall && g_hDestPickerCombo) {
        int sel = (int)SendMessageW(g_hDestPickerCombo, CB_GETCURSEL, 0, 0);
        if (sel < 0 || sel >= NUM_PACK_FOLDERS) sel = 0;
        wcsncpy(chosen, PACK_FOLDERS[sel], 63);
    }
    HWND owner = g_hDestPickerOwner;
    DestroyWindow(hwnd);
    if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
    if (doInstall && chosen[0]) {
        wcsncpy(g_lastPackFolder, chosen, 63); g_lastPackFolder[63] = 0;
        SaveSettings();
        if (g_hPackCombo) {
            LRESULT idx = SendMessageW(g_hPackCombo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)chosen);
            if (idx != CB_ERR) SendMessageW(g_hPackCombo, CB_SETCURSEL, (WPARAM)idx, 0);
        }
        StartInstallJob(owner, TRUE, g_destPickerUrl, chosen);
    }
}

static LRESULT CALLBACK DestPickerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
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
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT);
        return (LRESULT)(g_hBrushBg ? g_hBrushBg : GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_DESTPICKER_INSTALL: CloseDestPicker(hwnd, TRUE); return 0;
        case ID_DESTPICKER_CANCEL:  CloseDestPicker(hwnd, FALSE); return 0;
        }
        break;
    case WM_CLOSE:
        CloseDestPicker(hwnd, FALSE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowDestPicker(HWND hOwner, HINSTANCE hInst, const wchar_t* profileUrl)
{
    wcsncpy(g_destPickerUrl, profileUrl, 2047); g_destPickerUrl[2047] = 0;

    static BOOL classRegistered = FALSE;
    const wchar_t CLASS_NAME[] = L"CVLauncherDestPickerWnd";
    if (!classRegistered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc   = DestPickerWndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = CLASS_NAME;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
        RegisterClassW(&wc);
        classRegistered = TRUE;
    }

    const int w = 340, h = 170;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT rc = {0, 0, w, h};
    AdjustWindowRect(&rc, style, FALSE);
    int winW = rc.right - rc.left, winH = rc.bottom - rc.top;

    RECT prc; GetWindowRect(hOwner, &prc);
    int posX = prc.left + ((prc.right - prc.left) - winW) / 2;
    int posY = prc.top + ((prc.bottom - prc.top) - winH) / 2;

    g_hDestPickerOwner = hOwner;

    HWND hPop = CreateWindowExW(WS_EX_DLGMODALFRAME, CLASS_NAME, L"Choose Destination",
        style, posX, posY, winW, winH, hOwner, NULL, hInst, NULL);
    if (!hPop) return;
    ApplyDarkTitlebar(hPop, g_darkMode);

    HWND hMsg = CreateWindowW(L"STATIC", L"Install this mod to which pack folder?",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 20, w - 40, 20, hPop, NULL, hInst, NULL);
    SendMessageW(hMsg, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);

    g_hDestPickerCombo = CreateWindowW(L"COMBOBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        20, 46, w - 40, 200, hPop, (HMENU)(INT_PTR)ID_DESTPICKER_COMBO, hInst, NULL);
    SendMessageW(g_hDestPickerCombo, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);
    for (int i = 0; i < NUM_PACK_FOLDERS; i++) {
        SendMessageW(g_hDestPickerCombo, CB_ADDSTRING, 0, (LPARAM)PACK_FOLDERS[i]);
    }
    {
        LRESULT idx = g_lastPackFolder[0]
            ? SendMessageW(g_hDestPickerCombo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)g_lastPackFolder)
            : CB_ERR;
        SendMessageW(g_hDestPickerCombo, CB_SETCURSEL, (idx == CB_ERR) ? 0 : (WPARAM)idx, 0);
    }
    SetWindowTheme(g_hDestPickerCombo, g_darkMode ? L"DarkMode_Explorer" : L"Explorer", NULL);

    HWND hInstallBtn = CreateWindowW(L"BUTTON", L"Install",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
        20, h - 60, 140, 32, hPop, (HMENU)(INT_PTR)ID_DESTPICKER_INSTALL, hInst, NULL);
    SendMessageW(hInstallBtn, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);

    HWND hCancelBtn = CreateWindowW(L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        w - 20 - 100, h - 60, 100, 32, hPop, (HMENU)(INT_PTR)ID_DESTPICKER_CANCEL, hInst, NULL);
    SendMessageW(hCancelBtn, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);

    EnableWindow(hOwner, FALSE);
    ShowWindow(hPop, SW_SHOW);
    UpdateWindow(hPop);
    SetForegroundWindow(hPop);
    SetFocus(g_hDestPickerCombo);
}

/* ===================== GameBanana mod browser (Browse Mods tab) ===================== */

/* ---- tiny bounded JSON scanner - not a general parser, just enough to
   pull the handful of fields we need out of GameBanana's Mod/Index
   response without pulling in a JSON library ---- */

static const char* FindSubstrBounded(const char* start, const char* end, const char* needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0 || start >= end) return NULL;
    for (const char* p = start; p + (ptrdiff_t)nlen <= end; p++) {
        size_t k = 0;
        while (k < nlen && p[k] == needle[k]) k++;
        if (k == nlen) return p;
    }
    return NULL;
}

/* p is positioned anywhere before the object's opening '{' (skipping over
   things like a preceding ':' or '[' is fine); matches braces while
   ignoring anything inside quoted strings, bounded by `end`. */
static BOOL SpanJsonObjectAfterColon(const char* p, const char* end, const char** outStart, const char** outEnd)
{
    while (p < end && *p != '{') {
        if (*p == '}' || *p == ']') return FALSE;
        p++;
    }
    if (p >= end || *p != '{') return FALSE;
    const char* start = p;
    int depth = 0;
    BOOL inStr = FALSE;
    for (; p < end; p++) {
        char c = *p;
        if (inStr) {
            if (c == '\\') { p++; continue; }
            if (c == '"') inStr = FALSE;
            continue;
        }
        if (c == '"') { inStr = TRUE; continue; }
        if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) { *outStart = start; *outEnd = p + 1; return TRUE; }
        }
    }
    return FALSE;
}

/* advances *pp past the next object found before `end`; returns FALSE once
   the enclosing array's ']' is reached (or on malformed input) */
static BOOL NextJsonArrayObject(const char** pp, const char* end, const char** outStart, const char** outEnd)
{
    const char* p = *pp;
    while (p < end && *p != '{' && *p != ']') p++;
    if (p >= end || *p == ']') { *pp = p; return FALSE; }
    if (!SpanJsonObjectAfterColon(p, end, outStart, outEnd)) { *pp = end; return FALSE; }
    *pp = *outEnd;
    return TRUE;
}

static BOOL ExtractJsonIntAt(const char* p, const char* end, long* outVal)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    BOOL neg = FALSE;
    if (p < end && *p == '-') { neg = TRUE; p++; }
    if (p >= end || *p < '0' || *p > '9') return FALSE;
    long v = 0;
    while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    *outVal = neg ? -v : v;
    return TRUE;
}

/* p points at the opening quote of a JSON string value. Resolves the
   handful of escapes GameBanana's API actually uses (including \uXXXX)
   into literal UTF-8 bytes, then reuses the settings-file UTF-8 decoder
   to get a proper wide string - handles both escaped and raw non-ASCII
   text correctly instead of a naive byte-for-byte copy. */
static void ExtractJsonStringAt(const char* p, const char* end, wchar_t* out, int outCap)
{
    out[0] = 0;
    /* GameBanana's API returns pretty-printed JSON with a space after each
       colon (e.g. `"_sName": "Some Mod"`), not compact JSON - this was
       being missed here (ExtractJsonIntAt already skips leading
       whitespace, this didn't), so the strict "must be a quote right
       here" check below was silently bailing out on every string field:
       names, author names, and thumbnail URLs were all coming back
       empty because of this one missing skip. */
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (p >= end || *p != '"') return;
    p++;

    char utf8buf[1024];
    int oi = 0;
    while (p < end && *p != '"' && oi < (int)sizeof(utf8buf) - 4) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\' && p + 1 < end) {
            char e = p[1];
            switch (e) {
            case '"':  utf8buf[oi++] = '"';  p += 2; break;
            case '\\': utf8buf[oi++] = '\\'; p += 2; break;
            case '/':  utf8buf[oi++] = '/';  p += 2; break;
            case 'n':  utf8buf[oi++] = '\n'; p += 2; break;
            case 't':  utf8buf[oi++] = '\t'; p += 2; break;
            case 'u':
                if (p + 5 < end) {
                    unsigned int code = 0;
                    for (int k = 0; k < 4; k++) {
                        char hc = p[2 + k];
                        int v = (hc >= '0' && hc <= '9') ? hc - '0' :
                                (hc >= 'a' && hc <= 'f') ? hc - 'a' + 10 :
                                (hc >= 'A' && hc <= 'F') ? hc - 'A' + 10 : 0;
                        code = (code << 4) | (unsigned)v;
                    }
                    if (code < 0x80) {
                        utf8buf[oi++] = (char)code;
                    } else if (code < 0x800) {
                        utf8buf[oi++] = (char)(0xC0 | (code >> 6));
                        utf8buf[oi++] = (char)(0x80 | (code & 0x3F));
                    } else {
                        utf8buf[oi++] = (char)(0xE0 | (code >> 12));
                        utf8buf[oi++] = (char)(0x80 | ((code >> 6) & 0x3F));
                        utf8buf[oi++] = (char)(0x80 | (code & 0x3F));
                    }
                    p += 6;
                } else { p++; }
                break;
            default: utf8buf[oi++] = e; p += 2; break;
            }
        } else {
            utf8buf[oi++] = (char)c;
            p++;
        }
    }
    DecodeUtf8ToWideBuf((const BYTE*)utf8buf, (DWORD)oi, out, outCap);
}

/* ---- decoding JPG/PNG thumbnails via WIC (Windows Imaging Component) ----
   this app has no bundled image codec (miniz is a zip library only), so
   thumbnails are decoded through the OS's own WIC COM service instead of
   pulling in a third-party decoder. */
static HBITMAP DecodeImageToBitmap(IWICImagingFactory* pFactory, const BYTE* data, DWORD size)
{
    if (!pFactory || !data || size == 0) return NULL;

    HBITMAP hBmp = NULL;
    IWICStream* pStream = NULL;
    IWICBitmapDecoder* pDecoder = NULL;
    IWICBitmapFrameDecode* pFrame = NULL;
    IWICFormatConverter* pConverter = NULL;

    HRESULT hr = pFactory->lpVtbl->CreateStream(pFactory, &pStream);
    if (SUCCEEDED(hr)) hr = pStream->lpVtbl->InitializeFromMemory(pStream, (BYTE*)data, size);
    if (SUCCEEDED(hr)) {
        hr = pFactory->lpVtbl->CreateDecoderFromStream(pFactory, (IStream*)pStream, NULL,
            WICDecodeMetadataCacheOnDemand, &pDecoder);
    }
    if (SUCCEEDED(hr)) hr = pDecoder->lpVtbl->GetFrame(pDecoder, 0, &pFrame);
    if (SUCCEEDED(hr)) hr = pFactory->lpVtbl->CreateFormatConverter(pFactory, &pConverter);
    if (SUCCEEDED(hr)) {
        hr = pConverter->lpVtbl->Initialize(pConverter, (IWICBitmapSource*)pFrame,
            &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    }

    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) hr = pConverter->lpVtbl->GetSize(pConverter, &w, &h);

    if (SUCCEEDED(hr) && w > 0 && h > 0) {
        BITMAPINFO bmi; memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = (LONG)w;
        bmi.bmiHeader.biHeight = -(LONG)h; /* top-down, matches WIC's row order */
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = NULL;
        hBmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (hBmp && bits) {
            UINT stride = w * 4;
            hr = pConverter->lpVtbl->CopyPixels(pConverter, NULL, stride, stride * h, (BYTE*)bits);
            if (FAILED(hr)) { DeleteObject(hBmp); hBmp = NULL; }
        }
    }

    if (pConverter) pConverter->lpVtbl->Release(pConverter);
    if (pFrame) pFrame->lpVtbl->Release(pFrame);
    if (pDecoder) pDecoder->lpVtbl->Release(pDecoder);
    if (pStream) pStream->lpVtbl->Release(pStream);
    return hBmp;
}

/* ---- mod list state (populated on a background thread, only ever read/
   written on the UI thread once handed over via WM_APP_MODS_LOADED) ---- */

#define MODS_PER_PAGE 20
#define MAX_MOD_ENTRIES 200
#define WM_APP_MODS_LOADED    (WM_APP + 1)

typedef struct {
    long modId;
    wchar_t name[128];
    wchar_t author[64];
    wchar_t category[64];
    wchar_t profileUrl[160];
    long likeCount;
    long viewCount;
    long dateAdded;
    HBITMAP thumb;
} ModEntry;

typedef enum { MODLOAD_IDLE, MODLOAD_LOADING, MODLOAD_LOADED, MODLOAD_ERROR } ModLoadState;

static ModEntry g_modEntries[MAX_MOD_ENTRIES];
static int g_modCount = 0;
static int g_modNextPage = 1;
static ModLoadState g_modLoadState = MODLOAD_IDLE;
static wchar_t g_modErrorMsg[256] = L"";
static BOOL g_modHasMore = TRUE;
static int g_modScrollY = 0;
static HWND g_hModListWnd = NULL;

/* real page tabs (Prev/1/2/3.../Next) instead of an easy-to-miss "Load
   more mods..." text link - each "page" here is MODLIST_PAGE_SIZE mods
   out of the current search/sort results, not a raw GameBanana page (see
   MOD_FETCH_RAW_PERPAGE/MOD_FETCH_MAX_RAW_PAGES below for that). Clicking
   Next past the last already-loaded page triggers fetching another batch
   and automatically advances onto it once it arrives. */
#define MODLIST_PAGE_SIZE 20
static int g_modCurrentPage = 0;      /* 0-indexed, into the filtered/sorted results */
static BOOL g_modPendingAdvance = FALSE;

/* search + sort. GameBanana's Mod/Index _sSort rejected the whole
   request outright for values it doesn't recognize (confirmed via the
   literal "_sErrorCode":"UNKNOWN_SORT" error it returned, per BUILD.txt)
   - but real captured browser requests confirmed apiv13 + Generic_Category
   + _sSort=Generic_MostViewed AND _sSort=Generic_MostLiked as both
   genuinely working, so Likes and Views now trigger a real server-side
   fetch in that order (see ModSortServerValue) rather than only
   reordering whatever happened to already be cached. Newest and Name
   still have no confirmed server equivalent (Newest is just the default
   fetch order with _sSort omitted, and Name isn't a GameBanana concept
   at all), so those two stay a pure client-side resort of g_modEntries.
   Search is a separate endpoint (Util/Search/Results) with its own real
   captured request/response, wired in separately - see StartModFetch. */
typedef enum { MODSORT_NEWEST = 0, MODSORT_NAME, MODSORT_LIKES, MODSORT_VIEWS } ModSortMode;
static ModSortMode g_modSortMode = MODSORT_NEWEST;
static wchar_t g_modSearchQuery[128] = L"";
static int g_modFilteredIndices[MAX_MOD_ENTRIES];
static int g_modFilteredCount = 0;
static HWND g_hModSearchEdit = NULL;
static HWND g_hModSortCombo = NULL;

#define ID_MODLIST_SEARCH 5010
#define ID_MODLIST_SORT   5011
#define MODLIST_SEARCH_DEBOUNCE_TIMER_ID 2
#define MODLIST_SEARCH_DEBOUNCE_MS 450

/* result handed from the worker thread to the UI thread */
typedef struct {
    ModEntry* entries;
    int count;
    BOOL hasMore;
    BOOL ok;
    wchar_t errMsg[256];
} ModFetchResult;

typedef struct { HWND hwnd; int page; ModSortMode sortMode; wchar_t searchQuery[128]; } ModFetchThreadParam;

/* Both confirmed directly against real captured browser requests (apiv13,
   Mod/Index, _aFilters[Generic_Category]=44064) - one with
   _sSort=Generic_MostViewed, one with _sSort=Generic_MostLiked, each
   returning correctly-ordered, correctly-scoped results. MODSORT_NEWEST
   and MODSORT_NAME return NULL (omit _sSort entirely), since the
   confirmed default behavior with no _sSort at all is newest-first, and
   NAME has no known server-side equivalent - it stays a pure
   client-side resort of whatever's loaded. */
static const wchar_t* ModSortServerValue(ModSortMode mode)
{
    switch (mode) {
    case MODSORT_LIKES: return L"Generic_MostLiked";
    case MODSORT_VIEWS: return L"Generic_MostViewed";
    case MODSORT_NEWEST:
    case MODSORT_NAME:
    default: return NULL;
    }
}

static void FreeModFetchResult(ModFetchResult* r)
{
    if (!r) return;
    if (r->entries) {
        for (int i = 0; i < r->count; i++) {
            if (r->entries[i].thumb) DeleteObject(r->entries[i].thumb);
        }
        free(r->entries);
    }
    free(r);
}

/* Two real captured requests (both against _aFilters[Generic_Category]
   with different _sSort values) came back with every single record - 30
   for 30 across both - correctly belonging to our game. So unlike
   _aFilters[Generic_Game], which never reliably scoped results in
   testing, filtering by GAMEBANANA_CATEGORY_ID actually works. The
   client-side _aGame._idRow check right below is kept anyway as a cheap
   safety net (costs nothing, guards against the rare edge case), but
   this should no longer need to hunt through many pages to fill a
   batch - a modest budget is kept mainly as protection against an
   unexpectedly sparse result set, not because filtering is expected to
   fail outright the way it used to be assumed to. */
#define MOD_FETCH_RAW_PERPAGE 50
#define MOD_FETCH_MAX_RAW_PAGES 6

/* Minimal query-string encoder for search terms: letters/digits/-_.~ pass
   through, spaces become +, other ASCII punctuation is percent-encoded.
   Non-ASCII characters are dropped rather than UTF-8-percent-encoded -
   search terms here are expected to be plain ASCII in practice, and this
   keeps the encoder simple. */
static void UrlEncodeSearchQuery(const wchar_t* src, wchar_t* out, int outCap)
{
    static const wchar_t hex[] = L"0123456789ABCDEF";
    int oi = 0;
    for (const wchar_t* p = src; *p && oi < outCap - 4; p++) {
        wchar_t c = *p;
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
            c == L'-' || c == L'_' || c == L'.' || c == L'~') {
            out[oi++] = c;
        } else if (c == L' ') {
            out[oi++] = L'+';
        } else if (c < 128) {
            out[oi++] = L'%';
            out[oi++] = hex[(c >> 4) & 0xF];
            out[oi++] = hex[c & 0xF];
        }
    }
    out[oi] = 0;
}

static DWORD WINAPI ModBrowserFetchThreadProc(LPVOID lpParam)
{
    ModFetchThreadParam* tp = (ModFetchThreadParam*)lpParam;
    HWND targetWnd = tp->hwnd;
    int page = tp->page;
    ModSortMode sortMode = tp->sortMode;
    wchar_t searchQuery[128];
    wcscpy(searchQuery, tp->searchQuery);
    free(tp);

    BOOL isSearch = (searchQuery[0] != 0);
    wchar_t searchQueryEncoded[384] = L"";
    if (isSearch) UrlEncodeSearchQuery(searchQuery, searchQueryEncoded, 384);

    ModFetchResult* result = (ModFetchResult*)calloc(1, sizeof(ModFetchResult));
    if (!result) return 0;

    HRESULT coHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IWICImagingFactory* pFactory = NULL;
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
        &IID_IWICImagingFactory, (void**)&pFactory);

    ModEntry* entries = (ModEntry*)calloc(MODS_PER_PAGE, sizeof(ModEntry));
    int count = 0;
    int pagesChecked = 0;
    BOOL anyFetchOk = FALSE;
    BOOL rawPageWasFull = TRUE; /* becomes FALSE once a raw page comes back short, meaning no more data */
    wchar_t lastErr[256] = L"";
    const wchar_t* sortParam = ModSortServerValue(sortMode);

    while (entries && count < MODS_PER_PAGE && pagesChecked < MOD_FETCH_MAX_RAW_PAGES && rawPageWasFull) {
        if (pagesChecked > 0) Sleep(200); /* be a reasonable API citizen across a multi-page scan */

        wchar_t apiUrl[768];
        if (isSearch) {
            /* Confirmed against a real captured browser request AND
               response: apiv13's Util/Search/Results, with _idGameRow/
               _sSearchString/_sOrder/_csvFields. The response confirmed
               _idGameRow genuinely filters correctly - 15 for 15 records
               in the captured response belonged to our game, even with
               the search string left as placeholder text - so unlike
               Mod/Index's old Generic_Game filter, this one has real
               evidence behind it, not just hope. Only _sOrder=best_match
               is confirmed - the current sort mode isn't applied while a
               search is active, since I don't have confirmed _sOrder
               values beyond that one. Results mix every submission type
               (Mod, Wip, Sound, Thread, ...), filtered below to
               _sModelName=="Mod" only. */
            wsprintfW(apiUrl,
                L"https://gamebanana.com/apiv13/Util/Search/Results?_sOrder=best_match&_idGameRow=%ld&_sSearchString=%s"
                L"&_csvFields=name%%2Cdescription%%2Carticle%%2Cattribs%%2Cstudio%%2Cowner%%2Ccredits&_nPage=%d",
                GAMEBANANA_GAME_ID, searchQueryEncoded, page);
        } else if (sortParam) {
            wsprintfW(apiUrl,
                L"https://gamebanana.com/apiv13/Mod/Index?_nPage=%d&_nPerpage=%d&_aFilters%%5BGeneric_Category%%5D=%ld&_sSort=%s",
                page, MOD_FETCH_RAW_PERPAGE, GAMEBANANA_CATEGORY_ID, sortParam);
        } else {
            wsprintfW(apiUrl,
                L"https://gamebanana.com/apiv13/Mod/Index?_nPage=%d&_nPerpage=%d&_aFilters%%5BGeneric_Category%%5D=%ld",
                page, MOD_FETCH_RAW_PERPAGE, GAMEBANANA_CATEGORY_ID);
        }

        wchar_t errMsg[256] = L"";
        DWORD jsonSize = 0;
        BYTE* jsonData = HttpGetToMemory(apiUrl, &jsonSize, errMsg);
        pagesChecked++;
        page++;
        if (!jsonData) {
            wcsncpy(lastErr, errMsg[0] ? errMsg : L"Couldn't reach GameBanana.", 255);
            continue; /* could be transient - keep trying the next page */
        }

        char* json = (char*)malloc((size_t)jsonSize + 1);
        if (!json) { free(jsonData); wcscpy(lastErr, L"Out of memory."); break; }
        memcpy(json, jsonData, jsonSize);
        json[jsonSize] = 0;
        free(jsonData);
        const char* jsonEnd = json + jsonSize;

        const char* recPos = FindSubstrBounded(json, jsonEnd, "\"_aRecords\"");
        if (!recPos) {
            /* not real JSON we recognize (e.g. an error/block page) - show
               a snippet of whatever actually came back so this is
               diagnosable from the on-screen message, without needing a
               separate debug build */
            wchar_t snippet[181] = L"";
            int snippetBytes = (jsonSize < 140) ? (int)jsonSize : 140;
            int wn = MultiByteToWideChar(CP_UTF8, 0, json, snippetBytes, snippet, 180);
            if (wn <= 0) wn = MultiByteToWideChar(CP_ACP, 0, json, snippetBytes, snippet, 180);
            if (wn > 0) snippet[wn] = 0; else snippet[0] = 0;
            for (wchar_t* p = snippet; *p; p++) {
                if (*p == L'\r' || *p == L'\n' || *p == L'\t') *p = L' ';
            }

            if (snippet[0]) {
                wsprintfW(lastErr, L"GameBanana returned something unexpected:\n%s", snippet);
            } else {
                wcscpy(lastErr, L"GameBanana returned something unexpected (empty/unreadable response).");
            }
            free(json);
            break;
        }
        anyFetchOk = TRUE;

        recPos += strlen("\"_aRecords\"");
        while (recPos < jsonEnd && *recPos != '[') recPos++;
        if (recPos < jsonEnd) recPos++;

        int recordsOnThisPage = 0;
        const char* recStart; const char* recEnd;
        while (NextJsonArrayObject(&recPos, jsonEnd, &recStart, &recEnd)) {
            recordsOnThisPage++;
            /* Once this fetch's batch is full, keep scanning the rest of
               THIS page just to get an accurate count for rawPageWasFull
               below, but skip the expensive per-match work (thumbnail
               download included) - otherwise recordsOnThisPage reflects
               only however many records happened to precede the 20th
               match, which is almost never the page's true size, and
               rawPageWasFull/hasMore ends up wrong on nearly every fetch. */
            if (count >= MODS_PER_PAGE) continue;

            long idRow = 0;
            const char* idPos = FindSubstrBounded(recStart, recEnd, "\"_idRow\":");
            if (!idPos || !ExtractJsonIntAt(idPos + 9, recEnd, &idRow)) continue;

            /* client-side safety net: only keep mods for our configured
               game, regardless of whether the server-side filter above
               actually took effect - this is what makes the paging loop
               above necessary in the first place */
            long gameId = -1;
            const char* gamePos = FindSubstrBounded(recStart, recEnd, "\"_aGame\":");
            if (gamePos) {
                const char* gs; const char* ge;
                if (SpanJsonObjectAfterColon(gamePos + 9, recEnd, &gs, &ge)) {
                    const char* gidPos = FindSubstrBounded(gs, ge, "\"_idRow\":");
                    if (gidPos) ExtractJsonIntAt(gidPos + 9, ge, &gameId);
                }
            }
            if (gameId != GAMEBANANA_GAME_ID) continue;

            /* Mod/Index only ever returns Mod-type records anyway, but
               Util/Search/Results mixes in Wips/Sounds/Threads/Requests/
               etc. - this launcher only installs Mod-type submissions, so
               filter to that regardless of which endpoint this came from. */
            char modelName[16] = {0};
            const char* modelPos = FindSubstrBounded(recStart, recEnd, "\"_sModelName\":");
            if (modelPos) {
                const char* mp = modelPos + 14;
                while (mp < recEnd && (*mp == ' ' || *mp == '\t')) mp++;
                if (mp < recEnd && *mp == '"') {
                    mp++;
                    int mi = 0;
                    while (mp < recEnd && *mp != '"' && mi < 15) modelName[mi++] = *mp++;
                    modelName[mi] = 0;
                }
            }
            if (modelName[0] && strcmp(modelName, "Mod") != 0) continue;

            ModEntry ne; memset(&ne, 0, sizeof(ne));
            ne.modId = idRow;
            wsprintfW(ne.profileUrl, L"https://gamebanana.com/mods/%ld", idRow);

            const char* namePos = FindSubstrBounded(recStart, recEnd, "\"_sName\":");
            if (namePos) ExtractJsonStringAt(namePos + 9, recEnd, ne.name, 128);
            if (!ne.name[0]) wcscpy(ne.name, L"(untitled)");

            const char* subPos = FindSubstrBounded(recStart, recEnd, "\"_aSubmitter\":");
            if (subPos) {
                const char* ss; const char* se;
                if (SpanJsonObjectAfterColon(subPos + 14, recEnd, &ss, &se)) {
                    const char* snamePos = FindSubstrBounded(ss, se, "\"_sName\":");
                    if (snamePos) ExtractJsonStringAt(snamePos + 9, se, ne.author, 64);
                }
            }
            if (!ne.author[0]) wcscpy(ne.author, L"Unknown");

            const char* catPos = FindSubstrBounded(recStart, recEnd, "\"_aRootCategory\":");
            if (catPos) {
                const char* cs; const char* ce;
                if (SpanJsonObjectAfterColon(catPos + strlen("\"_aRootCategory\":"), recEnd, &cs, &ce)) {
                    const char* cnamePos = FindSubstrBounded(cs, ce, "\"_sName\":");
                    if (cnamePos) ExtractJsonStringAt(cnamePos + strlen("\"_sName\":"), ce, ne.category, 64);
                }
            }

            const char* likePos = FindSubstrBounded(recStart, recEnd, "\"_nLikeCount\":");
            if (likePos) ExtractJsonIntAt(likePos + strlen("\"_nLikeCount\":"), recEnd, &ne.likeCount);
            const char* viewPos = FindSubstrBounded(recStart, recEnd, "\"_nViewCount\":");
            if (viewPos) ExtractJsonIntAt(viewPos + strlen("\"_nViewCount\":"), recEnd, &ne.viewCount);
            const char* datePos = FindSubstrBounded(recStart, recEnd, "\"_tsDateAdded\":");
            if (datePos) ExtractJsonIntAt(datePos + strlen("\"_tsDateAdded\":"), recEnd, &ne.dateAdded);

            wchar_t thumbUrl[512] = L"";
            const char* contentPos = FindSubstrBounded(recStart, recEnd, "\"_aPreviewContent\":");
            if (contentPos) {
                const char* cs; const char* ce;
                if (SpanJsonObjectAfterColon(contentPos + 19, recEnd, &cs, &ce)) {
                    const char* ssPos = FindSubstrBounded(cs, ce, "\"screenshot\":");
                    if (ssPos) {
                        const char* ss; const char* se;
                        if (SpanJsonObjectAfterColon(ssPos + 13, ce, &ss, &se)) {
                            wchar_t baseUrl[256] = L"", file220[128] = L"";
                            const char* basePos = FindSubstrBounded(ss, se, "\"_sBaseUrl\":");
                            if (basePos) ExtractJsonStringAt(basePos + 12, se, baseUrl, 256);
                            const char* filePos = FindSubstrBounded(ss, se, "\"_sFile220\":");
                            if (filePos) ExtractJsonStringAt(filePos + 12, se, file220, 128);
                            if (baseUrl[0] && file220[0]) wsprintfW(thumbUrl, L"%s/%s", baseUrl, file220);
                        }
                    }
                }
            }

            if (thumbUrl[0] && pFactory) {
                wchar_t imgErr[256] = L"";
                DWORD imgSize = 0;
                BYTE* imgData = HttpGetToMemory(thumbUrl, &imgSize, imgErr);
                if (imgData) {
                    ne.thumb = DecodeImageToBitmap(pFactory, imgData, imgSize);
                    free(imgData);
                }
            }

            entries[count++] = ne;
        }

        free(json);
        rawPageWasFull = (recordsOnThisPage >= MOD_FETCH_RAW_PERPAGE);
    }

    if (pFactory) pFactory->lpVtbl->Release(pFactory);

    /* ok means "the fetch itself worked", independent of whether any
       matches were found - that distinction now reaches the UI instead of
       both cases showing the same "no mods found" text */
    result->ok = anyFetchOk;
    if (!anyFetchOk) {
        wcsncpy(result->errMsg, lastErr[0] ? lastErr : L"Couldn't reach GameBanana.", 255);
    }
    result->entries = entries;
    result->count = count;
    result->hasMore = rawPageWasFull; /* more raw pages may still exist beyond where this stopped */

    if (coHr == S_OK || coHr == S_FALSE) CoUninitialize();

    if (IsWindow(targetWnd)) {
        PostMessageW(targetWnd, WM_APP_MODS_LOADED, (WPARAM)page, (LPARAM)result);
    } else {
        FreeModFetchResult(result);
    }
    return 0;
}

static void StartModFetch(HWND listWnd, int page, ModSortMode sortMode, const wchar_t* searchQuery)
{
    g_modLoadState = MODLOAD_LOADING;
    ModFetchThreadParam* tp = (ModFetchThreadParam*)malloc(sizeof(ModFetchThreadParam));
    if (!tp) {
        g_modLoadState = (g_modCount == 0) ? MODLOAD_ERROR : MODLOAD_LOADED;
        if (g_modCount > 0) g_modHasMore = TRUE;
        wcscpy(g_modErrorMsg, L"Out of memory.");
        return;
    }
    tp->hwnd = listWnd;
    tp->page = page;
    tp->sortMode = sortMode;
    wcsncpy(tp->searchQuery, searchQuery ? searchQuery : L"", 127);
    tp->searchQuery[127] = 0;
    HANDLE hThread = CreateThread(NULL, 0, ModBrowserFetchThreadProc, tp, 0, NULL);
    if (hThread) {
        CloseHandle(hThread);
    } else {
        free(tp);
        g_modLoadState = (g_modCount == 0) ? MODLOAD_ERROR : MODLOAD_LOADED;
        if (g_modCount > 0) g_modHasMore = TRUE;
        wcscpy(g_modErrorMsg, L"Couldn't start the download.");
    }
}

typedef struct DropTargetImpl {
    IDropTarget base;   /* must be first member */
    LONG refCount;
    HWND hwnd;
} DropTargetImpl;

static BOOL DT_FormatAvailable(IDataObject* pDataObj, CLIPFORMAT cf)
{
    FORMATETC fmt = { cf, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    return pDataObj->lpVtbl->QueryGetData(pDataObj, &fmt) == S_OK;
}

static HRESULT STDMETHODCALLTYPE DT_QueryInterface(IDropTarget* this_, REFIID riid, void** ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropTarget)) {
        *ppv = this_;
        this_->lpVtbl->AddRef(this_);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE DT_AddRef(IDropTarget* this_)
{
    DropTargetImpl* self = (DropTargetImpl*)this_;
    return (ULONG)InterlockedIncrement(&self->refCount);
}

static ULONG STDMETHODCALLTYPE DT_Release(IDropTarget* this_)
{
    DropTargetImpl* self = (DropTargetImpl*)this_;
    LONG c = InterlockedDecrement(&self->refCount);
    if (c == 0) free(self);
    return (ULONG)c;
}

static HRESULT STDMETHODCALLTYPE DT_DragEnter(IDropTarget* this_, IDataObject* pDataObj,
    DWORD grfKeyState, POINTL pt, DWORD* pdwEffect)
{
    (void)this_; (void)grfKeyState; (void)pt;
    BOOL ok = DT_FormatAvailable(pDataObj, CF_HDROP) || DT_FormatAvailable(pDataObj, CF_UNICODETEXT);
    *pdwEffect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE DT_DragOver(IDropTarget* this_, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect)
{
    DropTargetImpl* self = (DropTargetImpl*)this_;
    (void)grfKeyState;
    POINT cpt; cpt.x = pt.x; cpt.y = pt.y;
    ScreenToClient(self->hwnd, &cpt);
    *pdwEffect = PtInRect(&g_dropZoneRect, cpt) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE DT_DragLeave(IDropTarget* this_)
{
    (void)this_;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE DT_Drop(IDropTarget* this_, IDataObject* pDataObj,
    DWORD grfKeyState, POINTL pt, DWORD* pdwEffect)
{
    DropTargetImpl* self = (DropTargetImpl*)this_;
    (void)grfKeyState;

    POINT cpt; cpt.x = pt.x; cpt.y = pt.y;
    ScreenToClient(self->hwnd, &cpt);
    if (!PtInRect(&g_dropZoneRect, cpt)) {
        *pdwEffect = DROPEFFECT_NONE;
        return S_OK;
    }

    FORMATETC fmtDrop = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM stg;
    if (pDataObj->lpVtbl->GetData(pDataObj, &fmtDrop, &stg) == S_OK) {
        HDROP hDrop = (HDROP)GlobalLock(stg.hGlobal);
        if (hDrop) {
            UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
            for (UINT i = 0; i < count; i++) {
                wchar_t path[MAX_PATH];
                if (DragQueryFileW(hDrop, i, path, MAX_PATH)) {
                    wchar_t destFolder[64];
                    GetCurrentPackFolder(destFolder, 64);
                    StartInstallJob(self->hwnd, FALSE, path, destFolder);
                }
            }
            GlobalUnlock(stg.hGlobal);
        }
        ReleaseStgMedium(&stg);
        *pdwEffect = DROPEFFECT_COPY;
        return S_OK;
    }

    FORMATETC fmtText = { CF_UNICODETEXT, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    if (pDataObj->lpVtbl->GetData(pDataObj, &fmtText, &stg) == S_OK) {
        wchar_t* text = (wchar_t*)GlobalLock(stg.hGlobal);
        if (text) {
            wchar_t url[2048];
            wcsncpy(url, text, 2047);
            url[2047] = 0;
            GlobalUnlock(stg.hGlobal);
            ReleaseStgMedium(&stg);

            int len = (int)wcslen(url);
            while (len > 0 && (url[len-1]=='\r'||url[len-1]=='\n'||url[len-1]==' '||url[len-1]=='\t')) url[--len]=0;

            if (_wcsnicmp(url, L"http://", 7) == 0 || _wcsnicmp(url, L"https://", 8) == 0) {
                wchar_t destFolder[64];
                GetCurrentPackFolder(destFolder, 64);
                StartInstallJob(self->hwnd, TRUE, url, destFolder);
            } else {
                MessageBoxW(self->hwnd, L"That doesn't look like a web link.",
                    L"The Choicer Voicer - Launcher", MB_ICONWARNING | MB_OK);
            }
        } else {
            ReleaseStgMedium(&stg);
        }
        *pdwEffect = DROPEFFECT_COPY;
        return S_OK;
    }

    *pdwEffect = DROPEFFECT_NONE;
    return S_OK;
}

static IDropTargetVtbl g_dropTargetVtbl = {
    DT_QueryInterface, DT_AddRef, DT_Release,
    DT_DragEnter, DT_DragOver, DT_DragLeave, DT_Drop
};

static DropTargetImpl* g_pDropTarget = NULL;

/* ===================== theming ===================== */

static void ApplyDarkTitlebar(HWND hwnd, BOOL dark)
{
    BOOL value = dark;
    if (DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &value, sizeof(value)) != S_OK) {
        DwmSetWindowAttribute(hwnd, 19, &value, sizeof(value)); /* older Win10 builds */
    }
}

static void ApplyThemeColors(HWND hwnd)
{
    if (g_hBrushBg) DeleteObject(g_hBrushBg);
    if (g_hBrushPanel) DeleteObject(g_hBrushPanel);

    g_hBrushBg = CreateSolidBrush(g_darkMode ? COLOR_DARK_BG : COLOR_LIGHT_BG);
    g_hBrushPanel = CreateSolidBrush(g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL);

    ApplyDarkTitlebar(hwnd, g_darkMode);

    const wchar_t* comboTheme = g_darkMode ? L"DarkMode_Explorer" : L"Explorer";
    if (g_hNormalCombo) SetWindowTheme(g_hNormalCombo, comboTheme, NULL);
    if (g_hCompatCombo) SetWindowTheme(g_hCompatCombo, comboTheme, NULL);
    if (g_hPackCombo)   SetWindowTheme(g_hPackCombo, comboTheme, NULL);
    if (g_hModSortCombo) SetWindowTheme(g_hModSortCombo, comboTheme, NULL);

    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

/* ===================== UI helpers ===================== */

/* Tracks mouse hover per-button (via a window prop) so WM_DRAWITEM can show
   a subtle highlight, matching the hover feedback comboboxes already have
   natively. */
static LRESULT CALLBACK ButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    (void)dwRefData;
    switch (msg) {
    case WM_MOUSEMOVE:
        if (!GetPropW(hwnd, L"CVHover")) {
            SetPropW(hwnd, L"CVHover", (HANDLE)1);
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            tme.dwHoverTime = 0;
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        RemovePropW(hwnd, L"CVHover");
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_NCDESTROY:
        RemovePropW(hwnd, L"CVHover");
        RemoveWindowSubclass(hwnd, ButtonSubclassProc, uIdSubclass);
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

/* Owner-drawn so it can be recolored for dark mode and hover reliably
   (comctl32-themed push buttons don't respond to WM_CTLCOLORBTN or track
   hover on their own). */
static HWND MakeButton(HWND parent, HINSTANCE hInst, const wchar_t* text,
                        int x, int y, int w, int ht, int id, HFONT font)
{
    HWND ctl = CreateWindowW(L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        x, y, w, ht, parent, (HMENU)(INT_PTR)id, hInst, NULL);
    if (font) SendMessageW(ctl, WM_SETFONT, (WPARAM)font, TRUE);
    SetWindowSubclass(ctl, ButtonSubclassProc, 1, 0);
    return ctl;
}

static HWND MakeCombo(HWND parent, HINSTANCE hInst, int x, int y, int w, int ht, int id, HFONT font)
{
    HWND ctl = CreateWindowW(L"COMBOBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        x, y, w, ht, parent, (HMENU)(INT_PTR)id, hInst, NULL);
    if (font) SendMessageW(ctl, WM_SETFONT, (WPARAM)font, TRUE);
    return ctl;
}

static HWND MakeGroupBox(HWND parent, HINSTANCE hInst, const wchar_t* text,
                          int x, int y, int w, int ht, HFONT font)
{
    /* A native BS_GROUPBOX only recolors its title strip via WM_CTLCOLORBTN -
       its interior fill is theme-drawn and ignores that brush entirely, which
       is why it stayed white in dark mode. A plain bordered STATIC panel
       fills its whole rect reliably (same mechanism the title/subtitle
       labels already use correctly), so build the "card" out of that plus
       a separate title label on top instead. */
    HWND panel = CreateWindowW(L"STATIC", NULL,
        WS_CHILD | WS_VISIBLE | WS_BORDER,
        x, y, w, ht, parent, NULL, hInst, NULL);

    HWND title = CreateWindowW(L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x + 10, y + 6, w - 20, 20, parent, NULL, hInst, NULL);
    if (font) SendMessageW(title, WM_SETFONT, (WPARAM)font, TRUE);

    /* MakeGroupBox is only ever used for the two Launcher-tab mode cards,
       so it's safe to always track both of its windows for tab-switching */
    TrackLauncherPageWnd(panel);
    TrackLauncherPageWnd(title);

    return panel;
}

/* ===================== Browse Mods tab: scrollable mod list window ===================== */

#define MODLIST_ROW_H     84
#define MODLIST_THUMB     64
#define MODLIST_PAD       10
#define MODLIST_BTN_W     90
#define MODLIST_BTN_H     30
#define MODLIST_HEADER_H  40
#define MODLIST_PAGEBAR_H 44
#define MODLIST_PAGEBTN_W 32
#define MODLIST_PAGEBTN_H 26

static RECT ModListInstallBtnRect(RECT clientRc, int rowScreenY)
{
    RECT r;
    r.right = clientRc.right - MODLIST_PAD;
    r.left = r.right - MODLIST_BTN_W;
    r.top = rowScreenY + (MODLIST_ROW_H - MODLIST_BTN_H) / 2;
    r.bottom = r.top + MODLIST_BTN_H;
    return r;
}

/* shared geometry for the page bar (Prev / 1 2 3... / Next) at the
   bottom of the list - computed once, used identically by both painting
   and click hit-testing so they can never disagree */
typedef struct {
    RECT prevRc;
    RECT nextRc;
    RECT pageRc[7];
    int pageNum[7];
    int visibleCount;
    int totalPages;
} PagebarLayout;

static void ComputePagebarLayout(RECT clientRc, PagebarLayout* out)
{
    memset(out, 0, sizeof(*out));
    int totalPages = (g_modFilteredCount + MODLIST_PAGE_SIZE - 1) / MODLIST_PAGE_SIZE;
    if (totalPages < 1) totalPages = 1;
    out->totalPages = totalPages;

    int maxSlots = 7;
    if (maxSlots > totalPages) maxSlots = totalPages;
    int startPage = g_modCurrentPage - maxSlots / 2;
    if (startPage < 0) startPage = 0;
    if (startPage + maxSlots > totalPages) startPage = totalPages - maxSlots;
    if (startPage < 0) startPage = 0;
    out->visibleCount = maxSlots;

    int barTop = clientRc.bottom - MODLIST_PAGEBAR_H;
    int barMidY = barTop + MODLIST_PAGEBAR_H / 2;
    int btnGap = 4;
    int totalBtnsWidth = maxSlots * MODLIST_PAGEBTN_W + (maxSlots > 0 ? (maxSlots - 1) * btnGap : 0);
    int centerX = clientRc.right / 2;
    int firstX = centerX - totalBtnsWidth / 2;

    for (int i = 0; i < maxSlots; i++) {
        int x = firstX + i * (MODLIST_PAGEBTN_W + btnGap);
        RECT r = { x, barMidY - MODLIST_PAGEBTN_H / 2, x + MODLIST_PAGEBTN_W, barMidY + MODLIST_PAGEBTN_H / 2 };
        out->pageRc[i] = r;
        out->pageNum[i] = startPage + i;
    }

    RECT prev = { firstX - MODLIST_PAD - 60, barMidY - MODLIST_PAGEBTN_H / 2,
                  firstX - MODLIST_PAD, barMidY + MODLIST_PAGEBTN_H / 2 };
    out->prevRc = prev;

    int lastX = firstX + (maxSlots > 0 ? maxSlots * (MODLIST_PAGEBTN_W + btnGap) : 0);
    RECT next = { lastX + MODLIST_PAD, barMidY - MODLIST_PAGEBTN_H / 2,
                  lastX + MODLIST_PAD + 60, barMidY + MODLIST_PAGEBTN_H / 2 };
    out->nextRc = next;
}

static void ModListDoInstall(HWND listWnd, int index)
{
    if (index < 0 || index >= g_modCount) return;
    HWND mainWnd = GetParent(listWnd);
    ShowDestPicker(mainWnd, g_hInstance, g_modEntries[index].profileUrl);
}

static BOOL WStrContainsCI(const wchar_t* haystack, const wchar_t* needle)
{
    if (!needle || !needle[0]) return TRUE;
    size_t hn = wcslen(haystack), nn = wcslen(needle);
    if (nn > hn) return FALSE;
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t j = 0;
        while (j < nn && towlower(haystack[i + j]) == towlower(needle[j])) j++;
        if (j == nn) return TRUE;
    }
    return FALSE;
}

static ModSortMode g_modSortModeForCompare = MODSORT_NEWEST;

static int ModEntryCompare(const void* a, const void* b)
{
    const ModEntry* ea = (const ModEntry*)a;
    const ModEntry* eb = (const ModEntry*)b;
    switch (g_modSortModeForCompare) {
    case MODSORT_NAME:
        return _wcsicmp(ea->name, eb->name);
    case MODSORT_LIKES:
        if (eb->likeCount != ea->likeCount) return (eb->likeCount > ea->likeCount) ? 1 : -1;
        return 0;
    case MODSORT_VIEWS:
        if (eb->viewCount != ea->viewCount) return (eb->viewCount > ea->viewCount) ? 1 : -1;
        return 0;
    case MODSORT_NEWEST:
    default:
        if (eb->dateAdded > ea->dateAdded) return 1;
        if (eb->dateAdded < ea->dateAdded) return -1;
        return 0;
    }
}

/* re-sorts g_modEntries in place per g_modSortMode and rebuilds
   g_modFilteredIndices per g_modSearchQuery - called any time entries are
   added, the search text changes, or the sort mode changes */
static void RebuildModListView(void)
{
    g_modSortModeForCompare = g_modSortMode;
    if (g_modCount > 0) qsort(g_modEntries, (size_t)g_modCount, sizeof(ModEntry), ModEntryCompare);

    g_modFilteredCount = 0;
    for (int i = 0; i < g_modCount; i++) {
        ModEntry* e = &g_modEntries[i];
        /* When a search is active, what's cached was already fetched via
           a server-side Util/Search/Results query (see StartModFetch/
           WM_TIMER) - re-checking name/author/category here could reject
           genuine matches the server found via description/tags/other
           fields this client-side check doesn't look at. Only re-filter
           locally when there's no active search (plain browse mode). */
        BOOL matches = (g_modSearchQuery[0] != 0) ? TRUE
                     : (WStrContainsCI(e->name, g_modSearchQuery)
                     || WStrContainsCI(e->author, g_modSearchQuery)
                     || WStrContainsCI(e->category, g_modSearchQuery));
        if (matches && g_modFilteredCount < MAX_MOD_ENTRIES) {
            g_modFilteredIndices[g_modFilteredCount++] = i;
        }
    }

    int totalPages = (g_modFilteredCount + MODLIST_PAGE_SIZE - 1) / MODLIST_PAGE_SIZE;
    if (totalPages < 1) totalPages = 1;

    if (g_modPendingAdvance && g_modCurrentPage + 1 < totalPages) {
        g_modCurrentPage++;
        g_modPendingAdvance = FALSE;
    }
    if (g_modCurrentPage >= totalPages) g_modCurrentPage = totalPages - 1;
    if (g_modCurrentPage < 0) g_modCurrentPage = 0;

    if (g_hModListWnd) {
        int pageStart = g_modCurrentPage * MODLIST_PAGE_SIZE;
        int pageEnd = pageStart + MODLIST_PAGE_SIZE;
        if (pageEnd > g_modFilteredCount) pageEnd = g_modFilteredCount;
        int rowsOnPage = pageEnd - pageStart;
        if (rowsOnPage < 0) rowsOnPage = 0;

        RECT rc; GetClientRect(g_hModListWnd, &rc);
        int totalH = MODLIST_HEADER_H + rowsOnPage * MODLIST_ROW_H;
        int visibleH = rc.bottom - MODLIST_PAGEBAR_H;
        int maxScroll = totalH - visibleH;
        if (maxScroll < 0) maxScroll = 0;
        if (g_modScrollY > maxScroll) g_modScrollY = maxScroll;
        if (g_modScrollY < 0) g_modScrollY = 0;
    }
}

static LRESULT CALLBACK ModListWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);

        g_hModSearchEdit = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            10, 8, 300, 24, hwnd, (HMENU)(INT_PTR)ID_MODLIST_SEARCH, hInst, NULL);
        SendMessageW(g_hModSearchEdit, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);

        g_hModSortCombo = CreateWindowW(L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            320, 8, 220, 200, hwnd, (HMENU)(INT_PTR)ID_MODLIST_SORT, hInst, NULL);
        SendMessageW(g_hModSortCombo, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);
        SendMessageW(g_hModSortCombo, CB_ADDSTRING, 0, (LPARAM)L"Sort: Newest");
        SendMessageW(g_hModSortCombo, CB_ADDSTRING, 0, (LPARAM)L"Sort: Name (A-Z)");
        SendMessageW(g_hModSortCombo, CB_ADDSTRING, 0, (LPARAM)L"Sort: Most Liked");
        SendMessageW(g_hModSortCombo, CB_ADDSTRING, 0, (LPARAM)L"Sort: Most Viewed");
        SendMessageW(g_hModSortCombo, CB_SETCURSEL, (WPARAM)g_modSortMode, 0);
        SetWindowTheme(g_hModSortCombo, g_darkMode ? L"DarkMode_Explorer" : L"Explorer", NULL);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        HBRUSH bg = CreateSolidBrush(g_darkMode ? COLOR_DARK_BG : COLOR_LIGHT_BG);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        SetBkMode(hdc, TRANSPARENT);
        COLORREF textColor = g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT;

        int contentBottom = rc.bottom - MODLIST_PAGEBAR_H;
        int pageStart = g_modCurrentPage * MODLIST_PAGE_SIZE;
        int pageEnd = pageStart + MODLIST_PAGE_SIZE;
        if (pageEnd > g_modFilteredCount) pageEnd = g_modFilteredCount;

        HDC memDC = CreateCompatibleDC(hdc);

        for (int i = pageStart; i < pageEnd; i++) {
            int ri = i - pageStart;
            int y = MODLIST_HEADER_H + ri * MODLIST_ROW_H - g_modScrollY;
            if (y + MODLIST_ROW_H < MODLIST_HEADER_H || y > contentBottom) continue;

            ModEntry* e = &g_modEntries[g_modFilteredIndices[i]];
            RECT thumbRc = { MODLIST_PAD, y + MODLIST_PAD,
                              MODLIST_PAD + MODLIST_THUMB, y + MODLIST_PAD + MODLIST_THUMB };
            if (e->thumb) {
                HGDIOBJ oldBmp = SelectObject(memDC, e->thumb);
                BITMAP bm; GetObjectW(e->thumb, sizeof(bm), &bm);
                SetStretchBltMode(hdc, HALFTONE);
                SetBrushOrgEx(hdc, 0, 0, NULL);
                StretchBlt(hdc, thumbRc.left, thumbRc.top, MODLIST_THUMB, MODLIST_THUMB,
                    memDC, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                SelectObject(memDC, oldBmp);
            } else {
                HBRUSH ph = CreateSolidBrush(g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL);
                FillRect(hdc, &thumbRc, ph);
                DeleteObject(ph);
            }
            HPEN pen = CreatePen(PS_SOLID, 1, textColor);
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, thumbRc.left, thumbRc.top, thumbRc.right, thumbRc.bottom);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);

            RECT textRc = { thumbRc.right + MODLIST_PAD, y + MODLIST_PAD,
                             rc.right - MODLIST_PAD - MODLIST_BTN_W - MODLIST_PAD, y + MODLIST_PAD + 20 };
            SetTextColor(hdc, textColor);
            HGDIOBJ oldFont = SelectObject(hdc, g_fontRegular);
            DrawTextW(hdc, e->name, -1, &textRc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

            RECT authorRc = textRc;
            authorRc.top += 22; authorRc.bottom += 22;
            wchar_t byLine[160];
            wsprintfW(byLine, L"by %s", e->author);
            SelectObject(hdc, g_fontSmall);
            DrawTextW(hdc, byLine, -1, &authorRc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

            /* mod type/category (Mod Packs, Hosts, Dub Mode, Skins, etc.) */
            RECT catRc = authorRc;
            catRc.top += 18; catRc.bottom += 18;
            DrawTextW(hdc, e->category[0] ? e->category : L"Mod", -1, &catRc,
                DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(hdc, oldFont);

            RECT btnRc = ModListInstallBtnRect(rc, y);
            HBRUSH btnBrush = CreateSolidBrush(g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL);
            FillRect(hdc, &btnRc, btnBrush);
            DeleteObject(btnBrush);
            HPEN btnPen = CreatePen(PS_SOLID, 1, textColor);
            HGDIOBJ oldBtnPen = SelectObject(hdc, btnPen);
            HGDIOBJ oldBtnBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, btnRc.left, btnRc.top, btnRc.right, btnRc.bottom);
            SelectObject(hdc, oldBtnBrush);
            SelectObject(hdc, oldBtnPen);
            DeleteObject(btnPen);
            SelectObject(hdc, g_fontRegular);
            DrawTextW(hdc, L"Install", -1, &btnRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            HPEN sepPen = CreatePen(PS_SOLID, 1, g_darkMode ? RGB(60, 60, 60) : RGB(220, 220, 220));
            HGDIOBJ oldSepPen = SelectObject(hdc, sepPen);
            MoveToEx(hdc, MODLIST_PAD, y, NULL);
            LineTo(hdc, rc.right - MODLIST_PAD, y);
            SelectObject(hdc, oldSepPen);
            DeleteObject(sepPen);
        }

        DeleteDC(memDC);

        /* empty-state messaging, drawn in the row area (not the page bar) */
        if (g_modLoadState != MODLOAD_LOADING && g_modLoadState != MODLOAD_ERROR) {
            RECT emptyRc = { 0, MODLIST_HEADER_H, rc.right, contentBottom };
            SetTextColor(hdc, textColor);
            SelectObject(hdc, g_fontRegular);
            if (g_modCount == 0) {
                DrawTextW(hdc, L"No mods found for this game yet.", -1, &emptyRc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else if (g_modFilteredCount == 0) {
                DrawTextW(hdc, L"No mods match your search.", -1, &emptyRc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }

        /* page bar - fixed at the bottom, not part of the scrolling area */
        RECT barRc = { 0, contentBottom, rc.right, rc.bottom };
        HBRUSH barBg = CreateSolidBrush(g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL);
        FillRect(hdc, &barRc, barBg);
        DeleteObject(barBg);
        HPEN barLinePen = CreatePen(PS_SOLID, 1, g_darkMode ? RGB(60, 60, 60) : RGB(220, 220, 220));
        HGDIOBJ oldBarLinePen = SelectObject(hdc, barLinePen);
        MoveToEx(hdc, 0, contentBottom, NULL);
        LineTo(hdc, rc.right, contentBottom);
        SelectObject(hdc, oldBarLinePen);
        DeleteObject(barLinePen);

        SetTextColor(hdc, textColor);
        SelectObject(hdc, g_fontRegular);

        if (g_modLoadState == MODLOAD_LOADING) {
            DrawTextW(hdc, L"Loading mods\u2026", -1, &barRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (g_modLoadState == MODLOAD_ERROR) {
            wchar_t msg[320];
            wsprintfW(msg, L"%s  (click to retry)", g_modErrorMsg);
            DrawTextW(hdc, msg, -1, &barRc, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        } else {
            PagebarLayout lay;
            ComputePagebarLayout(rc, &lay);
            BOOL canPrev = (g_modCurrentPage > 0);
            BOOL canNext = (g_modCurrentPage + 1 < lay.totalPages) || g_modHasMore;

            HPEN ctrlPen = CreatePen(PS_SOLID, 1, textColor);
            HGDIOBJ oldCtrlPen = SelectObject(hdc, ctrlPen);
            HGDIOBJ oldCtrlBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

            SetTextColor(hdc, canPrev ? textColor : RGB(140, 140, 140));
            Rectangle(hdc, lay.prevRc.left, lay.prevRc.top, lay.prevRc.right, lay.prevRc.bottom);
            DrawTextW(hdc, L"< Prev", -1, &lay.prevRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            for (int i = 0; i < lay.visibleCount; i++) {
                BOOL isCurrent = (lay.pageNum[i] == g_modCurrentPage);
                if (isCurrent) {
                    HBRUSH curBg = CreateSolidBrush(g_darkMode ? COLOR_DARK_BG : COLOR_LIGHT_BG);
                    FillRect(hdc, &lay.pageRc[i], curBg);
                    DeleteObject(curBg);
                }
                SetTextColor(hdc, textColor);
                Rectangle(hdc, lay.pageRc[i].left, lay.pageRc[i].top, lay.pageRc[i].right, lay.pageRc[i].bottom);
                wchar_t num[8];
                wsprintfW(num, L"%d", lay.pageNum[i] + 1);
                DrawTextW(hdc, num, -1, &lay.pageRc[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            SetTextColor(hdc, canNext ? textColor : RGB(140, 140, 140));
            Rectangle(hdc, lay.nextRc.left, lay.nextRc.top, lay.nextRc.right, lay.nextRc.bottom);
            DrawTextW(hdc, L"Next >", -1, &lay.nextRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdc, oldCtrlBrush);
            SelectObject(hdc, oldCtrlPen);
            DeleteObject(ctrlPen);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int mouseX = GET_X_LPARAM(lParam);
        int mouseY = GET_Y_LPARAM(lParam);
        RECT rc; GetClientRect(hwnd, &rc);
        int contentBottom = rc.bottom - MODLIST_PAGEBAR_H;

        if (mouseY >= contentBottom) {
            /* click landed in the page bar */
            if (g_modLoadState == MODLOAD_ERROR) {
                StartModFetch(hwnd, g_modNextPage, g_modSortMode, g_modSearchQuery);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (g_modLoadState == MODLOAD_LOADING) return 0;

            PagebarLayout lay;
            ComputePagebarLayout(rc, &lay);

            if (mouseX >= lay.prevRc.left && mouseX <= lay.prevRc.right &&
                mouseY >= lay.prevRc.top && mouseY <= lay.prevRc.bottom) {
                if (g_modCurrentPage > 0) {
                    g_modCurrentPage--;
                    g_modScrollY = 0;
                    RebuildModListView();
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }
            if (mouseX >= lay.nextRc.left && mouseX <= lay.nextRc.right &&
                mouseY >= lay.nextRc.top && mouseY <= lay.nextRc.bottom) {
                if (g_modCurrentPage + 1 < lay.totalPages) {
                    g_modCurrentPage++;
                    g_modScrollY = 0;
                    RebuildModListView();
                    InvalidateRect(hwnd, NULL, FALSE);
                } else if (g_modHasMore) {
                    g_modPendingAdvance = TRUE;
                    StartModFetch(hwnd, g_modNextPage, g_modSortMode, g_modSearchQuery);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }
            for (int i = 0; i < lay.visibleCount; i++) {
                RECT r = lay.pageRc[i];
                if (mouseX >= r.left && mouseX <= r.right && mouseY >= r.top && mouseY <= r.bottom) {
                    if (lay.pageNum[i] != g_modCurrentPage) {
                        g_modCurrentPage = lay.pageNum[i];
                        g_modScrollY = 0;
                        RebuildModListView();
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    return 0;
                }
            }
            return 0;
        }

        int pageStart = g_modCurrentPage * MODLIST_PAGE_SIZE;
        int pageEnd = pageStart + MODLIST_PAGE_SIZE;
        if (pageEnd > g_modFilteredCount) pageEnd = g_modFilteredCount;
        int rowsOnPage = pageEnd - pageStart;

        int absY = mouseY + g_modScrollY - MODLIST_HEADER_H;
        int ri = (absY >= 0) ? (absY / MODLIST_ROW_H) : -1;
        if (ri >= 0 && ri < rowsOnPage) {
            int rowScreenY = MODLIST_HEADER_H + ri * MODLIST_ROW_H - g_modScrollY;
            RECT btnRc = ModListInstallBtnRect(rc, rowScreenY);
            if (mouseX >= btnRc.left && mouseX <= btnRc.right && mouseY >= btnRc.top && mouseY <= btnRc.bottom) {
                ModListDoInstall(hwnd, g_modFilteredIndices[pageStart + ri]);
            }
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        RECT rc; GetClientRect(hwnd, &rc);
        int contentBottom = rc.bottom - MODLIST_PAGEBAR_H;
        int pageStart = g_modCurrentPage * MODLIST_PAGE_SIZE;
        int pageEnd = pageStart + MODLIST_PAGE_SIZE;
        if (pageEnd > g_modFilteredCount) pageEnd = g_modFilteredCount;
        int rowsOnPage = pageEnd - pageStart;
        if (rowsOnPage < 0) rowsOnPage = 0;

        int totalH = MODLIST_HEADER_H + rowsOnPage * MODLIST_ROW_H;
        int maxScroll = totalH - contentBottom;
        if (maxScroll < 0) maxScroll = 0;
        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        g_modScrollY -= (delta / WHEEL_DELTA) * MODLIST_ROW_H;
        if (g_modScrollY < 0) g_modScrollY = 0;
        if (g_modScrollY > maxScroll) g_modScrollY = maxScroll;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_TIMER: {
        if (wParam == MODLIST_SEARCH_DEBOUNCE_TIMER_ID) {
            KillTimer(hwnd, MODLIST_SEARCH_DEBOUNCE_TIMER_ID);
            GetWindowTextW(g_hModSearchEdit, g_modSearchQuery, 127);
            g_modCurrentPage = 0;
            g_modScrollY = 0;

            /* a non-empty query is a fresh Util/Search/Results query;
               clearing it returns to the regular Mod/Index browse fetch -
               either way what's cached was fetched under a different
               query/endpoint, so start over rather than locally
               re-filtering the old cache */
            for (int i = 0; i < g_modCount; i++) {
                if (g_modEntries[i].thumb) DeleteObject(g_modEntries[i].thumb);
            }
            g_modCount = 0;
            g_modFilteredCount = 0;
            g_modNextPage = 1;
            g_modPendingAdvance = FALSE;
            StartModFetch(g_hModListWnd, g_modNextPage, g_modSortMode, g_modSearchQuery);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_MODLIST_SEARCH && HIWORD(wParam) == EN_CHANGE) {
            /* debounce - wait for the user to pause typing before firing a
               fresh server request, rather than one per keystroke */
            SetTimer(hwnd, MODLIST_SEARCH_DEBOUNCE_TIMER_ID, MODLIST_SEARCH_DEBOUNCE_MS, NULL);
            return 0;
        }
        if (LOWORD(wParam) == ID_MODLIST_SORT && HIWORD(wParam) == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(g_hModSortCombo, CB_GETCURSEL, 0, 0);
            if (sel >= 0) g_modSortMode = (ModSortMode)sel;
            g_modCurrentPage = 0;
            g_modScrollY = 0;

            if (ModSortServerValue(g_modSortMode) != NULL) {
                /* Likes/Views are real server-side orderings - what's
                   cached so far was fetched under a DIFFERENT order (or
                   no particular order), so "most liked among an arbitrary
                   newest-biased sample" isn't the same thing as "most
                   liked overall". Start over with a fresh fetch in the
                   actual requested order instead of just re-sorting the
                   old cache. */
                for (int i = 0; i < g_modCount; i++) {
                    if (g_modEntries[i].thumb) DeleteObject(g_modEntries[i].thumb);
                }
                g_modCount = 0;
                g_modFilteredCount = 0;
                g_modNextPage = 1;
                g_modPendingAdvance = FALSE;
                StartModFetch(g_hModListWnd, g_modNextPage, g_modSortMode, g_modSearchQuery);
            } else {
                /* Newest/Name have no server equivalent (Newest is just
                   the default fetch order anyway) - re-sorting whatever
                   is already cached, using its real stored dates/names,
                   is correct regardless of what order it was fetched in. */
                RebuildModListView();
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;

    case WM_APP_MODS_LOADED: {
        ModFetchResult* result = (ModFetchResult*)lParam;
        int page = (int)wParam;
        if (result) {
            if (result->ok) {
                int room = MAX_MOD_ENTRIES - g_modCount;
                int toCopy = result->count < room ? result->count : room;
                for (int i = 0; i < toCopy; i++) g_modEntries[g_modCount++] = result->entries[i];
                /* anything beyond capacity is discarded - free its bitmap
                   since ownership wasn't transferred */
                for (int i = toCopy; i < result->count; i++) {
                    if (result->entries[i].thumb) DeleteObject(result->entries[i].thumb);
                }
                g_modLoadState = MODLOAD_LOADED;
                g_modHasMore = result->hasMore && (room > toCopy || toCopy == result->count);
                /* page (wParam) is already the next unfetched raw page - the
                   worker thread increments it internally as it walks
                   forward across however many raw pages it had to check */
                g_modNextPage = page;
                RebuildModListView(); /* also auto-advances onto the newly
                                          loaded page if the user was
                                          waiting on Next to bring it in */
            } else {
                if (g_modCount == 0) {
                    /* nothing loaded at all yet - a bare error+retry
                       screen makes sense since there's nothing to browse */
                    g_modLoadState = MODLOAD_ERROR;
                } else {
                    /* mods from earlier fetches are already loaded and
                       browsable - a failed "load more" attempt shouldn't
                       take away Prev/page-number navigation to them. Stay
                       in LOADED so the normal page bar keeps showing;
                       hasMore stays TRUE so Next can be clicked again to
                       retry rather than silently vanishing. */
                    g_modLoadState = MODLOAD_LOADED;
                    g_modHasMore = TRUE;
                }
                g_modPendingAdvance = FALSE;
                wcsncpy(g_modErrorMsg, result->errMsg, 255);
                g_modErrorMsg[255] = 0;
            }
            free(result->entries); /* bitmaps now owned by g_modEntries (or freed above) */
            free(result);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, MODLIST_SEARCH_DEBOUNCE_TIMER_ID);
        for (int i = 0; i < g_modCount; i++) {
            if (g_modEntries[i].thumb) DeleteObject(g_modEntries[i].thumb);
        }
        g_modCount = 0;
        g_modLoadState = MODLOAD_IDLE;
        g_modNextPage = 1;
        g_modHasMore = TRUE;
        g_modScrollY = 0;
        g_modFilteredCount = 0;
        g_modSearchQuery[0] = 0;
        g_modSortMode = MODSORT_NEWEST;
        g_modCurrentPage = 0;
        g_modPendingAdvance = FALSE;
        g_hModSearchEdit = NULL;
        g_hModSortCombo = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static HWND CreateModListWnd(HWND parent, HINSTANCE hInst, int x, int y, int w, int h)
{
    static BOOL classRegistered = FALSE;
    const wchar_t CLASS_NAME[] = L"CVLauncherModListWnd";
    if (!classRegistered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc   = ModListWndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = CLASS_NAME;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        RegisterClassW(&wc);
        classRegistered = TRUE;
    }
    return CreateWindowExW(WS_EX_CLIENTEDGE, CLASS_NAME, NULL,
        WS_CHILD | WS_BORDER,
        x, y, w, h, parent, NULL, hInst, NULL);
}

/* Shows the Launcher tab's own content (real child windows + the overlay-
   drawn text) or the Browse Mods tab's list window, and kicks off the
   first fetch the first time Browse Mods is opened. */
static void SwitchTab(HWND hwnd, AppTab newTab)
{
    if (g_activeTab == newTab) return;
    g_activeTab = newTab;

    BOOL showLauncher = (newTab == TAB_LAUNCHER);
    for (int i = 0; i < g_launcherPageWndCount; i++) {
        ShowWindow(g_launcherPageWnds[i], showLauncher ? SW_SHOW : SW_HIDE);
    }
    for (int i = 0; i < NUM_OVERLAY_LABELS; i++) {
        g_overlayLabels[i].visible = showLauncher;
    }
    if (g_hModListWnd) {
        ShowWindow(g_hModListWnd, showLauncher ? SW_HIDE : SW_SHOW);
    }

    if (newTab == TAB_BROWSE && g_modLoadState == MODLOAD_IDLE) {
        StartModFetch(g_hModListWnd, g_modNextPage, g_modSortMode, g_modSearchQuery);
    }

    if (g_hTabLauncherBtn) InvalidateRect(g_hTabLauncherBtn, NULL, TRUE);
    if (g_hTabBrowseBtn) InvalidateRect(g_hTabBrowseBtn, NULL, TRUE);
    InvalidateRect(hwnd, NULL, FALSE);
}

/* forward declaration - defined near the end of the file, but needed by
   WndProc's WM_COMMAND handler so the header's info button can reopen it */
static void ShowFirstRunPopup(HWND hOwner, HINSTANCE hInst);

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = ((LPCREATESTRUCTW)lParam)->hInstance;

        g_fontTitle = CreateFontW(-22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_fontRegular = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_fontSmall = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        LoadSettings();
        g_hInstance = hInst;

        /* ===== tab row: Launcher / Browse Mods ===== */
        #define TAB_TOP 15
        #define TAB_H   32
        #define TAB_BOTTOM (TAB_TOP + TAB_H)

        g_hTabLauncherBtn = MakeButton(hwnd, hInst, L"Launcher", 15, TAB_TOP, 110, TAB_H, ID_TAB_LAUNCHER, g_fontRegular);
        g_hTabBrowseBtn = MakeButton(hwnd, hInst, L"Browse Mods", 130, TAB_TOP, 140, TAB_H, ID_TAB_BROWSE, g_fontRegular);

        /* ===== header bar: Mod Folder / Get Mods / Info on the left,
           Dark Mode on the far right ===== */
        #define HEADER_TOP (TAB_BOTTOM + 10)
        #define HEADER_H   36
        #define HEADER_BOTTOM (HEADER_TOP + HEADER_H)
        #define CONTENT_TOP (HEADER_BOTTOM + 15)

        MakeButton(hwnd, hInst, L"Mod Folder", 15, HEADER_TOP, 140, HEADER_H, ID_MODDING, g_fontRegular);
        MakeButton(hwnd, hInst, L"Get Mods", 165, HEADER_TOP, 140, HEADER_H, ID_GET_MODS, g_fontRegular);
        MakeButton(hwnd, hInst, L"\u24D8", 315, HEADER_TOP, HEADER_H, HEADER_H, ID_README_INFO, g_fontRegular);

        /* Owner-drawn, like every other header button - a native
           BS_AUTOCHECKBOX keeps its check-glyph background theme-drawn no
           matter what colors are returned from WM_CTLCOLORSTATIC, which is
           exactly why it stood out with a mismatched light box in dark
           mode. Drawing our own glyph avoids that entirely. */
        g_hDarkModeCheck = MakeButton(hwnd, hInst, g_darkMode ? L"\u2611 Dark Mode" : L"\u2610 Dark Mode",
            635 - 130, HEADER_TOP, 130, HEADER_H, IDC_DARKMODE_CHECK, g_fontRegular);

        /* side image */
        g_hBitmap = LoadBitmapW(hInst, MAKEINTRESOURCEW(IDB_SIDEIMAGE));
        HWND hImg = CreateWindowW(L"STATIC", NULL,
            WS_CHILD | WS_VISIBLE | SS_BITMAP,
            15, CONTENT_TOP, 250, 333, hwnd, NULL, hInst, NULL);
        SendMessageW(hImg, STM_SETIMAGE, (WPARAM)IMAGE_BITMAP, (LPARAM)g_hBitmap);
        TrackLauncherPageWnd(hImg);

        int rx = 285;   /* right column x */

        g_overlayLabels[0].text = L"The Choicer Voicer";
        g_overlayLabels[0].rect = (RECT){ rx, CONTENT_TOP + 5, rx + 230, CONTENT_TOP + 5 + 34 };
        g_overlayLabels[0].font = g_fontTitle;
        g_overlayLabels[0].visible = TRUE;

        g_overlayLabels[1].text = L"Launcher; Choose a version from the dropdown windows to play the version you want.";
        g_overlayLabels[1].rect = (RECT){ rx, CONTENT_TOP + 43, rx + 350, CONTENT_TOP + 43 + 54 };
        g_overlayLabels[1].font = g_fontRegular;
        g_overlayLabels[1].visible = TRUE;

        /* Normal Mode card: bordered panel using more of the width/height
           alongside the side image, instead of a thin single-height row */
        MakeGroupBox(hwnd, hInst, L"Normal Mode", rx, CONTENT_TOP + 117, 350, 98, g_fontRegular);
        g_hNormalCombo = MakeCombo(hwnd, hInst, rx + 15, CONTENT_TOP + 151, 215, 200, IDC_NORMAL_COMBO, g_fontRegular);
        g_hLaunchNormalBtn = MakeButton(hwnd, hInst, L"Launch", rx + 240, CONTENT_TOP + 151, 95, 30, ID_LAUNCH_NORMAL, g_fontRegular);
        TrackLauncherPageWnd(g_hNormalCombo);
        TrackLauncherPageWnd(g_hLaunchNormalBtn);

        /* Compatibility Mode card */
        MakeGroupBox(hwnd, hInst, L"Compatibility Mode", rx, CONTENT_TOP + 229, 350, 98, g_fontRegular);
        g_hCompatCombo = MakeCombo(hwnd, hInst, rx + 15, CONTENT_TOP + 263, 215, 200, IDC_COMPAT_COMBO, g_fontRegular);
        g_hLaunchCompatBtn = MakeButton(hwnd, hInst, L"Launch", rx + 240, CONTENT_TOP + 263, 95, 30, ID_LAUNCH_COMPAT, g_fontRegular);
        TrackLauncherPageWnd(g_hCompatCombo);
        TrackLauncherPageWnd(g_hLaunchCompatBtn);

        /* discover versions on disk and fill the two dropdowns */
        {
            wchar_t exeDir[MAX_PATH];
            GetExeDir(exeDir, MAX_PATH);
            wchar_t normalDir[MAX_PATH], compatDir[MAX_PATH];
            wsprintfW(normalDir, L"%s\\%s", exeDir, NORMAL_DIR_NAME);
            wsprintfW(compatDir, L"%s\\%s", exeDir, COMPAT_DIR_NAME);

            ScanModeFolder(normalDir, &g_normalVersions);
            ScanModeFolder(compatDir, &g_compatVersions);

            PopulateVersionCombo(g_hNormalCombo, &g_normalVersions, g_hLaunchNormalBtn);
            PopulateVersionCombo(g_hCompatCombo, &g_compatVersions, g_hLaunchCompatBtn);

            SelectRememberedVersion(g_hNormalCombo, &g_normalVersions, g_lastNormalExe);
            SelectRememberedVersion(g_hCompatCombo, &g_compatVersions, g_lastCompatExe);
        }

        /* Everything below here spans the full window width, so it must start
           below BOTH columns - the side image bottom is the taller of the
           two, not just the right column's own content. The old Get Mods /
           Modding Folder button row now lives in the header instead, so this
           section starts directly with the "install mod to" row. */
        #define MOD_SECTION_TOP (CONTENT_TOP + 333 + 16)

        g_overlayLabels[2].text = L"Install downloaded mod to:";
        g_overlayLabels[2].rect = (RECT){ 15, MOD_SECTION_TOP, 15 + 190, MOD_SECTION_TOP + 20 };
        g_overlayLabels[2].font = g_fontRegular;
        g_overlayLabels[2].visible = TRUE;
        g_hPackCombo = MakeCombo(hwnd, hInst, 210, MOD_SECTION_TOP - 4, 200, 200, IDC_PACK_COMBO, g_fontRegular);
        for (int i = 0; i < NUM_PACK_FOLDERS; i++) {
            SendMessageW(g_hPackCombo, CB_ADDSTRING, 0, (LPARAM)PACK_FOLDERS[i]);
        }
        {
            LRESULT foundIdx = g_lastPackFolder[0]
                ? SendMessageW(g_hPackCombo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)g_lastPackFolder)
                : CB_ERR;
            SendMessageW(g_hPackCombo, CB_SETCURSEL, (foundIdx == CB_ERR) ? 0 : (WPARAM)foundIdx, 0);
        }
        TrackLauncherPageWnd(g_hPackCombo);

        HWND hDrop = CreateWindowW(L"STATIC",
            L"Drag a .zip/.rar file or a GameBanana mod link here",
            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE | WS_BORDER,
            15, MOD_SECTION_TOP + 28, 620, 70, hwnd, (HMENU)(INT_PTR)IDC_DROPZONE, hInst, NULL);
        SendMessageW(hDrop, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);
        g_dropZoneRect.left = 15; g_dropZoneRect.top = MOD_SECTION_TOP + 28;
        g_dropZoneRect.right = 15 + 620; g_dropZoneRect.bottom = MOD_SECTION_TOP + 28 + 70;
        TrackLauncherPageWnd(hDrop);

        /* small subtitle under the drop zone explaining how to use the
           GameBanana link-drop method (and that https:// must be included) */
        g_overlayLabels[3].text =
            L"Tip: For the link method to work, add a https:// in front of the "
            L"'gamebanana.com/mods' link. The downloader won't work without it. "
            L"Download links [https://gamebanana.com/dl/######] also work.";
        g_overlayLabels[3].rect = (RECT){ g_dropZoneRect.left, g_dropZoneRect.bottom + 4,
                                           g_dropZoneRect.left + 620, g_dropZoneRect.bottom + 4 + 32 };
        g_overlayLabels[3].font = g_fontSmall;
        g_overlayLabels[3].visible = TRUE;

        /* Browse Mods tab: a single scrollable list window covering the
           same footprint the Launcher tab's content uses, so switching
           tabs is just show/hide of two mutually-exclusive halves */
        {
            RECT crc; GetClientRect(hwnd, &crc);
            int listH = crc.bottom - 15 - CONTENT_TOP;
            g_hModListWnd = CreateModListWnd(hwnd, hInst, 15, CONTENT_TOP, 620, listH);
        }

        /* animated backdrop: tinted image + slow pulsing/drifting stars */
        g_hBgLightBitmap = LoadBitmapW(hInst, MAKEINTRESOURCEW(IDB_BGLIGHT));
        g_hBgDarkBitmap  = LoadBitmapW(hInst, MAKEINTRESOURCEW(IDB_BGDARK));
        {
            RECT crc; GetClientRect(hwnd, &crc);
            InitStars(crc.right, crc.bottom);
        }
        SetTimer(hwnd, ANIM_TIMER_ID, ANIM_INTERVAL_MS, NULL);

        ApplyThemeColors(hwnd);

        g_pDropTarget = (DropTargetImpl*)malloc(sizeof(DropTargetImpl));
        if (g_pDropTarget) {
            g_pDropTarget->base.lpVtbl = &g_dropTargetVtbl;
            g_pDropTarget->refCount = 1;
            g_pDropTarget->hwnd = hwnd;
            RegisterDragDrop(hwnd, (IDropTarget*)g_pDropTarget);
        }
        return 0;
    }

    case WM_TIMER: {
        if (wParam == ANIM_TIMER_ID) {
            RECT crc; GetClientRect(hwnd, &crc);
            UpdateStars(crc.right, crc.bottom);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hwnd, &rc);
        PaintAnimatedBackdrop(hdc, rc.right, rc.bottom);
        return 1;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
        if (dis->CtlType == ODT_BUTTON) {
            BOOL pressed = (dis->itemState & ODS_SELECTED) != 0;
            BOOL disabled = (dis->itemState & ODS_DISABLED) != 0;
            BOOL hovered = GetPropW(dis->hwndItem, L"CVHover") != NULL;

            /* the two tab buttons show which tab is active by staying in
               the "pressed" fill permanently, like a selected tab */
            if ((dis->CtlID == ID_TAB_LAUNCHER && g_activeTab == TAB_LAUNCHER) ||
                (dis->CtlID == ID_TAB_BROWSE && g_activeTab == TAB_BROWSE)) {
                pressed = TRUE;
            }

            COLORREF fillNormal = g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL;
            COLORREF fillHover = g_darkMode ? RGB(58,58,64) : RGB(228,230,235);
            COLORREF fillPressed = g_darkMode ? COLOR_DARK_PRESSED : COLOR_LIGHT_PRESSED;
            COLORREF border = g_darkMode ? COLOR_DARK_BORDER : COLOR_LIGHT_BORDER;
            COLORREF textColor = disabled
                ? (g_darkMode ? RGB(110,110,110) : RGB(160,160,160))
                : (g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT);

            COLORREF fillColor = pressed ? fillPressed : (hovered ? fillHover : fillNormal);
            HBRUSH hFill = CreateSolidBrush(fillColor);
            FillRect(dis->hDC, &dis->rcItem, hFill);
            DeleteObject(hFill);

            HPEN hPen = CreatePen(PS_SOLID, 1, border);
            HGDIOBJ oldPen = SelectObject(dis->hDC, hPen);
            HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
            SelectObject(dis->hDC, oldBrush);
            SelectObject(dis->hDC, oldPen);
            DeleteObject(hPen);

            wchar_t buf[128];
            GetWindowTextW(dis->hwndItem, buf, 128);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, textColor);
            HGDIOBJ oldFont = SelectObject(dis->hDC, g_fontRegular);
            RECT textRect = dis->rcItem;
            DrawTextW(dis->hDC, buf, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dis->hDC, oldFont);

            if (dis->itemState & ODS_FOCUS) {
                RECT focusRect = dis->rcItem;
                InflateRect(&focusRect, -3, -3);
                DrawFocusRect(dis->hDC, &focusRect);
            }
            return TRUE;
        }
        return FALSE;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT);
        return (LRESULT)(g_hBrushPanel ? g_hBrushPanel : GetSysColorBrush(COLOR_BTNFACE));
    }

    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT);
        return (LRESULT)(g_hBrushPanel ? g_hBrushPanel : GetSysColorBrush(COLOR_BTNFACE));
    }

    case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT);
        return (LRESULT)(g_hBrushPanel ? g_hBrushPanel : GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_COMMAND: {
        if (HIWORD(wParam) == BN_CLICKED) {
            switch (LOWORD(wParam)) {
            case ID_LAUNCH_NORMAL: LaunchSelectedVersion(hwnd, g_hNormalCombo, &g_normalVersions, TRUE); break;
            case ID_LAUNCH_COMPAT: LaunchSelectedVersion(hwnd, g_hCompatCombo, &g_compatVersions, FALSE); break;
            case ID_MODDING:       OpenModdingFolder(hwnd); break;
            case ID_GET_MODS:      OpenGetMods(hwnd); break;
            case ID_README_INFO:   ShowFirstRunPopup(hwnd, g_hInstance); break;
            case ID_TAB_LAUNCHER:  SwitchTab(hwnd, TAB_LAUNCHER); break;
            case ID_TAB_BROWSE:    SwitchTab(hwnd, TAB_BROWSE); break;
            case IDC_DARKMODE_CHECK:
                g_darkMode = !g_darkMode;
                SetWindowTextW(g_hDarkModeCheck, g_darkMode ? L"\u2611 Dark Mode" : L"\u2610 Dark Mode");
                SaveSettings();
                ApplyThemeColors(hwnd);
                break;
            }
        } else if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_PACK_COMBO) {
            /* remember the chosen pack folder immediately, so it's
               persisted no matter how the app ends up being closed */
            int sel = (int)SendMessageW(g_hPackCombo, CB_GETCURSEL, 0, 0);
            if (sel != CB_ERR) {
                SendMessageW(g_hPackCombo, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)g_lastPackFolder);
                SaveSettings();
            }
        }
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, ANIM_TIMER_ID);
        if (g_pDropTarget) {
            RevokeDragDrop(hwnd);
            ((IDropTarget*)g_pDropTarget)->lpVtbl->Release((IDropTarget*)g_pDropTarget);
            g_pDropTarget = NULL;
        }
        VersionList_Free(&g_normalVersions);
        VersionList_Free(&g_compatVersions);
        if (g_hBitmap) DeleteObject(g_hBitmap);
        if (g_hBgLightBitmap) DeleteObject(g_hBgLightBitmap);
        if (g_hBgDarkBitmap) DeleteObject(g_hBgDarkBitmap);
        if (g_hMemBitmap) DeleteObject(g_hMemBitmap);
        if (g_hMemDC) DeleteDC(g_hMemDC);
        if (g_hBrushBg) DeleteObject(g_hBrushBg);
        if (g_hBrushPanel) DeleteObject(g_hBrushPanel);
        if (g_fontTitle) DeleteObject(g_fontTitle);
        if (g_fontRegular) DeleteObject(g_fontRegular);
        if (g_fontSmall) DeleteObject(g_fontSmall);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ===================== first-run "read the README" popup ===================== */

static HWND g_hFirstRunOwner = NULL;
static HWND g_hFirstRunPopup = NULL;

static void CloseFirstRunPopup(HWND hwnd)
{
    g_readmeShown = TRUE;
    SaveSettings();
    if (g_hFirstRunOwner) {
        EnableWindow(g_hFirstRunOwner, TRUE);
        SetForegroundWindow(g_hFirstRunOwner);
    }
    DestroyWindow(hwnd);
    g_hFirstRunPopup = NULL;
}

static LRESULT CALLBACK FirstRunWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
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
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT);
        return (LRESULT)(g_hBrushBg ? g_hBrushBg : GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_README_OPEN:
            ShellExecuteW(hwnd, L"open", README_URL, NULL, NULL, SW_SHOWNORMAL);
            CloseFirstRunPopup(hwnd);
            return 0;
        case ID_README_CLOSE:
            CloseFirstRunPopup(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        CloseFirstRunPopup(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* Shown once, on the very first launch (tracked via ReadmeShown= in
   launcher_settings.ini) - points the person at the GitHub README so they
   know where their game files/versions need to go before they go looking
   for that folder layout themselves. */
static void ShowFirstRunPopup(HWND hOwner, HINSTANCE hInst)
{
    static BOOL classRegistered = FALSE;
    const wchar_t CLASS_NAME[] = L"CVLauncherFirstRunWnd";
    if (!classRegistered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc   = FirstRunWndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = CLASS_NAME;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
        RegisterClassW(&wc);
        classRegistered = TRUE;
    }

    const int w = 380, h = 220;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT rc = {0, 0, w, h};
    AdjustWindowRect(&rc, style, FALSE);
    int winW = rc.right - rc.left, winH = rc.bottom - rc.top;

    RECT prc; GetWindowRect(hOwner, &prc);
    int posX = prc.left + ((prc.right - prc.left) - winW) / 2;
    int posY = prc.top + ((prc.bottom - prc.top) - winH) / 2;

    g_hFirstRunOwner = hOwner;

    HWND hPop = CreateWindowExW(WS_EX_DLGMODALFRAME, CLASS_NAME,
        L"Welcome to The Choicer Voicer Launcher",
        style, posX, posY, winW, winH, hOwner, NULL, hInst, NULL);
    if (!hPop) return;
    g_hFirstRunPopup = hPop;

    ApplyDarkTitlebar(hPop, g_darkMode);

    HWND hMsg = CreateWindowW(L"STATIC",
        L"Before dropping in your game files, please read the README on GitHub - "
        L"it explains exactly where your game/version folders need to go so the "
        L"launcher can find them.",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 20, w - 40, 100, hPop, NULL, hInst, NULL);
    SendMessageW(hMsg, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);

    HWND hOpenBtn = CreateWindowW(L"BUTTON", L"Open README",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
        20, h - 60, 160, 32, hPop, (HMENU)(INT_PTR)ID_README_OPEN, hInst, NULL);
    SendMessageW(hOpenBtn, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);

    HWND hCloseBtn = CreateWindowW(L"BUTTON", L"Close",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        w - 20 - 100, h - 60, 100, 32, hPop, (HMENU)(INT_PTR)ID_README_CLOSE, hInst, NULL);
    SendMessageW(hCloseBtn, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);

    EnableWindow(hOwner, FALSE);
    ShowWindow(hPop, SW_SHOW);
    UpdateWindow(hPop);
    SetForegroundWindow(hPop);
    SetFocus(hOpenBtn);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                     PWSTR pCmdLine, int nCmdShow)
{
    (void)hPrevInstance; (void)pCmdLine;

    OleInitialize(NULL);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    const wchar_t CLASS_NAME[] = L"ChoicerVoicerLauncherWnd";

    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL; /* WM_ERASEBKGND paints the background so dark mode can switch it live */
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    RegisterClassW(&wc);

    RECT rc = {0, 0, 650, 610};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    AdjustWindowRect(&rc, style, FALSE);

    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - winW) / 2;
    int posY = (screenH - winH) / 2;

    HWND hwnd = CreateWindowExW(0, CLASS_NAME, L"The Choicer Voicer - Launcher",
        style, posX, posY, winW, winH,
        NULL, NULL, hInstance, NULL);

    if (!hwnd) { OleUninitialize(); return 0; }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    /* one-time "read the README" popup, shown only if it's never been
       shown before (tracked in launcher_settings.ini) */
    if (!g_readmeShown) {
        ShowFirstRunPopup(hwnd, hInstance);
    }

    MSG msg = {0};
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    OleUninitialize();
    return 0;
}
