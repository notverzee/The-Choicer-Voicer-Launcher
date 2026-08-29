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
#include <wchar.h>
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

#define IDB_SIDEIMAGE   100
#define IDI_APPICON     101
#define IDB_BGLIGHT     102
#define IDB_BGDARK      103

/* ---- folders (relative to the launcher's own exe) that hold versions ---- */
static const wchar_t* NORMAL_DIR_NAME = L"Normal";
static const wchar_t* COMPAT_DIR_NAME = L"Compatibility";

static const wchar_t* GAME_DIR_SUBPATH = L"\\YeahMaybe\\ChoicerVoicer\\game";
static const wchar_t* GAMEBANANA_URL = L"https://gamebanana.com/games/20674";
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
        }
    }
    free(text);
}

static void SaveSettings(void)
{
    wchar_t path[MAX_PATH];
    GetSettingsPath(path);

    wchar_t content[3 * MAX_PATH + 64];
    wsprintfW(content, L"NormalLastExe=%s\r\nCompatLastExe=%s\r\nDarkMode=%d\r\nReadmeShown=%d\r\n",
        g_lastNormalExe, g_lastCompatExe, g_darkMode ? 1 : 0, g_readmeShown ? 1 : 0);

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
        if (!lbl->text) continue;
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

/* ===================== local file drop entry point ===================== */

static void ProcessDroppedFile(HWND hwnd, const wchar_t* filePath)
{
    size_t len = wcslen(filePath);
    BOOL isZip = (len > 4 && _wcsicmp(filePath + len - 4, L".zip") == 0);
    BOOL isRar = (len > 4 && _wcsicmp(filePath + len - 4, L".rar") == 0);

    if (!isZip && !isRar) {
        MessageBoxW(hwnd, L"Only .zip and .rar files are supported.",
            L"The Choicer Voicer - Launcher", MB_ICONWARNING | MB_OK);
        return;
    }

    wchar_t gameDir[MAX_PATH];
    if (!GetAppDataGameDir(gameDir) || !PathIsDirW(gameDir)) {
        wchar_t msg[512];
        wsprintfW(msg,
            L"The game's data folder doesn't exist yet:\n%s\n\n"
            L"Run the game at least once first.", gameDir);
        MessageBoxW(hwnd, msg, L"The Choicer Voicer - Launcher", MB_ICONINFORMATION | MB_OK);
        return;
    }

    int sel = (int)SendMessageW(g_hPackCombo, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= NUM_PACK_FOLDERS) sel = 0;
    const wchar_t* packName = PACK_FOLDERS[sel];

    wchar_t destPackDir[MAX_PATH];
    wsprintfW(destPackDir, L"%s\\%s", gameDir, packName);
    EnsureDirW(destPackDir);

    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    wchar_t stagingDir[MAX_PATH];
    static LONG s_stagingCounter = 0;
    wsprintfW(stagingDir, L"%scvlauncher_staging_%lu_%ld",
        tempDir, GetTickCount(), InterlockedIncrement(&s_stagingCounter));
    EnsureDirW(stagingDir);

    wchar_t errMsg[512] = L"";
    BOOL extractOk = isZip
        ? ExtractZipToDirW(filePath, stagingDir, errMsg)
        : ExtractRarToDirW(filePath, stagingDir, errMsg);

    if (!extractOk) {
        MessageBoxW(hwnd, errMsg[0] ? errMsg : L"Extraction failed.",
            L"The Choicer Voicer - Launcher", MB_ICONERROR | MB_OK);
        DeleteDirectoryRecursiveW(stagingDir);
        return;
    }

    wchar_t archiveBase[MAX_PATH];
    GetArchiveBaseName(filePath, archiveBase);

    wchar_t finalPath[MAX_PATH];
    BOOL finalizeOk = FinalizeIntoPackFolder(stagingDir, destPackDir, archiveBase, finalPath);

    DeleteDirectoryRecursiveW(stagingDir);

    if (!finalizeOk) {
        MessageBoxW(hwnd, L"Extracted, but couldn't move the mod into place.",
            L"The Choicer Voicer - Launcher", MB_ICONERROR | MB_OK);
        return;
    }

    wchar_t msg[600];
    wsprintfW(msg, L"Mod installed to:\n%s", finalPath);
    MessageBoxW(hwnd, msg, L"The Choicer Voicer - Launcher", MB_ICONINFORMATION | MB_OK);
}

/* ===================== HTTP download (WinINet) ===================== */

static BYTE* HttpGetToMemory(const wchar_t* url, DWORD* outSize, wchar_t* errMsg)
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

/* ===================== dropped-URL entry point (GameBanana link or direct file link) ===================== */

static void ProcessDroppedURL(HWND hwnd, const wchar_t* url)
{
    wchar_t downloadUrl[2048];
    wchar_t itemType[32], itemId[32];

    if (IsGameBananaModPageUrl(url, itemType, itemId)) {
        wchar_t apiUrl[1024];
        wsprintfW(apiUrl,
            L"https://api.gamebanana.com/Core/Item/Data?itemtype=%s&itemid=%s&fields=Files().aFiles()&format=json_min",
            itemType, itemId);

        wchar_t errMsg[512] = L"";
        DWORD apiSize = 0;
        BYTE* apiData = HttpGetToMemory(apiUrl, &apiSize, errMsg);
        if (!apiData) {
            MessageBoxW(hwnd, errMsg[0] ? errMsg : L"Couldn't reach GameBanana's API.",
                L"The Choicer Voicer - Launcher", MB_ICONERROR | MB_OK);
            return;
        }

        char* jsonText = (char*)malloc((size_t)apiSize + 1);
        if (!jsonText) { free(apiData); return; }
        memcpy(jsonText, apiData, apiSize);
        jsonText[apiSize] = 0;
        free(apiData);

        wchar_t fileId[64];
        BOOL found = ExtractFirstJsonNumericKey(jsonText, fileId);
        free(jsonText);

        if (!found) {
            MessageBoxW(hwnd, L"Couldn't find a downloadable file on that GameBanana page.",
                L"The Choicer Voicer - Launcher", MB_ICONWARNING | MB_OK);
            return;
        }

        wsprintfW(downloadUrl, L"https://gamebanana.com/dl/%s", fileId);
    } else {
        wcsncpy(downloadUrl, url, 2047);
        downloadUrl[2047] = 0;
    }

    wchar_t errMsg2[512] = L"";
    DWORD fileSize = 0;
    BYTE* fileData = HttpGetToMemory(downloadUrl, &fileSize, errMsg2);
    if (!fileData) {
        MessageBoxW(hwnd, errMsg2[0] ? errMsg2 : L"Download failed.",
            L"The Choicer Voicer - Launcher", MB_ICONERROR | MB_OK);
        return;
    }

    int archiveType = SniffArchiveType(fileData, fileSize);
    if (archiveType == 0) {
        free(fileData);
        MessageBoxW(hwnd, L"The downloaded file doesn't look like a .zip or .rar archive.",
            L"The Choicer Voicer - Launcher", MB_ICONWARNING | MB_OK);
        return;
    }

    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    wchar_t tempFile[MAX_PATH];
    wsprintfW(tempFile, L"%scvlauncher_dl_%lu.%s", tempDir, GetTickCount(),
        archiveType == 1 ? L"zip" : L"rar");

    HANDLE hOut = CreateFileW(tempFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOut == INVALID_HANDLE_VALUE) {
        free(fileData);
        MessageBoxW(hwnd, L"Couldn't save the downloaded file.",
            L"The Choicer Voicer - Launcher", MB_ICONERROR | MB_OK);
        return;
    }
    DWORD written = 0;
    WriteFile(hOut, fileData, fileSize, &written, NULL);
    CloseHandle(hOut);
    free(fileData);

    ProcessDroppedFile(hwnd, tempFile);
    DeleteFileW(tempFile);
}

/* ===================== OLE drag & drop (files AND dragged links) ===================== */

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
                    ProcessDroppedFile(self->hwnd, path);
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
                ProcessDroppedURL(self->hwnd, url);
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
    if (g_hDarkModeCheck) SetWindowTheme(g_hDarkModeCheck, comboTheme, NULL);

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

    return panel;
}

static HWND MakeCheckbox(HWND parent, HINSTANCE hInst, const wchar_t* text,
                          int x, int y, int w, int ht, int id, HFONT font)
{
    HWND ctl = CreateWindowW(L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        x, y, w, ht, parent, (HMENU)(INT_PTR)id, hInst, NULL);
    if (font) SendMessageW(ctl, WM_SETFONT, (WPARAM)font, TRUE);
    return ctl;
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

        /* ===== header bar: Mod Folder / Get Mods / Info on the left,
           Dark Mode on the far right ===== */
        #define HEADER_TOP 15
        #define HEADER_H   36
        #define HEADER_BOTTOM (HEADER_TOP + HEADER_H)
        #define CONTENT_TOP (HEADER_BOTTOM + 15)

        MakeButton(hwnd, hInst, L"Mod Folder", 15, HEADER_TOP, 140, HEADER_H, ID_MODDING, g_fontRegular);
        MakeButton(hwnd, hInst, L"Get Mods", 165, HEADER_TOP, 140, HEADER_H, ID_GET_MODS, g_fontRegular);
        MakeButton(hwnd, hInst, L"\u24D8", 315, HEADER_TOP, HEADER_H, HEADER_H, ID_README_INFO, g_fontRegular);

        g_hDarkModeCheck = MakeCheckbox(hwnd, hInst, L"Dark Mode", 635 - 110, HEADER_TOP + (HEADER_H - 22) / 2,
            110, 22, IDC_DARKMODE_CHECK, g_fontRegular);
        SendMessageW(g_hDarkModeCheck, BM_SETCHECK, g_darkMode ? BST_CHECKED : BST_UNCHECKED, 0);

        /* side image */
        g_hBitmap = LoadBitmapW(hInst, MAKEINTRESOURCEW(IDB_SIDEIMAGE));
        HWND hImg = CreateWindowW(L"STATIC", NULL,
            WS_CHILD | WS_VISIBLE | SS_BITMAP,
            15, CONTENT_TOP, 250, 333, hwnd, NULL, hInst, NULL);
        SendMessageW(hImg, STM_SETIMAGE, (WPARAM)IMAGE_BITMAP, (LPARAM)g_hBitmap);

        int rx = 285;   /* right column x */

        g_overlayLabels[0].text = L"The Choicer Voicer";
        g_overlayLabels[0].rect = (RECT){ rx, CONTENT_TOP + 5, rx + 230, CONTENT_TOP + 5 + 34 };
        g_overlayLabels[0].font = g_fontTitle;

        g_overlayLabels[1].text = L"Launcher; Choose a version from the dropdown windows to play the version you want.";
        g_overlayLabels[1].rect = (RECT){ rx, CONTENT_TOP + 43, rx + 350, CONTENT_TOP + 43 + 54 };
        g_overlayLabels[1].font = g_fontRegular;

        /* Normal Mode card: bordered panel using more of the width/height
           alongside the side image, instead of a thin single-height row */
        MakeGroupBox(hwnd, hInst, L"Normal Mode", rx, CONTENT_TOP + 117, 350, 98, g_fontRegular);
        g_hNormalCombo = MakeCombo(hwnd, hInst, rx + 15, CONTENT_TOP + 151, 215, 200, IDC_NORMAL_COMBO, g_fontRegular);
        g_hLaunchNormalBtn = MakeButton(hwnd, hInst, L"Launch", rx + 240, CONTENT_TOP + 151, 95, 30, ID_LAUNCH_NORMAL, g_fontRegular);

        /* Compatibility Mode card */
        MakeGroupBox(hwnd, hInst, L"Compatibility Mode", rx, CONTENT_TOP + 229, 350, 98, g_fontRegular);
        g_hCompatCombo = MakeCombo(hwnd, hInst, rx + 15, CONTENT_TOP + 263, 215, 200, IDC_COMPAT_COMBO, g_fontRegular);
        g_hLaunchCompatBtn = MakeButton(hwnd, hInst, L"Launch", rx + 240, CONTENT_TOP + 263, 95, 30, ID_LAUNCH_COMPAT, g_fontRegular);

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
        g_hPackCombo = MakeCombo(hwnd, hInst, 210, MOD_SECTION_TOP - 4, 200, 200, IDC_PACK_COMBO, g_fontRegular);
        for (int i = 0; i < NUM_PACK_FOLDERS; i++) {
            SendMessageW(g_hPackCombo, CB_ADDSTRING, 0, (LPARAM)PACK_FOLDERS[i]);
        }
        SendMessageW(g_hPackCombo, CB_SETCURSEL, 0, 0);

        HWND hDrop = CreateWindowW(L"STATIC",
            L"Drag a .zip/.rar file or a GameBanana mod link here",
            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE | WS_BORDER,
            15, MOD_SECTION_TOP + 28, 620, 70, hwnd, (HMENU)(INT_PTR)IDC_DROPZONE, hInst, NULL);
        SendMessageW(hDrop, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);
        g_dropZoneRect.left = 15; g_dropZoneRect.top = MOD_SECTION_TOP + 28;
        g_dropZoneRect.right = 15 + 620; g_dropZoneRect.bottom = MOD_SECTION_TOP + 28 + 70;

        /* small subtitle under the drop zone explaining that GameBanana
           wants the actual download link, not the mod page link */
        g_overlayLabels[3].text = L"Tip: drag the mod's actual DOWNLOAD link here, not the mod page link. "
            L"Open the mod page on GameBanana, click Download, then drag that link here.";
        g_overlayLabels[3].rect = (RECT){ g_dropZoneRect.left, g_dropZoneRect.bottom + 4,
                                           g_dropZoneRect.left + 620, g_dropZoneRect.bottom + 4 + 32 };
        g_overlayLabels[3].font = g_fontSmall;

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
            case IDC_DARKMODE_CHECK:
                g_darkMode = (SendMessageW(g_hDarkModeCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
                SaveSettings();
                ApplyThemeColors(hwnd);
                break;
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

    RECT rc = {0, 0, 650, 565};
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
