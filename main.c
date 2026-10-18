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
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <windowsx.h>
#include <commdlg.h>
#include <wchar.h>
#include <wctype.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#include "miniz.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "msimg32.lib")

/* ---- control / resource IDs ---- */
#define ID_LAUNCH_NORMAL   1001
#define ID_LAUNCH_COMPAT   1002
#define ID_MODDING          1005
#define IDC_DARKMODE_CHECK  1007

#define IDC_NORMAL_COMBO  3001
#define IDC_COMPAT_COMBO  3002

#define ID_README_OPEN    4001
#define ID_README_CLOSE   4002
#define ID_README_INFO    4003

#define ID_TAB_LAUNCHER    5001
#define ID_TAB_BROWSE      5002
#define ID_TAB_INSTALLED   5003
#define ID_TAB_DUB         5004

#define ID_ADD_NORMAL_VERSION 7010
#define ID_ADD_COMPAT_VERSION 7011
#define ID_OPEN_BROWSE_FROM_LAUNCHER 7012

#define IDB_SIDEIMAGE   100
#define IDI_APPICON     101
#define IDB_BGLIGHT     102
#define IDB_BGDARK      103

#define IDP_HOME_BLACK    110
#define IDP_HOME_WHITE    111
#define IDP_BROWSE_BLACK  112
#define IDP_BROWSE_WHITE  113
#define IDP_MODS_BLACK    114
#define IDP_MODS_WHITE    115
#define IDP_THEME_BLACK   116
#define IDP_THEME_WHITE   117
#define IDP_FOLDER_BLACK  118
#define IDP_FOLDER_WHITE  119
#define IDP_INFO_BLACK    120
#define IDP_INFO_WHITE    121
#define IDP_LOGO          122

/* ---- folders (relative to the launcher's own exe) that hold versions ---- */
static const wchar_t* NORMAL_DIR_NAME = L"Normal";
static const wchar_t* COMPAT_DIR_NAME = L"Compatibility";

static const wchar_t* GAME_DIR_SUBPATH = L"\\YeahMaybe\\ChoicerVoicer\\game";
/* must stay in sync with the game's numeric ID used throughout */
static const long GAMEBANANA_GAME_ID = 20674;
/* "Dub Mode" root category - confirmed via two real captured requests (both
   sort variants) where every single returned record, 30 for 30, had
   _aGame._idRow == GAMEBANANA_GAME_ID. IMPORTANT: this is only ONE of the
   game's categories, so browsing with it alone hid every mod outside Dub
   Mode (they only showed up through search). The browse fetch now asks for
   the whole game first (_aFilters[Generic_Game]) and only falls back to
   this category if the server turns out to ignore that filter - see
   g_gameFilterMode below. Util/Search/Results uses _idGameRow directly. */
static const long GAMEBANANA_CATEGORY_ID = 44064;
static const wchar_t* README_URL =
    L"https://github.com/notverzee/The-Choicer-Voicer-Launcher/blob/main/README.md";

static const wchar_t* PACK_FOLDERS[] = {
    L"packs_chatter", L"packs_host", L"packs_judges",
    L"packs_menu", L"packs_player", L"packs_studio", L"packs_voice"
};
#define NUM_PACK_FOLDERS 7

/* ---- colour themes ----
   Every theme has a dark and a light variant. g_themeIdx picks the colour (0 = the original
   neutral look, then purple / red / green / blue) and the Dark mode switch picks dark or light, so
   "dark purple" and "light purple" are the same choice in two modes. The COLOR_* names below are
   kept (they now read the active palette) so all the existing drawing code follows the theme. */
typedef struct {
    COLORREF bg, panel, pressed, border, text;     /* surfaces and text */
    COLORREF accent;                               /* active rings, sliders, links */
    COLORREF tint; int tintAlpha;                  /* laid over the backdrop picture (0 = none) */
} ThemePal;
#define NUM_THEMES 5
static const ThemePal g_darkPals[NUM_THEMES] = {
    /* default */ { RGB(32,32,32),  RGB(45,45,48),  RGB(60,60,64),  RGB(75,75,75),   RGB(230,230,230), RGB(100,170,255), RGB(0,0,0),     0   },
    /* purple  */ { RGB(26,20,36),  RGB(38,30,52),  RGB(54,44,74),  RGB(84,68,112),  RGB(236,230,246), RGB(178,136,255), RGB(70,28,128),  125 },
    /* red     */ { RGB(34,18,20),  RGB(48,28,30),  RGB(68,40,42),  RGB(112,64,66),  RGB(246,232,232), RGB(255,112,112), RGB(130,24,30),  125 },
    /* green   */ { RGB(18,30,22),  RGB(28,44,32),  RGB(40,62,46),  RGB(62,104,72),  RGB(230,246,234), RGB(110,215,140), RGB(24,100,48),  125 },
    /* blue    */ { RGB(18,24,38),  RGB(28,36,56),  RGB(40,52,80),  RGB(62,84,128),  RGB(230,238,250), RGB(100,170,255), RGB(24,64,140),  115 },
};
static const ThemePal g_lightPals[NUM_THEMES] = {
    /* default */ { RGB(240,240,240), RGB(255,255,255), RGB(222,222,222), RGB(170,170,170), RGB(82,82,90),  RGB(30,100,210), RGB(0,0,0),       0   },
    /* purple  */ { RGB(242,236,250), RGB(252,249,255), RGB(224,214,240), RGB(190,174,214), RGB(86,74,104), RGB(112,64,196), RGB(205,175,245), 120 },
    /* red     */ { RGB(250,238,238), RGB(255,250,250), RGB(240,218,218), RGB(214,174,174), RGB(104,72,72), RGB(200,52,52),  RGB(250,175,175), 120 },
    /* green   */ { RGB(236,248,238), RGB(248,255,250), RGB(214,236,218), RGB(166,206,174), RGB(64,92,70),  RGB(30,130,62),  RGB(165,228,180), 120 },
    /* blue    */ { RGB(234,242,252), RGB(248,251,255), RGB(212,226,244), RGB(164,190,224), RGB(60,82,110), RGB(30,100,200), RGB(165,205,252), 120 },
};
static const wchar_t* const g_themeNames[NUM_THEMES] = { L"Default", L"Purple", L"Red", L"Green", L"Blue" };
static int g_themeIdx = 0;
#define COLOR_DARK_BG       (g_darkPals[g_themeIdx].bg)
#define COLOR_DARK_PANEL    (g_darkPals[g_themeIdx].panel)
#define COLOR_DARK_PRESSED  (g_darkPals[g_themeIdx].pressed)
#define COLOR_DARK_TEXT     (g_darkPals[g_themeIdx].text)
#define COLOR_DARK_BORDER   (g_darkPals[g_themeIdx].border)
#define COLOR_LIGHT_BG      (g_lightPals[g_themeIdx].bg)
#define COLOR_LIGHT_PANEL   (g_lightPals[g_themeIdx].panel)
#define COLOR_LIGHT_PRESSED (g_lightPals[g_themeIdx].pressed)
#define COLOR_LIGHT_TEXT    (g_lightPals[g_themeIdx].text)
#define COLOR_LIGHT_BORDER  (g_lightPals[g_themeIdx].border)

static HFONT g_fontTitle    = NULL;
static HFONT g_fontRegular  = NULL;
static HFONT g_fontSmall    = NULL;
static HFONT g_fontIcon     = NULL;
#define IDR_UIFONT 130
static HFONT g_fontGroupHeader = NULL;
static HFONT g_fontSymbol   = NULL;   /* Segoe UI Symbol, for glyphs Anton doesn't have (checkbox squares, arrows) */
static wchar_t g_uiFace[LF_FACESIZE] = L"Segoe UI";

/* UI font: Anton (SIL Open Font License - free to embed). The .ttf is
   compiled into the EXE as an RCDATA resource and registered privately for
   this process only, so nothing needs installing. Falls back to Segoe UI if
   loading somehow fails. */
static void PickUiFontFace(HINSTANCE hInst)
{
    wcscpy(g_uiFace, L"Segoe UI");
    HRSRC hr = FindResourceW(hInst, MAKEINTRESOURCEW(IDR_UIFONT), (LPCWSTR)RT_RCDATA);
    if (!hr) return;
    HGLOBAL hg = LoadResource(hInst, hr);
    DWORD sz = SizeofResource(hInst, hr);
    void* data = hg ? LockResource(hg) : NULL;
    if (!data || !sz) return;
    DWORD n = 0;
    if (AddFontMemResourceEx(data, sz, NULL, &n) && n > 0)
        wcscpy(g_uiFace, L"Anton");
}
static HBITMAP g_hBitmap    = NULL;
static HWND   g_hDarkModeCheck = NULL;
static RECT   g_dropZoneRect = {0, 0, 0, 0};

static HBRUSH g_hBrushBg  = NULL;
static HBRUSH g_hBrushPanel = NULL;
static BOOL   g_darkMode = FALSE;
static const ThemePal* Pal(void) { return g_darkMode ? &g_darkPals[g_themeIdx] : &g_lightPals[g_themeIdx]; }
#define THEME_ACCENT (Pal()->accent)
/* a faint divider line: between the page background and its border colour */
#define SEP_COLOR BlendColor(g_darkMode ? COLOR_DARK_BG : COLOR_LIGHT_BG, g_darkMode ? COLOR_DARK_BORDER : COLOR_LIGHT_BORDER, 0.55f)
static BOOL   g_readmeShown = FALSE;
static HINSTANCE g_hInstance = NULL;

/* ---- tabs: "Launcher" (the existing UI), "Browse Mods", and "Installed
   Mods" (this pack folder's mods, with size/sort/delete/move) ---- */
typedef enum { TAB_LAUNCHER = 0, TAB_BROWSE = 1, TAB_INSTALLED = 2, TAB_DUB = 3, TAB_SETTINGS = 4 } AppTab;
static AppTab g_activeTab = TAB_LAUNCHER;
static HWND   g_hTabLauncherBtn = NULL;
static HWND   g_hTabBrowseBtn = NULL;
static HWND   g_hTabInstalledBtn = NULL;
static HWND   g_hTabDubBtn = NULL;
/* Dub Renderer tab (see dubrender.inc) - declared here so ApplyThemeColors can reach them */
static HWND   g_hDubWnd = NULL;
static HWND   g_hDubCombo = NULL;
static HWND   g_hDubLog = NULL;
/* hover tooltips for every MakeButton() button - one shared tooltip window; the text for
   each button comes from ButtonTipText() (defined just before SwitchTab) */
static HWND   g_hTip = NULL;
static const wchar_t* ButtonTipText(int id);
static BOOL g_musicOn = FALSE;          /* background music on/off (saved in the settings file) */
static int  g_musicVolume = 60;         /* 0..100, set by the Settings slider (saved too) */
static HWND g_hSettingsWnd = NULL;      /* Settings tab (see settings.inc) */
static HWND g_hSetInfoEdit = NULL;
static HWND g_hTabSettingsBtn = NULL;
static HWND g_lastGroupTitle = NULL;    /* title label of the group box MakeGroupBox() just built */

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
#define NUM_STARS 100            /* the most stars the Settings slider can show */
#define DEFAULT_STAR_COUNT 40   /* how many there were before it became adjustable */
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
static int  g_starCount = DEFAULT_STAR_COUNT;   /* 0..NUM_STARS, set by the Settings slider (saved) */
static HBITMAP g_hBgLightBitmap = NULL;
static HBITMAP g_hBgDarkBitmap  = NULL;

/* custom button-icon art (see wiring near WM_CREATE): each pair is the
   same icon drawn for dark mode ("Black" - dark panel background) and
   light mode ("White" - light panel background), with the background
   already baked in, since these load as one image per button rather than
   the old hand-drawn glyph-on-a-drawn-circle/square approach */
static HBITMAP g_hIconHomeBlack = NULL, g_hIconHomeWhite = NULL;
static HBITMAP g_hIconBrowseBlack = NULL, g_hIconBrowseWhite = NULL;
static HBITMAP g_hIconModsBlack = NULL, g_hIconModsWhite = NULL;
static HBITMAP g_hIconThemeBlack = NULL, g_hIconThemeWhite = NULL;
static HBITMAP g_hIconFolderBlack = NULL, g_hIconFolderWhite = NULL;
static HBITMAP g_hIconInfoBlack = NULL, g_hIconInfoWhite = NULL;
static HBITMAP g_hLogoBitmap = NULL;
static RECT g_logoRect = {0, 0, 0, 0};
static BOOL g_logoVisible = FALSE;
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
static wchar_t g_lastPackFolder[MAX_PATH] = L"";

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

/* strips the gameDir prefix from an absolute path, leaving the part
   that's meaningful as a mod_meta.ini key or a display path (e.g.
   "packs_host\FemaleVoices\CoolMod") - used so metadata stays correctly
   attached to a mod wherever it's nested, not just at a pack's root */
static void GetPathRelativeToGameDir(const wchar_t* gameDir, const wchar_t* fullPath, wchar_t* out, int outCap)
{
    size_t gameDirLen = wcslen(gameDir);
    if (_wcsnicmp(fullPath, gameDir, gameDirLen) == 0 && fullPath[gameDirLen] == L'\\') {
        wcsncpy(out, fullPath + gameDirLen + 1, outCap - 1);
        out[outCap - 1] = 0;
    } else {
        wcsncpy(out, fullPath, outCap - 1);
        out[outCap - 1] = 0;
    }
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
    g_musicOn = FALSE;
    g_musicVolume = 60;
    g_themeIdx = 0;
    g_starCount = DEFAULT_STAR_COUNT;

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
        } else if (_wcsnicmp(lineStart, L"MusicOn=", 8) == 0) {
            g_musicOn = (lineStart[8] == L'1');
        } else if (_wcsnicmp(lineStart, L"MusicVolume=", 12) == 0) {
            int v = _wtoi(lineStart + 12);
            g_musicVolume = v < 0 ? 0 : (v > 100 ? 100 : v);
        } else if (_wcsnicmp(lineStart, L"Theme=", 6) == 0) {
            int v = _wtoi(lineStart + 6);
            g_themeIdx = (v >= 0 && v < NUM_THEMES) ? v : 0;
        } else if (_wcsnicmp(lineStart, L"Stars=", 6) == 0) {
            int v = _wtoi(lineStart + 6);
            g_starCount = v < 0 ? 0 : (v > NUM_STARS ? NUM_STARS : v);
        } else if (_wcsnicmp(lineStart, L"ReadmeShown=", 12) == 0) {
            g_readmeShown = (lineStart[12] == L'1');
        } else if (_wcsnicmp(lineStart, L"PackFolder=", 11) == 0) {
            wcsncpy(g_lastPackFolder, lineStart + 11, MAX_PATH - 1);
            g_lastPackFolder[MAX_PATH - 1] = 0;
        }
    }
    free(text);
}

static void SaveSettings(void)
{
    wchar_t path[MAX_PATH];
    GetSettingsPath(path);

    wchar_t content[4 * MAX_PATH + 128];
    wsprintfW(content, L"NormalLastExe=%s\r\nCompatLastExe=%s\r\nDarkMode=%d\r\nReadmeShown=%d\r\nPackFolder=%s\r\nMusicOn=%d\r\nMusicVolume=%d\r\nTheme=%d\r\nStars=%d\r\n",
        g_lastNormalExe, g_lastCompatExe, g_darkMode ? 1 : 0, g_readmeShown ? 1 : 0, g_lastPackFolder, g_musicOn ? 1 : 0, g_musicVolume, g_themeIdx, g_starCount);

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
#include "uikit.inc"

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

    /* The tinted backdrop only changes when the window size or the theme does, so it is
       scaled into a cache bitmap once and just copied every frame. (Re-stretching it with
       HALFTONE on every animation tick got very slow on a big window - that was a big
       part of the home page feeling choppy.) */
    if (!g_hBgCacheDC || g_bgCacheW != w || g_bgCacheH != h || g_bgCacheDark != g_darkMode) {
        if (g_hBgCacheDC) { DeleteDC(g_hBgCacheDC); g_hBgCacheDC = NULL; }
        if (g_hBgCacheBmp) { DeleteObject(g_hBgCacheBmp); g_hBgCacheBmp = NULL; }
        g_hBgCacheDC = CreateCompatibleDC(hdc);
        g_hBgCacheBmp = CreateCompatibleBitmap(hdc, w, h);
        SelectObject(g_hBgCacheDC, g_hBgCacheBmp);
        g_bgCacheW = w; g_bgCacheH = h; g_bgCacheDark = g_darkMode;

        HBITMAP bgBmp = g_darkMode ? g_hBgDarkBitmap : g_hBgLightBitmap;
        if (bgBmp) {
            BITMAP bm;
            GetObjectW(bgBmp, sizeof(bm), &bm);
            HDC bgDC = CreateCompatibleDC(hdc);
            HGDIOBJ oldBgBmp = SelectObject(bgDC, bgBmp);
            /* stretched to fully cover the client area; HALFTONE keeps a blurred backdrop smooth */
            SetStretchBltMode(g_hBgCacheDC, g_sizing ? COLORONCOLOR : HALFTONE);
            SetBrushOrgEx(g_hBgCacheDC, 0, 0, NULL);
            StretchBlt(g_hBgCacheDC, 0, 0, w, h, bgDC, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            SelectObject(bgDC, oldBgBmp);
            DeleteDC(bgDC);
        } else {
            RECT full = {0, 0, w, h};
            FillRect(g_hBgCacheDC, &full, g_hBrushBg);
        }
        /* colour themes: wash the backdrop picture with the theme's tint (one cheap constant-alpha blend) */
        if (Pal()->tintAlpha > 0) {
            HDC t = CreateCompatibleDC(hdc);
            HBITMAP tb = CreateCompatibleBitmap(hdc, 1, 1);
            HGDIOBJ otb = SelectObject(t, tb);
            SetPixel(t, 0, 0, Pal()->tint);
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)Pal()->tintAlpha, 0 };
            AlphaBlend(g_hBgCacheDC, 0, 0, w, h, t, 0, 0, 1, 1, bf);
            SelectObject(t, otb);
            DeleteObject(tb);
            DeleteDC(t);
        }
    }
    BitBlt(g_hMemDC, 0, 0, w, h, g_hBgCacheDC, 0, 0, SRCCOPY);

    COLORREF bgTint = g_darkMode ? COLOR_DARK_BG : COLOR_LIGHT_BG;
    COLORREF starCore = BlendColor(g_darkMode ? RGB(220,225,255) : RGB(130,142,182), Pal()->accent, g_themeIdx ? 0.30f : 0.0f);

    HGDIOBJ oldPen = SelectObject(g_hMemDC, GetStockObject(NULL_PEN));
    for (int i = 0; i < g_starCount; i++) {
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

    /* wordmark logo (drawn before the text captions so label[0]'s text
       fallback, if the logo failed to load, still ends up on top) */
    /* Launcher page: the three cards are smooth rounded panels painted right into the backdrop;
       other tabs get a thin rounded outline around their page */
    if (g_logoVisible) {
        COLORREF cardFill = g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL;
        COLORREF cardBorder = g_darkMode ? COLOR_DARK_BORDER : COLOR_LIGHT_BORDER;
        for (int i = 0; i < 2; i++) DrawCardAA(g_hMemDC, g_cardRect[i], 14, cardFill, cardBorder);
    } else {
        RECT o = g_contentRect;
        InflateRect(&o, 1, 1);
        DrawOutlineAA(g_hMemDC, o, 16, g_darkMode ? COLOR_DARK_BORDER : COLOR_LIGHT_BORDER);
    }

    if (g_logoVisible && g_hLogoBitmap) {
        HDC logoDC = CreateCompatibleDC(g_hMemDC);
        HGDIOBJ oldLogoBmp = SelectObject(logoDC, g_hLogoBitmap);
        BITMAP lbm; GetObjectW(g_hLogoBitmap, sizeof(lbm), &lbm);
        BLENDFUNCTION bf;
        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;
        AlphaBlend(g_hMemDC, g_logoRect.left, g_logoRect.top,
            g_logoRect.right - g_logoRect.left, g_logoRect.bottom - g_logoRect.top,
            logoDC, 0, 0, lbm.bmWidth, lbm.bmHeight, bf);
        SelectObject(logoDC, oldLogoBmp);
        DeleteDC(logoDC);
    }

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

    PaintTitleBar(g_hMemDC, w);

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

/* runs an already-located extractor exe against one archive; shared by
   both the RAR path (7-Zip or WinRAR, whichever FindRarExtractor found)
   and the 7z path (7-Zip only) below */
static BOOL RunExternalExtractor(const wchar_t* exePath, BOOL isSevenZip,
    const wchar_t* archivePath, const wchar_t* destDir, const wchar_t* toolLabel, wchar_t* errMsg)
{
    wchar_t cmdLine[2048];
    if (isSevenZip) {
        wsprintfW(cmdLine, L"\"%s\" x -y -o\"%s\" \"%s\"", exePath, destDir, archivePath);
    } else {
        wsprintfW(cmdLine, L"\"%s\" x -y \"%s\" \"%s\\\"", exePath, archivePath, destDir);
    }

    STARTUPINFOW si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));

    if (!CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        wsprintfW(errMsg, L"Couldn't start the %s extractor.", toolLabel);
        return FALSE;
    }

    WaitForSingleObject(pi.hProcess, 60000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0) {
        wsprintfW(errMsg, L"The %s extractor reported an error (code %lu) - the archive is likely corrupted.",
            toolLabel, exitCode);
        return FALSE;
    }
    return TRUE;
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
    return RunExternalExtractor(exePath, isSevenZip, rarPath, destDir, L"RAR", errMsg);
}

/* 7z extraction only ever goes through 7-Zip itself (7z.exe) - WinRAR's
   UnRAR.exe is RAR-only and isn't a reliable fallback for .7z */
static BOOL FindSevenZipExe(wchar_t* outExe)
{
    wchar_t pf[MAX_PATH] = {0}, pf86[MAX_PATH] = {0}, candidate[MAX_PATH];
    GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
    GetEnvironmentVariableW(L"ProgramFiles(x86)", pf86, MAX_PATH);

    if (pf[0]) {
        wsprintfW(candidate, L"%s\\7-Zip\\7z.exe", pf);
        if (PathExistsW(candidate)) { wcscpy(outExe, candidate); return TRUE; }
    }
    if (pf86[0]) {
        wsprintfW(candidate, L"%s\\7-Zip\\7z.exe", pf86);
        if (PathExistsW(candidate)) { wcscpy(outExe, candidate); return TRUE; }
    }
    return FALSE;
}

static BOOL Extract7zToDirW(const wchar_t* sevenZipPath, const wchar_t* destDir, wchar_t* errMsg)
{
    wchar_t exePath[MAX_PATH];
    if (!FindSevenZipExe(exePath)) {
        wsprintfW(errMsg,
            L"7z extraction needs 7-Zip installed on this PC.\n"
            L"Install 7-Zip (free, 7-zip.org) or re-zip the mod as .zip.");
        return FALSE;
    }
    return RunExternalExtractor(exePath, TRUE, sevenZipPath, destDir, L"7z", errMsg);
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
    if (size >= 6 && data[0]==0x37 && data[1]==0x7A && data[2]==0xBC && data[3]==0xAF &&
        data[4]==0x27 && data[5]==0x1C) return 3; /* 7z */
    return 0;
}

/* ===================== install pipeline: download/extract/install with
   a progress window, used by both the drop zone and the mod browser's
   Install button ===================== */

/* forward declaration - defined further down near WM_CREATE/theme
   handling, but needed here by the progress/destination-picker popups */
static void ApplyDarkTitlebar(HWND hwnd, BOOL dark);

/* forward declarations - real definitions live with the rest of the
   owner-draw button plumbing (ButtonSubclassProc/ApplyThemeColors), but
   every popup created before that point in the file (Add Version,
   Choose Destination, etc.) needs them so its buttons render themed for
   dark mode instead of falling back to plain OS push-buttons */
static HWND MakeButton(HWND parent, HINSTANCE hInst, const wchar_t* text,
                        int x, int y, int w, int ht, int id, HFONT font);
static void DrawThemedOwnerButton(LPDRAWITEMSTRUCT dis);

/* forward declarations - real definitions live with the rest of the
   Installed Mods tab's folder-tree infrastructure further down (they
   need InstalledFolderEntry/g_instFolders, which aren't declared yet at
   this point in the file), but the Choose Destination popup and the
   drag-and-drop handler both need to reach a chosen destination folder
   before that point */
static void ShowDestPicker(HWND hOwner, HINSTANCE hInst, const wchar_t* profileUrl);
static void GetDownloadDestFolder(wchar_t* out, int outCap);
static void ShowDownloadFromLinkPicker(HWND hOwner, HINSTANCE hInst);

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
    /* set when the failure happened at the "do something with the
       archive" stage (unrecognized type, or a real extractor reported
       an error) rather than earlier (download failed outright, no game
       folder, etc.) - in that case there's a real archive file sitting
       on disk that the user might still want, even though the launcher
       couldn't do anything useful with it itself */
    BOOL offerDownloadAnyway;
    wchar_t rawFilePath[MAX_PATH];   /* the archive file to offer */
    BOOL rawFileIsTemp;              /* TRUE: our own downloaded temp file (safe to delete once handled). FALSE: the user's own local file (never delete it) */
    wchar_t destPackDir[MAX_PATH];   /* where "Download Anyway" should copy it to */
} InstallDoneMsg;

typedef struct {
    HWND mainWnd;
    HWND progressWnd;
    BOOL fromUrl;
    wchar_t url[2048];
    wchar_t localFile[MAX_PATH];
    wchar_t destPackFolder[MAX_PATH]; /* a path relative to gameDir - a bare pack name ("packs_host") or a nested relative path ("packs_host\FemaleVoices") now that installs can target any existing folder */
    /* only set when this install was started from the Browse Mods list's
       Install button (see ModListDoInstall/ShowDestPicker) - carries the
       same name/author/coverphoto/description/link the list showed, so
       the Installed Mods tab can display the same card instead of
       falling back to the bare folder name. Left empty (metaTitle[0]==0)
       for drag-and-drop installs, which show local icon.png/info.ini
       instead. */
    wchar_t metaTitle[128];
    wchar_t metaAuthor[64];
    wchar_t metaThumbUrl[512];
    wchar_t metaDescription[400];
} InstallJobParam;

static HWND g_hInstallProgressWnd = NULL;
static HWND g_hInstallProgressBar = NULL;
static HWND g_hInstallProgressLabel = NULL;
static HWND g_hInstallProgressOwner = NULL;

/* forward declaration - full definition lives with the rest of the
   Installed Mods tab, near the end of the file, but InstallJobThreadProc
   below needs to record where a mod came from (link/coverphoto/
   description) as soon as an install succeeds */
static void SaveModMetaRecord(const wchar_t* pathKey, const wchar_t* title, const wchar_t* author,
    const wchar_t* thumbUrl, const wchar_t* profileUrl, const wchar_t* description);

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

    /* computed up front (not just in the "got a real archive" branch
       below) so every failure path - including "couldn't tell what kind
       of file this is" - still has a destination ready if it ends up
       offering "Download Anyway" */
    wchar_t gameDir[MAX_PATH] = L"";
    wchar_t destPackDir[MAX_PATH] = L"";
    BOOL haveDestPackDir = GetAppDataGameDir(gameDir) && PathIsDirW(gameDir);
    if (haveDestPackDir) {
        wsprintfW(destPackDir, L"%s\\%s", gameDir, job->destPackFolder);
        EnsureDirW(destPackDir);
    }

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
        const wchar_t* archiveExt = (archiveType == 1) ? L"zip" : (archiveType == 2) ? L"rar"
                                   : (archiveType == 3) ? L"7z" : L"bin";

        wchar_t tempDir[MAX_PATH];
        GetTempPathW(MAX_PATH, tempDir);
        wsprintfW(filePath, L"%scvlauncher_dl_%lu.%s", tempDir, GetTickCount(), archiveExt);
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

        if (archiveType == 0) {
            /* still worth keeping - it might just be a format we don't
               recognize, and the person can decide for themselves */
            done->success = FALSE;
            wcscpy(done->resultMsg, L"The downloaded file doesn't look like a .zip, .rar, or .7z archive.");
            if (haveDestPackDir) {
                done->offerDownloadAnyway = TRUE;
                wcscpy(done->rawFilePath, filePath);
                done->rawFileIsTemp = TRUE;
                wcscpy(done->destPackDir, destPackDir);
            }
            goto finish;
        }
    } else {
        wcsncpy(filePath, job->localFile, MAX_PATH - 1);
        filePath[MAX_PATH - 1] = 0;
    }

    {
        size_t len = wcslen(filePath);
        BOOL isZip = (len > 4 && _wcsicmp(filePath + len - 4, L".zip") == 0);
        BOOL isRar = (len > 4 && _wcsicmp(filePath + len - 4, L".rar") == 0);
        BOOL isSevenZip = (len > 3 && _wcsicmp(filePath + len - 3, L".7z") == 0);
        if (!isZip && !isRar && !isSevenZip) {
            done->success = FALSE;
            wcscpy(done->resultMsg, L"Only .zip, .rar, and .7z files are supported.");
            if (haveDestPackDir) {
                done->offerDownloadAnyway = TRUE;
                wcscpy(done->rawFilePath, filePath);
                done->rawFileIsTemp = ownsTempFile;
                wcscpy(done->destPackDir, destPackDir);
            }
            goto finish;
        }

        PostInstallProgress(progressWnd, L"Preparing\u2026", -1);
        if (!haveDestPackDir) {
            done->success = FALSE;
            wsprintfW(done->resultMsg,
                L"The game's data folder doesn't exist yet:\n%s\n\nRun the game at least once first.", gameDir);
            goto finish;
        }

        wchar_t tempDir2[MAX_PATH];
        GetTempPathW(MAX_PATH, tempDir2);
        wchar_t stagingDir[MAX_PATH];
        static LONG s_stagingCounter = 0;
        wsprintfW(stagingDir, L"%scvlauncher_staging_%lu_%ld",
            tempDir2, GetTickCount(), InterlockedIncrement(&s_stagingCounter));
        EnsureDirW(stagingDir);

        PostInstallProgress(progressWnd, L"Extracting\u2026", -1);
        wchar_t errMsg3[512] = L"";
        BOOL extractOk = isZip ? ExtractZipToDirW(filePath, stagingDir, errMsg3)
                       : isRar ? ExtractRarToDirW(filePath, stagingDir, errMsg3)
                               : Extract7zToDirW(filePath, stagingDir, errMsg3);

        if (!extractOk) {
            done->success = FALSE;
            wcsncpy(done->resultMsg, errMsg3[0] ? errMsg3 : L"Extraction failed.", 599);
            DeleteDirectoryRecursiveW(stagingDir);

            /* the archive itself is still sitting right here - whether
               it's genuinely corrupted or the extractor tool just isn't
               installed, the person may still want the raw file */
            done->offerDownloadAnyway = TRUE;
            wcscpy(done->rawFilePath, filePath);
            done->rawFileIsTemp = ownsTempFile;
            wcscpy(done->destPackDir, destPackDir);
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

        /* record where this came from (Browse Mods list only - see
           InstallJobParam) so the Installed Mods tab can show the same
           title/author/coverphoto/description/link instead of just the
           folder name */
        if (job->metaTitle[0]) {
            wchar_t folderName[160] = L"";
            const wchar_t* lastSlash = wcsrchr(finalPath, L'\\');
            wcsncpy(folderName, lastSlash ? lastSlash + 1 : finalPath, 159);
            folderName[159] = 0;
            wchar_t metaKey[256];
            wsprintfW(metaKey, L"%s\\%s", job->destPackFolder, folderName);
            SaveModMetaRecord(metaKey, job->metaTitle, job->metaAuthor, job->metaThumbUrl,
                job->url, job->metaDescription);
        }
    }

finish:
    /* when "Download Anyway" is being offered the raw file has to
       survive until the person decides - whoever handles that choice
       (see WM_APP_INSTALL_DONE) takes over responsibility for cleaning
       up a temp file afterward */
    if (ownsTempFile && filePath[0] && !done->offerDownloadAnyway) DeleteFileW(filePath);
    free(job);
    if (IsWindow(progressWnd)) {
        PostMessageW(progressWnd, WM_APP_INSTALL_DONE, 0, (LPARAM)done);
    } else {
        free(done);
    }
    return 0;
}

#define ID_TASKDLG_DOWNLOAD_ANYWAY 9001

/* shown when an install failed at the archive stage (corrupted file,
   unrecognized type, missing extractor tool): lets the person keep the
   raw archive anyway by copying it into the destination folder as-is,
   instead of losing it. A downloaded temp file is cleaned up here
   whichever way they answer; a local file the person dropped in is
   theirs and is never deleted. */
static void ShowInstallFailedWithDownloadAnyway(HWND owner, InstallDoneMsg* d)
{
    const TASKDIALOG_BUTTON buttons[] = {
        { ID_TASKDLG_DOWNLOAD_ANYWAY, L"Download Anyway" },
    };

    TASKDIALOGCONFIG tdc;
    memset(&tdc, 0, sizeof(tdc));
    tdc.cbSize = sizeof(tdc);
    tdc.hwndParent = owner;
    tdc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    tdc.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    tdc.pszWindowTitle = L"The Choicer Voicer - Launcher";
    tdc.pszMainIcon = TD_ERROR_ICON;
    tdc.pszMainInstruction = L"Couldn't install this mod";
    tdc.pszContent = d->resultMsg;
    tdc.pszExpandedInformation =
        L"\"Download Anyway\" skips extraction and just saves the file itself into "
        L"the destination folder, so you can try opening or fixing it yourself.";
    tdc.pButtons = buttons;
    tdc.cButtons = ARRAYSIZE(buttons);
    tdc.nDefaultButton = IDCLOSE; /* Close is the default, so Enter never saves a possibly-broken file by accident */

    int pressed = 0;
    HRESULT hr = TaskDialogIndirect(&tdc, &pressed, NULL, NULL);

    if (SUCCEEDED(hr) && pressed == ID_TASKDLG_DOWNLOAD_ANYWAY) {
        const wchar_t* lastSlash = wcsrchr(d->rawFilePath, L'\\');
        const wchar_t* rawName = lastSlash ? lastSlash + 1 : d->rawFilePath;
        const wchar_t* dot = wcsrchr(rawName, L'.');

        wchar_t savedName[MAX_PATH];
        if (d->rawFileIsTemp) {
            /* the temp file's own name is just a download-tick number,
               so give the saved copy a readable one instead */
            wsprintfW(savedName, L"downloaded_mod%s", dot ? dot : L".bin");
        } else {
            wcsncpy(savedName, rawName, MAX_PATH - 1);
            savedName[MAX_PATH - 1] = 0;
        }

        wchar_t wanted[MAX_PATH], finalPath[MAX_PATH];
        wsprintfW(wanted, L"%s\\%s", d->destPackDir, savedName);
        MakeUniquePathW(wanted, finalPath);

        if (CopyFileW(d->rawFilePath, finalPath, TRUE)) {
            wchar_t doneMsg[MAX_PATH + 120];
            wsprintfW(doneMsg, L"Saved the file as-is to:\n%s", finalPath);
            MessageBoxW(owner, doneMsg, L"The Choicer Voicer - Launcher", MB_ICONINFORMATION | MB_OK);
        } else {
            MessageBoxW(owner, L"Couldn't copy the file into the destination folder.",
                L"The Choicer Voicer - Launcher", MB_ICONERROR | MB_OK);
        }
    }

    if (d->rawFileIsTemp) DeleteFileW(d->rawFilePath);
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
            if (!d->success && d->offerDownloadAnyway) {
                ShowInstallFailedWithDownloadAnyway(owner, d);
            } else {
                MessageBoxW(owner, d->resultMsg, L"The Choicer Voicer - Launcher",
                    d->success ? (MB_ICONINFORMATION | MB_OK) : (MB_ICONERROR | MB_OK));
            }
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
    DWORD style = WS_POPUP | WS_CAPTION | WS_CLIPCHILDREN;
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

static void StartInstallJob(HWND mainWnd, BOOL fromUrl, const wchar_t* urlOrPath, const wchar_t* destPackFolder,
    const wchar_t* metaTitle, const wchar_t* metaAuthor, const wchar_t* metaThumbUrl, const wchar_t* metaDescription)
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
    wcsncpy(job->destPackFolder, destPackFolder, MAX_PATH - 1); job->destPackFolder[MAX_PATH - 1] = 0;
    if (metaTitle) { wcsncpy(job->metaTitle, metaTitle, 127); job->metaTitle[127] = 0; }
    if (metaAuthor) { wcsncpy(job->metaAuthor, metaAuthor, 63); job->metaAuthor[63] = 0; }
    if (metaThumbUrl) { wcsncpy(job->metaThumbUrl, metaThumbUrl, 511); job->metaThumbUrl[511] = 0; }
    if (metaDescription) { wcsncpy(job->metaDescription, metaDescription, 399); job->metaDescription[399] = 0; }

    HANDLE hThread = CreateThread(NULL, 0, InstallJobThreadProc, job, 0, NULL);
    if (hThread) CloseHandle(hThread);
    else { free(job); DestroyWindow(progressWnd); }
}

/* ---- destination picker: which folder to install a mod-browser pick
   into, shown before StartInstallJob runs. The globals below are set
   directly by ModListDoInstall before it calls ShowDestPicker (which is
   defined later, alongside the rest of the folder-tree picker UI - see
   the forward declaration above) */

static wchar_t g_destPickerUrl[2048] = L"";
static wchar_t g_destPickerTitle[128] = L"";
static wchar_t g_destPickerAuthor[64] = L"";
static wchar_t g_destPickerThumbUrl[512] = L"";
static wchar_t g_destPickerDescription[400] = L"";

/* ===================== "Add Version" popup ===================== */
/* Copies a chosen EXE into a new Normal/<version>/ or
   Compatibility/<version>/ folder alongside a display.txt containing
   that same version string - matching exactly the layout
   ScanVersionFolder/ReadDisplayNameTxt already expect from a manually
   dropped-in version, so the new entry shows up in the dropdown the
   same way any other version would. */

#define ID_ADDVER_BROWSE  7001
#define ID_ADDVER_ADD     7002
#define ID_ADDVER_CANCEL  7003
#define ID_ADDVER_CHECK1  7004
#define ID_ADDVER_CHECK2  7005

static BOOL g_addVerIsNormal = TRUE;
static wchar_t g_addVerExePath[MAX_PATH] = L"";
static HWND g_hAddVerOwner = NULL;
static HWND g_hAddVerExeEdit = NULL;
static HWND g_hAddVerVersionEdit = NULL;
static HWND g_hAddVerCheck1 = NULL;
static HWND g_hAddVerCheck2 = NULL;
static BOOL g_addVerCheck1State = FALSE;
static BOOL g_addVerCheck2State = FALSE;
static wchar_t g_addVerCheck1Label[128] = L"";
static wchar_t g_addVerCheck2Label[128] = L"";

/* strict #.#.# - each # is one or more digits, exactly two dots,
   nothing else allowed anywhere in the string */
static BOOL IsValidVersionString(const wchar_t* s)
{
    int dots = 0;
    int digitsInSegment = 0;
    for (const wchar_t* p = s; *p; p++) {
        if (*p == L'.') {
            if (digitsInSegment == 0) return FALSE; /* e.g. "..", ".1.2", "1..2" */
            dots++;
            digitsInSegment = 0;
        } else if (*p >= L'0' && *p <= L'9') {
            digitsInSegment++;
        } else {
            return FALSE; /* any non-digit, non-dot character */
        }
    }
    return digitsInSegment > 0 && dots == 2; /* no trailing dot, exactly 3 segments */
}

static BOOL BrowseForVersionExe(HWND hOwner, wchar_t* outPath, DWORD outCap)
{
    wchar_t fileBuf[MAX_PATH] = L"";
    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hOwner;
    ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Select the version's EXE";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return FALSE;
    wcsncpy(outPath, fileBuf, outCap - 1);
    outPath[outCap - 1] = 0;
    return TRUE;
}

static void CloseAddVersionPopup(HWND hwnd)
{
    HWND owner = g_hAddVerOwner;
    DestroyWindow(hwnd);
    if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
    g_hAddVerExeEdit = NULL;
    g_hAddVerVersionEdit = NULL;
    g_hAddVerCheck1 = NULL;
    g_hAddVerCheck2 = NULL;
    g_addVerCheck1State = FALSE;
    g_addVerCheck2State = FALSE;
}

static void PerformAddVersion(HWND hwnd)
{
    wchar_t verBuf[64];
    GetWindowTextW(g_hAddVerVersionEdit, verBuf, 64);

    /* trim surrounding whitespace */
    int vlen = (int)wcslen(verBuf);
    while (vlen > 0 && (verBuf[vlen - 1] == L' ' || verBuf[vlen - 1] == L'\t')) verBuf[--vlen] = 0;
    int vstart = 0;
    while (verBuf[vstart] == L' ' || verBuf[vstart] == L'\t') vstart++;
    if (vstart > 0) memmove(verBuf, verBuf + vstart, (size_t)(vlen - vstart + 1) * sizeof(wchar_t));

    if (!g_addVerExePath[0]) {
        MessageBoxW(hwnd, L"Choose an EXE file first.", L"The Choicer Voicer - Launcher", MB_ICONWARNING | MB_OK);
        return;
    }
    if (!IsValidVersionString(verBuf)) {
        MessageBoxW(hwnd, L"Version number must be in #.#.# format (e.g. 1.5.3).",
            L"The Choicer Voicer - Launcher", MB_ICONWARNING | MB_OK);
        return;
    }
    if (!g_addVerCheck1State || !g_addVerCheck2State) {
        MessageBoxW(hwnd, L"Please confirm both checkboxes before adding this version.",
            L"The Choicer Voicer - Launcher", MB_ICONWARNING | MB_OK);
        return;
    }

    wchar_t exeDir[MAX_PATH];
    GetExeDir(exeDir, MAX_PATH);
    wchar_t modeDir[MAX_PATH];
    wsprintfW(modeDir, L"%s\\%s", exeDir, g_addVerIsNormal ? NORMAL_DIR_NAME : COMPAT_DIR_NAME);

    wchar_t versionDir[MAX_PATH];
    wsprintfW(versionDir, L"%s\\%s", modeDir, verBuf);
    if (PathIsDirW(versionDir)) {
        wchar_t msg[700];
        wsprintfW(msg, L"A version folder already exists for %s:\n%s\n\nPick a different version number.",
            verBuf, versionDir);
        MessageBoxW(hwnd, msg, L"The Choicer Voicer - Launcher", MB_ICONWARNING | MB_OK);
        return;
    }
    /* SHCreateDirectoryExW (behind EnsureDirW) creates the whole nested
       path in one go - no need to create modeDir separately first */
    if (!EnsureDirW(versionDir)) {
        MessageBoxW(hwnd, L"Couldn't create the version folder.",
            L"The Choicer Voicer - Launcher", MB_ICONERROR | MB_OK);
        return;
    }

    const wchar_t* exeFileName = wcsrchr(g_addVerExePath, L'\\');
    exeFileName = exeFileName ? exeFileName + 1 : g_addVerExePath;
    wchar_t destExePath[MAX_PATH];
    wsprintfW(destExePath, L"%s\\%s", versionDir, exeFileName);

    if (!CopyFileW(g_addVerExePath, destExePath, FALSE)) {
        MessageBoxW(hwnd, L"Couldn't copy the EXE into the version folder.",
            L"The Choicer Voicer - Launcher", MB_ICONERROR | MB_OK);
        return;
    }

    /* display.txt holds the same version text the user typed in, read
       back by ReadDisplayNameTxt as this version's label - plain UTF-8,
       no BOM needed since the decoder already tolerates either and the
       version string itself is always plain ASCII digits and dots */
    wchar_t displayTxtPath[MAX_PATH];
    wsprintfW(displayTxtPath, L"%s\\display.txt", versionDir);
    HANDLE hFile = CreateFileW(displayTxtPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, verBuf, -1, NULL, 0, NULL, NULL);
        if (utf8Len > 0) {
            char* utf8 = (char*)malloc((size_t)utf8Len);
            if (utf8) {
                WideCharToMultiByte(CP_UTF8, 0, verBuf, -1, utf8, utf8Len, NULL, NULL);
                DWORD written = 0;
                WriteFile(hFile, utf8, (DWORD)(utf8Len - 1), &written, NULL); /* drop the NUL */
                free(utf8);
            }
        }
        CloseHandle(hFile);
    }

    /* refresh the relevant dropdown so the new version shows up immediately */
    VersionList* list = g_addVerIsNormal ? &g_normalVersions : &g_compatVersions;
    HWND combo = g_addVerIsNormal ? g_hNormalCombo : g_hCompatCombo;
    HWND launchBtn = g_addVerIsNormal ? g_hLaunchNormalBtn : g_hLaunchCompatBtn;
    ScanModeFolder(modeDir, list);
    PopulateVersionCombo(combo, list, launchBtn);
    {
        LRESULT idx = SendMessageW(combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)verBuf);
        if (idx != CB_ERR) SendMessageW(combo, CB_SETCURSEL, (WPARAM)idx, 0);
    }

    HWND owner = g_hAddVerOwner;
    CloseAddVersionPopup(hwnd);

    wchar_t doneMsg[600];
    wsprintfW(doneMsg, L"Version %s added to %s.", verBuf, g_addVerIsNormal ? L"Normal Mode" : L"Compatibility Mode");
    MessageBoxW(owner, doneMsg, L"The Choicer Voicer - Launcher", MB_ICONINFORMATION | MB_OK);
}

static void UpdateAddVerCheckText(HWND ctl, BOOL state, const wchar_t* label)
{
    wchar_t buf[160];
    wsprintfW(buf, L"%s %s", state ? L"\u2611" : L"\u2610", label);
    SetWindowTextW(ctl, buf);
}

static LRESULT CALLBACK AddVersionWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
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
        SetTextColor(hdc, g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT);
        return (LRESULT)(g_hBrushBg ? g_hBrushBg : GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_CTLCOLOREDIT: {
        /* editable text boxes (the version number field) send this instead
           of WM_CTLCOLORSTATIC, and it wasn't handled, so the box always
           painted with the OS default white */
        HDC hdc = (HDC)wParam;
        SetBkColor(hdc, g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL);
        SetTextColor(hdc, g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT);
        return (LRESULT)(g_hBrushPanel ? g_hBrushPanel : GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_ADDVER_BROWSE: {
            wchar_t chosen[MAX_PATH];
            if (BrowseForVersionExe(hwnd, chosen, MAX_PATH)) {
                wcsncpy(g_addVerExePath, chosen, MAX_PATH - 1);
                g_addVerExePath[MAX_PATH - 1] = 0;
                SendMessageW(g_hAddVerExeEdit, WM_SETTEXT, 0, (LPARAM)g_addVerExePath);
            }
            return 0;
        }
        case ID_ADDVER_CHECK1:
            g_addVerCheck1State = !g_addVerCheck1State;
            UpdateAddVerCheckText(g_hAddVerCheck1, g_addVerCheck1State, g_addVerCheck1Label);
            return 0;
        case ID_ADDVER_CHECK2:
            g_addVerCheck2State = !g_addVerCheck2State;
            UpdateAddVerCheckText(g_hAddVerCheck2, g_addVerCheck2State, g_addVerCheck2Label);
            return 0;
        case ID_ADDVER_ADD:
            PerformAddVersion(hwnd);
            return 0;
        case ID_ADDVER_CANCEL:
            CloseAddVersionPopup(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        CloseAddVersionPopup(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowAddVersionPopup(HWND hOwner, HINSTANCE hInst, BOOL isNormal)
{
    g_addVerIsNormal = isNormal;
    g_addVerExePath[0] = 0;

    static BOOL classRegistered = FALSE;
    const wchar_t CLASS_NAME[] = L"CVLauncherAddVersionWnd";
    if (!classRegistered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc   = AddVersionWndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = CLASS_NAME;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
        RegisterClassW(&wc);
        classRegistered = TRUE;
    }

    const int w = 420, h = 358;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    RECT rc = {0, 0, w, h};
    AdjustWindowRect(&rc, style, FALSE);
    int winW = rc.right - rc.left, winH = rc.bottom - rc.top;

    RECT prc; GetWindowRect(hOwner, &prc);
    int posX = prc.left + ((prc.right - prc.left) - winW) / 2;
    int posY = prc.top + ((prc.bottom - prc.top) - winH) / 2;

    g_hAddVerOwner = hOwner;

    HWND hPop = CreateWindowExW(WS_EX_DLGMODALFRAME, CLASS_NAME,
        isNormal ? L"Add Normal Mode Version" : L"Add Compatibility Mode Version",
        style, posX, posY, winW, winH, hOwner, NULL, hInst, NULL);
    if (!hPop) return;
    ApplyDarkTitlebar(hPop, g_darkMode);

    /* tall enough for 3 wrapped lines at this popup's font size - it was
       clipped to 2 lines' worth of height before, cutting the text off */
    wchar_t introText[200];
    wsprintfW(introText, L"Pick the EXE for this %s version, give it a version number, then confirm below.",
        isNormal ? L"Normal Mode" : L"Compatibility Mode");
    HWND hIntro = CreateWindowW(L"STATIC", introText,
        WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 16, w - 40, 54, hPop, NULL, hInst, NULL);
    SendMessageW(hIntro, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);

    g_hAddVerExeEdit = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_READONLY,
        20, 76, w - 40 - 90 - 10, 24, hPop, NULL, hInst, NULL);
    SendMessageW(g_hAddVerExeEdit, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);

    HWND hBrowseBtn = MakeButton(hPop, hInst, L"Browse\u2026",
        w - 20 - 90, 75, 90, 26, ID_ADDVER_BROWSE, g_fontRegular);

    HWND hVerLabel = CreateWindowW(L"STATIC", L"Version number (#.#.#), e.g. 1.5.3:",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 114, w - 40, 18, hPop, NULL, hInst, NULL);
    SendMessageW(hVerLabel, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);

    g_hAddVerVersionEdit = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        20, 134, 160, 24, hPop, NULL, hInst, NULL);
    SendMessageW(g_hAddVerVersionEdit, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);

    /* same "\u2610/\u2611 + label" toggle-button format the Dark Mode
       control used before it moved into the sidebar, rather than native
       checkboxes - kept consistent with the rest of the app's controls */
    wsprintfW(g_addVerCheck1Label, L"I confirm this EXE is the %s version.",
        isNormal ? L"Normal Mode" : L"Compatibility Mode");
    wcscpy(g_addVerCheck2Label, L"I confirm this launcher's own EXE is in its own separate, isolated folder.");
    g_addVerCheck1State = FALSE;
    g_addVerCheck2State = FALSE;

    g_hAddVerCheck1 = MakeButton(hPop, hInst, L"", 20, 174, w - 40, 34, ID_ADDVER_CHECK1, g_fontRegular);
    SetWindowLongW(g_hAddVerCheck1, GWL_STYLE, GetWindowLongW(g_hAddVerCheck1, GWL_STYLE) | BS_MULTILINE);
    UpdateAddVerCheckText(g_hAddVerCheck1, FALSE, g_addVerCheck1Label);

    g_hAddVerCheck2 = MakeButton(hPop, hInst, L"", 20, 212, w - 40, 54, ID_ADDVER_CHECK2, g_fontRegular);
    SetWindowLongW(g_hAddVerCheck2, GWL_STYLE, GetWindowLongW(g_hAddVerCheck2, GWL_STYLE) | BS_MULTILINE);
    UpdateAddVerCheckText(g_hAddVerCheck2, FALSE, g_addVerCheck2Label);

    HWND hAddBtn = MakeButton(hPop, hInst, L"Add Version",
        20, h - 56, 150, 34, ID_ADDVER_ADD, g_fontRegular);
    (void)hAddBtn;

    HWND hCancelBtn = MakeButton(hPop, hInst, L"Cancel",
        w - 20 - 110, h - 56, 110, 34, ID_ADDVER_CANCEL, g_fontRegular);
    (void)hCancelBtn;

    EnableWindow(hOwner, FALSE);
    ShowWindow(hPop, SW_SHOW);
    UpdateWindow(hPop);
    SetForegroundWindow(hPop);
    SetFocus(hBrowseBtn);
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

/* AlphaBlend requires premultiplied alpha; WIC's 32bppBGRA decode above
   gives straight (unassociated) alpha instead, which would fringe the
   anti-aliased edges of the custom icon art (rounded corners, circle
   edges) if used with AlphaBlend as-is. This converts a decoded
   bitmap's own pixel buffer to premultiplied form in place. Only needed
   for bitmaps that will be drawn with AlphaBlend (the button icons/logo
   below) - the existing StretchBlt-drawn thumbnails elsewhere don't
   look at the alpha channel at all, so leaving those untouched is fine. */
static void PremultiplyBitmapAlpha(HBITMAP hbmp)
{
    if (!hbmp) return;
    DIBSECTION dib;
    if (GetObjectW(hbmp, sizeof(dib), &dib) != sizeof(dib)) return;
    if (dib.dsBm.bmBitsPixel != 32 || !dib.dsBm.bmBits) return;
    BYTE* p = (BYTE*)dib.dsBm.bmBits;
    LONG h = dib.dsBm.bmHeight; if (h < 0) h = -h;
    LONG count = dib.dsBm.bmWidth * h;
    for (LONG i = 0; i < count; i++) {
        BYTE a = p[3];
        if (a != 255) {
            p[0] = (BYTE)((p[0] * a) / 255);
            p[1] = (BYTE)((p[1] * a) / 255);
            p[2] = (BYTE)((p[2] * a) / 255);
        }
        p += 4;
    }
}

/* loads a PNG embedded as an RCDATA resource (see resource.rc) and
   decodes+premultiplies it in one step, ready for AlphaBlend */
static HBITMAP LoadPngResourceBitmap(HINSTANCE hInst, int resId, IWICImagingFactory* pFactory)
{
    HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!hRes) return NULL;
    HGLOBAL hData = LoadResource(hInst, hRes);
    if (!hData) return NULL;
    BYTE* data = (BYTE*)LockResource(hData);
    DWORD size = SizeofResource(hInst, hRes);
    if (!data || size == 0) return NULL;
    HBITMAP hbmp = DecodeImageToBitmap(pFactory, data, size);
    PremultiplyBitmapAlpha(hbmp);
    return hbmp;
}

/* draws one of the custom icon-button bitmaps (background baked in) via
   AlphaBlend, then a thin accent ring on top for hover/pressed feedback -
   since the fill color used to carry that feedback, but now the fill is
   part of the fixed art, a ring drawn over it is what shows interaction
   state instead */
static void DrawIconButtonBitmap(HDC hdc, RECT rcItem, HBITMAP hbmp, BOOL isCircle,
    BOOL hovered, BOOL pressed, BOOL disabled)
{
    if (hbmp) {
        HDC memDC = CreateCompatibleDC(hdc);
        HGDIOBJ oldBmp = SelectObject(memDC, hbmp);
        BITMAP bm; GetObjectW(hbmp, sizeof(bm), &bm);
        BLENDFUNCTION bf;
        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = disabled ? 140 : 255;
        bf.AlphaFormat = AC_SRC_ALPHA;
        AlphaBlend(hdc, rcItem.left, rcItem.top, rcItem.right - rcItem.left, rcItem.bottom - rcItem.top,
            memDC, 0, 0, bm.bmWidth, bm.bmHeight, bf);
        SelectObject(memDC, oldBmp);
        DeleteDC(memDC);
    }

    if (!pressed && !hovered) return;
    COLORREF ringColor = pressed ? THEME_ACCENT : (g_darkMode ? RGB(150, 150, 150) : RGB(110, 110, 110));
    HPEN ringPen = CreatePen(PS_SOLID, pressed ? 2 : 1, ringColor);
    HGDIOBJ oldPen = SelectObject(hdc, ringPen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RECT ringRc = rcItem;
    InflateRect(&ringRc, -1, -1);
    if (isCircle) {
        Ellipse(hdc, ringRc.left, ringRc.top, ringRc.right, ringRc.bottom);
    } else {
        RoundRect(hdc, ringRc.left, ringRc.top, ringRc.right, ringRc.bottom, 14, 14);
    }
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(ringPen);
}

/* ---- mod list state (populated on a background thread, only ever read/
   written on the UI thread once handed over via WM_APP_MODS_LOADED) ---- */

#define MODS_PER_PAGE 20
#define MAX_MOD_ENTRIES 2000
#define WM_APP_MODS_LOADED    (WM_APP + 1)
#define WM_APP_MODS_PROGRESS  (WM_APP + 20)   /* the fetch thread finished another raw page (repaint the "checked so far" text) */

typedef struct {
    long modId;
    wchar_t name[128];
    wchar_t author[64];
    wchar_t category[64];
    wchar_t profileUrl[160];
    long likeCount;
    long viewCount;
    long dateAdded;
    wchar_t thumbUrl[512];    /* kept alongside the decoded thumb so the
                                  Installed Mods tab can re-fetch/store it
                                  as a mod's coverphoto after install */
    wchar_t description[400]; /* only ever populated for search-mode
                                  fetches, which request it explicitly -
                                  see StartModFetch/isSearch below */
    HBITMAP thumb;
} ModEntry;

/* ---- mod types (browse filter) ----
   The same buckets the TCV Mod Manager by KevTheDev01 sorts GameBanana mods into: it reads each
   mod's root category name and picks a type from keywords in it, with anything unrecognised
   counting as Voice & Dub. (https://github.com/KevTheDev01/tcv-mod-manager) */
typedef enum { MODTYPE_ALL = 0, MODTYPE_VOICE, MODTYPE_PLAYERS, MODTYPE_STUDIOS, MODTYPE_JUDGES,
               MODTYPE_MENUS, MODTYPE_HOSTS, MODTYPE_CHATTER, MODTYPE_COUNT } ModType;
static const wchar_t* const g_modTypeLabels[MODTYPE_COUNT] = {
    L"All", L"Voice & Dub", L"Players", L"Studios", L"Judges", L"Menus", L"Hosts", L"Chatter" };
static int g_modTypeFilter = MODTYPE_ALL;       /* the chip that is currently selected */

static ModType ModTypeFromCategory(const wchar_t* category)
{
    wchar_t c[64];
    wcsncpy(c, category ? category : L"", 63);
    c[63] = 0;
    _wcslwr(c);
    if (wcsstr(c, L"player") || wcsstr(c, L"contestant")) return MODTYPE_PLAYERS;
    if (wcsstr(c, L"studio"))  return MODTYPE_STUDIOS;
    if (wcsstr(c, L"judge"))   return MODTYPE_JUDGES;
    if (wcsstr(c, L"menu"))    return MODTYPE_MENUS;
    if (wcsstr(c, L"host"))    return MODTYPE_HOSTS;
    if (wcsstr(c, L"chatter")) return MODTYPE_CHATTER;
    return MODTYPE_VOICE;
}

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
typedef enum { MODSORT_NEWEST = 0, MODSORT_LIKES, MODSORT_VIEWS } ModSortMode;
static ModSortMode g_modSortMode = MODSORT_NEWEST;
static wchar_t g_modSearchQuery[128] = L"";
static int g_modFilteredIndices[MAX_MOD_ENTRIES];
static int g_modFilteredCount = 0;
static HWND g_hModSearchEdit = NULL;
static HWND g_hModSortCombo = NULL;

#define ID_MODLIST_SEARCH 5010
#define ID_MODLIST_SORT   5011
#define ID_MODLIST_DOWNLOAD_LINK 5015
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
   returns NULL (omit _sSort entirely), since the
   confirmed default behavior with no _sSort at all is newest-first, and
   NEWEST has no server value either (see above), so both fall to the
   same default case. */
static const wchar_t* ModSortServerValue(ModSortMode mode)
{
    switch (mode) {
    case MODSORT_LIKES: return L"Generic_MostLiked";
    case MODSORT_VIEWS: return L"Generic_MostViewed";
    case MODSORT_NEWEST:
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

/* How the default (non-search) browse is scoped to this game:
     0 = not tried yet, 1 = _aFilters[Generic_Game] works (shows EVERY category),
     2 = the server ignored that filter, so use the single category instead (old behavior).
   Decided from the first page that comes back; kept for the rest of the session. */
static volatile LONG g_gameFilterMode = 0;
/* Whether the TCV Mod Manager's game-scoped listing (apiv11 Game/<id>/Subfeed) works from here, for the
   default Newest browse: 0 = not tried yet, 1 = works, 2 = didn't (fall back to the Mod/Index requests). */
static volatile LONG g_subfeedState = 0;
static long g_modSeq = 0;      /* orders Subfeed records that come without a date */
static volatile LONG g_modScanned = 0;   /* GameBanana records looked at since the list was last reset (for the progress text) */
/* A type chip can leave a fetch with few or no matches while much of the feed is still unread. Rather than
   stopping there (and claiming the type doesn't exist), the list keeps fetching in the background - like
   the TCV Mod Manager, which just loads everything - until a full page of results has turned up, the feed
   ends, or this budget of chained fetches is spent (then Next keeps looking). */
#define MOD_AUTO_FETCH_BUDGET 12
static int  g_modAutoLeft = MOD_AUTO_FETCH_BUDGET;
static BOOL g_modAutoFetch = FALSE;      /* TRUE only for the fetch the list starts by itself */

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
    BOOL forceCategory = FALSE;   /* this fetch only: the whole-game request errored, so use the category one */
    BOOL skipSubfeed = FALSE;     /* this fetch only: the Subfeed request failed, so use Mod/Index */
    /* the type chip: only mods of that type are kept, so a rare type may need many raw pages to fill a batch */
    const int typeFilter = g_modTypeFilter;
    const int maxRawPages = (typeFilter != MODTYPE_ALL) ? 20 : MOD_FETCH_MAX_RAW_PAGES;

    while (entries && count < MODS_PER_PAGE && pagesChecked < maxRawPages && rawPageWasFull) {
        if (pagesChecked > 0) Sleep(200); /* be a reasonable API citizen across a multi-page scan */

        /* default Newest browse: ask for the whole game the way the TCV Mod Manager does */
        const BOOL useSubfeed = (!isSearch && !sortParam && g_subfeedState != 2 && !skipSubfeed && !forceCategory);
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
        } else if (useSubfeed) {
            wsprintfW(apiUrl, L"https://gamebanana.com/apiv11/Game/%ld/Subfeed?_nPage=%d", GAMEBANANA_GAME_ID, page);
        } else if (sortParam) {
            if (g_gameFilterMode != 2 && !forceCategory)
                wsprintfW(apiUrl,
                    L"https://gamebanana.com/apiv13/Mod/Index?_nPage=%d&_nPerpage=%d&_aFilters%%5BGeneric_Game%%5D=%ld&_sSort=%s",
                    page, MOD_FETCH_RAW_PERPAGE, GAMEBANANA_GAME_ID, sortParam);
            else
                wsprintfW(apiUrl,
                    L"https://gamebanana.com/apiv13/Mod/Index?_nPage=%d&_nPerpage=%d&_aFilters%%5BGeneric_Category%%5D=%ld&_sSort=%s",
                    page, MOD_FETCH_RAW_PERPAGE, GAMEBANANA_CATEGORY_ID, sortParam);
        } else {
            if (g_gameFilterMode != 2 && !forceCategory)
                wsprintfW(apiUrl,
                    L"https://gamebanana.com/apiv13/Mod/Index?_nPage=%d&_nPerpage=%d&_aFilters%%5BGeneric_Game%%5D=%ld",
                    page, MOD_FETCH_RAW_PERPAGE, GAMEBANANA_GAME_ID);
            else
                wsprintfW(apiUrl,
                    L"https://gamebanana.com/apiv13/Mod/Index?_nPage=%d&_nPerpage=%d&_aFilters%%5BGeneric_Category%%5D=%ld",
                    page, MOD_FETCH_RAW_PERPAGE, GAMEBANANA_CATEGORY_ID);
        }
        const BOOL triedGameFilter = (!isSearch && !useSubfeed && g_gameFilterMode != 2 && !forceCategory);
        const int pageThisIteration = page;
        const int countBeforePage = count;

        wchar_t errMsg[256] = L"";
        DWORD jsonSize = 0;
        BYTE* jsonData = HttpGetToMemory(apiUrl, &jsonSize, errMsg);
        pagesChecked++;
        page++;
        if (!jsonData) {
            wcsncpy(lastErr, errMsg[0] ? errMsg : L"Couldn't reach GameBanana.", 255);
            if (useSubfeed && g_subfeedState == 0) {
                skipSubfeed = TRUE;                 /* try the Mod/Index requests for this fetch instead */
                page = pageThisIteration;
                pagesChecked = 0;
                continue;
            }
            if (triedGameFilter && g_gameFilterMode == 0) {
                /* the very first whole-game request failed (an error status would look like this, but so
                   would being offline): redo it with the category query, without writing the mode down */
                forceCategory = TRUE;
                page = pageThisIteration;
                pagesChecked = 0;
                continue;
            }
            continue; /* could be transient - keep trying the next page */
        }

        char* json = (char*)malloc((size_t)jsonSize + 1);
        if (!json) { free(jsonData); wcscpy(lastErr, L"Out of memory."); break; }
        memcpy(json, jsonData, jsonSize);
        json[jsonSize] = 0;
        free(jsonData);
        const char* jsonEnd = json + jsonSize;

        const char* recPos = FindSubstrBounded(json, jsonEnd, "\"_aRecords\"");
        if (!recPos && useSubfeed && g_subfeedState == 0) {
            free(json);
            skipSubfeed = TRUE;
            page = pageThisIteration;
            pagesChecked = 0;
            continue;
        }
        if (!recPos && triedGameFilter && g_gameFilterMode == 0) {
            /* an error page / unexpected reply to the whole-game request - try the category query instead */
            free(json);
            forceCategory = TRUE;
            page = pageThisIteration;
            pagesChecked = 0;
            continue;
        }
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
        int validOnPage = 0;          /* records that passed the game + Mod checks (before the type filter) */
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
            if (gameId != GAMEBANANA_GAME_ID && !(useSubfeed && gameId == -1)) continue;

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
            if (useSubfeed) {   /* like the Mod Manager: skip submissions that have no downloadable files */
                const char* hf = FindSubstrBounded(recStart, recEnd, "\"_bHasFiles\":");
                if (hf) {
                    const char* hv = hf + strlen("\"_bHasFiles\":");
                    while (hv < recEnd && (*hv == ' ' || *hv == '\t')) hv++;
                    if (recEnd - hv >= 5 && strncmp(hv, "false", 5) == 0) continue;
                }
            }
            validOnPage++;

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

            /* type chip: keep only mods of the chosen type (done here, before any thumbnail is downloaded) */
            if (typeFilter != MODTYPE_ALL && (int)ModTypeFromCategory(ne.category) != typeFilter) continue;

            const char* descPos = FindSubstrBounded(recStart, recEnd, "\"description\":");
            if (descPos) ExtractJsonStringAt(descPos + 15, recEnd, ne.description, 400);

            const char* likePos = FindSubstrBounded(recStart, recEnd, "\"_nLikeCount\":");
            if (likePos) ExtractJsonIntAt(likePos + strlen("\"_nLikeCount\":"), recEnd, &ne.likeCount);
            const char* viewPos = FindSubstrBounded(recStart, recEnd, "\"_nViewCount\":");
            if (viewPos) ExtractJsonIntAt(viewPos + strlen("\"_nViewCount\":"), recEnd, &ne.viewCount);
            const char* datePos = FindSubstrBounded(recStart, recEnd, "\"_tsDateAdded\":");
            if (datePos) ExtractJsonIntAt(datePos + strlen("\"_tsDateAdded\":"), recEnd, &ne.dateAdded);
            if (ne.dateAdded == 0 && useSubfeed) ne.dateAdded = 2000000000L - (++g_modSeq);   /* keep the feed's own order */

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

            if (!thumbUrl[0]) {
                /* Subfeed / apiv11 records keep their picture under _aPreviewMedia._aImages (same as the
                   TCV Mod Manager reads it) */
                const char* pm = FindSubstrBounded(recStart, recEnd, "\"_aPreviewMedia\":");
                if (pm) {
                    wchar_t baseUrl[256] = L"", fileName[128] = L"";
                    const char* bp = FindSubstrBounded(pm, recEnd, "\"_sBaseUrl\":");
                    if (bp) ExtractJsonStringAt(bp + strlen("\"_sBaseUrl\":"), recEnd, baseUrl, 256);
                    const char* fp = FindSubstrBounded(pm, recEnd, "\"_sFile220\":");
                    size_t fpl = strlen("\"_sFile220\":");
                    if (!fp) { fp = FindSubstrBounded(pm, recEnd, "\"_sFile530\":"); fpl = strlen("\"_sFile530\":"); }
                    if (!fp) { fp = FindSubstrBounded(pm, recEnd, "\"_sFile\":");    fpl = strlen("\"_sFile\":"); }
                    if (fp) ExtractJsonStringAt(fp + fpl, recEnd, fileName, 128);
                    if (baseUrl[0] && fileName[0]) wsprintfW(thumbUrl, L"%s/%s", baseUrl, fileName);
                }
            }

            wcsncpy(ne.thumbUrl, thumbUrl, 511); ne.thumbUrl[511] = 0;

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

        /* First page of an untested session: did the Generic_Game filter really scope the results?
           If it did, nearly every record on a 50-record page is ours (>= 10 is a very safe bar - an
           unscoped page of GameBanana-wide mods would contain essentially none). If not, redo this
           same page with the single-category query, which is known to scope correctly. */
        if (useSubfeed && g_subfeedState == 0) {
            /* did the game-scoped feed give us mods? (the type chip is deliberately not part of this test) */
            if (validOnPage >= 1) {
                g_subfeedState = 1;
            } else if (recordsOnThisPage > 0 && pagesChecked < 3) {
                /* a page made up only of non-Mod submissions (threads, WIPs...) says nothing yet - look at the next page */
            } else {
                g_subfeedState = 2;                 /* not usable from here: use the Mod/Index requests from now on */
                free(json);
                page = pageThisIteration;
                pagesChecked = 0;
                rawPageWasFull = TRUE;
                continue;
            }
        }
        if (triedGameFilter && g_gameFilterMode == 0 && recordsOnThisPage > 0) {
            if (validOnPage >= MODS_PER_PAGE / 2) {
                g_gameFilterMode = 1;
            } else {
                g_gameFilterMode = 2;
                free(json);
                page = pageThisIteration;
                pagesChecked = 0;
                rawPageWasFull = TRUE;
                continue;
            }
        }

        free(json);
        /* Subfeed pages are whatever size the feed uses, so only an empty page means "no more" there */
        rawPageWasFull = useSubfeed ? (recordsOnThisPage > 0) : (recordsOnThisPage >= MOD_FETCH_RAW_PERPAGE);
        InterlockedExchangeAdd(&g_modScanned, recordsOnThisPage);
        if (IsWindow(targetWnd)) PostMessageW(targetWnd, WM_APP_MODS_PROGRESS, 0, 0);
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
    if (!g_modAutoFetch) g_modAutoLeft = MOD_AUTO_FETCH_BUDGET;   /* a click / new search / new sort: fresh budget */
    g_modAutoFetch = FALSE;
    if (page == 1) g_modScanned = 0;
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
                    wchar_t destFolder[MAX_PATH];
                    GetDownloadDestFolder(destFolder, MAX_PATH);
                    wcsncpy(g_lastPackFolder, destFolder, MAX_PATH - 1); g_lastPackFolder[MAX_PATH - 1] = 0;
                    SaveSettings();
                    StartInstallJob(self->hwnd, FALSE, path, destFolder, NULL, NULL, NULL, NULL);
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
                wchar_t destFolder[MAX_PATH];
                GetDownloadDestFolder(destFolder, MAX_PATH);
                wcsncpy(g_lastPackFolder, destFolder, MAX_PATH - 1); g_lastPackFolder[MAX_PATH - 1] = 0;
                SaveSettings();
                StartInstallJob(self->hwnd, TRUE, url, destFolder, NULL, NULL, NULL, NULL);
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

    /* mingw's dwmapi.h doesn't define DWMWA_WINDOW_CORNER_PREFERENCE (33)
       or DWMWCP_ROUND (2) yet, so these are the raw documented values.
       Windows 11+ only; harmlessly ignored (non-S_OK) on Windows 10. */
    DWORD cornerPref = 2 /* DWMWCP_ROUND */;
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));
}

static void ApplyTipTheme(void)
{
    if (!g_hTip) return;
    /* turning visual styles off for the tooltip is what lets it take our colors */
    SetWindowTheme(g_hTip, L"", L"");
    SendMessageW(g_hTip, TTM_SETTIPBKCOLOR, (WPARAM)BlendColor(Pal()->panel, Pal()->text, 0.07f), 0);
    SendMessageW(g_hTip, TTM_SETTIPTEXTCOLOR, (WPARAM)(g_darkMode ? Pal()->text : BlendColor(Pal()->text, RGB(0,0,0), 0.3f)), 0);
}

static void ApplyThemeColors(HWND hwnd)
{
    g_bgCacheW = 0;          /* the tinted backdrop must be rebuilt for the new theme / mode */
    ApplyTipTheme();
    if (g_hBrushBg) DeleteObject(g_hBrushBg);
    if (g_hBrushPanel) DeleteObject(g_hBrushPanel);

    g_hBrushBg = CreateSolidBrush(g_darkMode ? COLOR_DARK_BG : COLOR_LIGHT_BG);
    g_hBrushPanel = CreateSolidBrush(g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL);

    ApplyDarkTitlebar(hwnd, g_darkMode);

    const wchar_t* comboTheme = g_darkMode ? L"DarkMode_Explorer" : L"Explorer";
    if (g_hNormalCombo) SetWindowTheme(g_hNormalCombo, comboTheme, NULL);
    if (g_hCompatCombo) SetWindowTheme(g_hCompatCombo, comboTheme, NULL);
    if (g_hModSortCombo) SetWindowTheme(g_hModSortCombo, comboTheme, NULL);
    if (g_hDubCombo) SetWindowTheme(g_hDubCombo, comboTheme, NULL);
    if (g_hDubLog) SetWindowTheme(g_hDubLog, comboTheme, NULL);
    if (g_hSetInfoEdit) SetWindowTheme(g_hSetInfoEdit, comboTheme, NULL);

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
        if (g_hTip) {
            TOOLINFOW ti; memset(&ti, 0, sizeof(ti));
            ti.cbSize = sizeof(ti); ti.hwnd = hwnd; ti.uId = (UINT_PTR)hwnd;
            SendMessageW(g_hTip, TTM_DELTOOLW, 0, (LPARAM)&ti);
        }
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

    /* hover description */
    const wchar_t* tip = ButtonTipText(id);
    if (tip) {
        if (!g_hTip) {
            g_hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, hInst, NULL);
            if (g_hTip) {
                SendMessageW(g_hTip, TTM_SETMAXTIPWIDTH, 0, 300);
                SendMessageW(g_hTip, TTM_SETDELAYTIME, TTDT_INITIAL, 400);
                SendMessageW(g_hTip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 9000);
                if (g_fontSmall) SendMessageW(g_hTip, WM_SETFONT, (WPARAM)g_fontSmall, FALSE);
                ApplyTipTheme();
            }
        }
        if (g_hTip) {
            TOOLINFOW ti; memset(&ti, 0, sizeof(ti));
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = ctl;
            ti.uId = (UINT_PTR)ctl;
            ti.lpszText = (LPWSTR)tip;
            SendMessageW(g_hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
    }
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

/* Generic owner-draw rendering for a plain MakeButton() button, shared by
   every popup's WM_DRAWITEM so Browse/Add Version/Cancel/Move/Back/etc.
   all pick up dark-mode colors and hover/press feedback the same way the
   main window's own buttons (like Launch) already do, instead of each
   popup falling back to the OS's default white push-button face. */
static void DrawThemedOwnerButton(LPDRAWITEMSTRUCT dis)
{
    BOOL pressed = (dis->itemState & ODS_SELECTED) != 0;
    BOOL disabled = (dis->itemState & ODS_DISABLED) != 0;
    BOOL hovered = GetPropW(dis->hwndItem, L"CVHover") != NULL;

    COLORREF fillNormal = g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL;
    COLORREF fillHover = BlendColor(fillNormal, g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT, 0.10f);
    COLORREF fillPressed = g_darkMode ? COLOR_DARK_PRESSED : COLOR_LIGHT_PRESSED;
    COLORREF border = g_darkMode ? COLOR_DARK_BORDER : COLOR_LIGHT_BORDER;
    COLORREF textColor = disabled
        ? BlendColor(fillNormal, g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT, g_darkMode ? 0.40f : 0.50f)
        : (g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT);

    COLORREF fillColor = pressed ? fillPressed : (hovered ? fillHover : fillNormal);
    /* smooth rounded corners: put back what is behind the button, then blend the shape over it */
    PaintButtonSurround(dis->hwndItem, dis->hDC, dis->rcItem);
    DrawRoundedAA(dis->hDC, dis->rcItem, 10, fillColor, 255, border, 1);

    wchar_t buf[160];
    GetWindowTextW(dis->hwndItem, buf, 160);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, textColor);
    HGDIOBJ oldFont = SelectObject(dis->hDC, g_fontRegular);
    RECT textRect = dis->rcItem;
    InflateRect(&textRect, -6, -2);
    /* the two confirmation "checkboxes" are multi-line buttons and need
       to wrap; DT_VCENTER only works paired with DT_SINGLELINE, so a
       wrapped label is top-aligned instead - the button's tall enough
       that this still looks centered in practice */
    LONG style = GetWindowLongW(dis->hwndItem, GWL_STYLE);
    UINT flags = (style & BS_MULTILINE) ? (DT_CENTER | DT_WORDBREAK) : (DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    const wchar_t* drawText = buf;
    /* Anton has no glyph for the checkbox squares or the back arrow, so a
       leading one of those is drawn separately in a symbol font and the
       rest of the label in the regular UI font */
    if (buf[0] == 0x2610 || buf[0] == 0x2611 || buf[0] == 0x25C0) {
        wchar_t g[2] = { buf[0], 0 };
        RECT gr = textRect;
        gr.left += 4;
        HGDIOBJ prevFont = SelectObject(dis->hDC, g_fontSymbol ? g_fontSymbol : g_fontRegular);
        DrawTextW(dis->hDC, g, -1, &gr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dis->hDC, prevFont);
        drawText = buf + 1;
        while (*drawText == L' ') drawText++;
        textRect.left += 30;
        if (style & BS_MULTILINE) flags = DT_LEFT | DT_WORDBREAK;
    }
    if ((style & BS_MULTILINE) && drawText != buf) {
        /* vertically center the wrapped checkbox label */
        RECT calc = textRect;
        DrawTextW(dis->hDC, drawText, -1, &calc, DT_LEFT | DT_WORDBREAK | DT_CALCRECT);
        int th = calc.bottom - calc.top, avail = textRect.bottom - textRect.top;
        if (th < avail) textRect.top += (avail - th) / 2;
    }
    DrawTextW(dis->hDC, drawText, -1, &textRect, flags);
    SelectObject(dis->hDC, oldFont);

    if (dis->itemState & ODS_FOCUS) {
        RECT focusRect = dis->rcItem;
        InflateRect(&focusRect, -3, -3);
        DrawFocusRect(dis->hDC, &focusRect);
    }
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
    g_lastGroupTitle = title;

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
#define MODLIST_BTN_W     72
#define MODLIST_BTN_H     26
#define MODLIST_HEADER_H  76          /* search row (40) + the type chip row below it */
#define MODLIST_CHIP_Y    44
#define MODLIST_CHIP_H    26
#define MODLIST_PAGEBAR_H 44
#define MODLIST_PAGEBTN_W 32
#define MODLIST_PAGEBTN_H 26
/* how many cards fit side by side: the grid gets an extra column every time the window
   gains room for another card (min card width ~330px) */
static int ListColsForWidth(int clientW, int pad, int gap, int minColW)
{
    int n = (clientW - 2 * pad + gap) / (minColW + gap);
    return n < 1 ? 1 : (n > 6 ? 6 : n);
}
static int g_modCols = 2;
#define MODLIST_COLS      g_modCols
#define MODLIST_COL_GAP   14

/* mod cards are laid out 2-per-row in a grid rather than one long single
   column - these two helpers are the single source of truth for that
   geometry, used identically by painting, scrolling, and hit-testing so
   they can never disagree with each other */
static int ModListColWidth(RECT clientRc)
{
    int w = (clientRc.right - MODLIST_PAD * 2 - MODLIST_COL_GAP * (MODLIST_COLS - 1)) / MODLIST_COLS;
    return w > 0 ? w : 1;
}

static RECT ModListCardRect(RECT clientRc, int gridIndex, int scrollY)
{
    int colW = ModListColWidth(clientRc);
    int col = gridIndex % MODLIST_COLS;
    int gridRow = gridIndex / MODLIST_COLS;
    RECT r;
    r.left = MODLIST_PAD + col * (colW + MODLIST_COL_GAP);
    r.right = r.left + colW;
    r.top = MODLIST_HEADER_H + gridRow * MODLIST_ROW_H - scrollY;
    r.bottom = r.top + MODLIST_ROW_H;
    return r;
}

/* Redirect (opens the mod's GameBanana page) sits above Install within
   the same button column, like Move/Delete do in the Installed Mods
   list - keeps both lists' cards feeling like the same design. */
static RECT ModListRedirectBtnRect(RECT cardRc)
{
    RECT r;
    r.right = cardRc.right - MODLIST_PAD;
    r.left = r.right - MODLIST_BTN_W;
    r.top = cardRc.top + MODLIST_PAD;
    r.bottom = r.top + MODLIST_BTN_H;
    return r;
}

static RECT ModListInstallBtnRect(RECT cardRc)
{
    RECT r = ModListRedirectBtnRect(cardRc);
    r.top = r.bottom + 6;
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
    /* stash this pick's metadata for CloseDestPicker to hand off to
       StartInstallJob once a destination folder is chosen - this is the
       one and only path that ever populates it, so ShowDestPicker/
       CloseDestPicker never see stale data from a previous pick */
    wcsncpy(g_destPickerTitle, g_modEntries[index].name, 127); g_destPickerTitle[127] = 0;
    wcsncpy(g_destPickerAuthor, g_modEntries[index].author, 63); g_destPickerAuthor[63] = 0;
    wcsncpy(g_destPickerThumbUrl, g_modEntries[index].thumbUrl, 511); g_destPickerThumbUrl[511] = 0;
    wcsncpy(g_destPickerDescription, g_modEntries[index].description, 399); g_destPickerDescription[399] = 0;
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
    case MODSORT_LIKES:
        if (eb->likeCount != ea->likeCount) return (eb->likeCount > ea->likeCount) ? 1 : -1;
        break;
    case MODSORT_VIEWS:
        if (eb->viewCount != ea->viewCount) return (eb->viewCount > ea->viewCount) ? 1 : -1;
        break;
    case MODSORT_NEWEST:
    default:
        if (eb->dateAdded > ea->dateAdded) return 1;
        if (eb->dateAdded < ea->dateAdded) return -1;
        break;
    }
    /* deterministic tiebreak. qsort isn't guaranteed stable, and
       RebuildModListView re-sorts the whole cache on every Prev/Next/
       page-number click (not just when new data arrives) - without this,
       mods tied on the active sort key (very common: lots of mods sit at
       0 likes/0 views) can land in a different relative order each time
       it re-sorts, which visibly shuffles a handful of them across the
       page boundary as you page back and forth. Comparing by modId once
       everything else ties makes repeated sorts of the same data always
       produce the same order. */
    if (ea->modId != eb->modId) return (ea->modId > eb->modId) ? 1 : -1;
    return 0;
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
        if (g_modTypeFilter != MODTYPE_ALL && (int)ModTypeFromCategory(e->category) != g_modTypeFilter) matches = FALSE;
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
        int gridRows = (rowsOnPage + MODLIST_COLS - 1) / MODLIST_COLS;
        int totalH = MODLIST_HEADER_H + gridRows * MODLIST_ROW_H;
        int visibleH = rc.bottom - MODLIST_PAGEBAR_H;
        int maxScroll = totalH - visibleH;
        if (maxScroll < 0) maxScroll = 0;
        if (g_modScrollY > maxScroll) g_modScrollY = maxScroll;
        if (g_modScrollY < 0) g_modScrollY = 0;
    }
}

/* where each type chip sits: pills flowing left to right under the search row */
static void ModTypeChipLayout(HDC hdc, RECT* out)
{
    HGDIOBJ of = SelectObject(hdc, g_fontSmall);
    int x = MODLIST_PAD + 2;
    for (int i = 0; i < MODTYPE_COUNT; i++) {
        SIZE sz;
        GetTextExtentPoint32W(hdc, g_modTypeLabels[i], (int)wcslen(g_modTypeLabels[i]), &sz);
        int w = sz.cx + 22;
        SetRect(&out[i], x, MODLIST_CHIP_Y, x + w, MODLIST_CHIP_Y + MODLIST_CHIP_H);
        x += w + 6;
    }
    SelectObject(hdc, of);
}

/* picking a chip works like changing the sort: what's cached was fetched for the previous chip, so
   throw it away and start a fresh fetch (the fetch thread keeps only mods of the chosen type) */
static void ModSetTypeFilter(HWND hwnd, int idx)
{
    if (idx < 0 || idx >= MODTYPE_COUNT || idx == g_modTypeFilter) return;
    g_modTypeFilter = idx;
    g_modCurrentPage = 0;
    g_modScrollY = 0;
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

static LRESULT CALLBACK ModListWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);

        g_hModSearchEdit = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            12, 10, 230, 28, hwnd, (HMENU)(INT_PTR)ID_MODLIST_SEARCH, hInst, NULL);
        SendMessageW(g_hModSearchEdit, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);

        g_hModSortCombo = CreateWindowW(L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            250, 10, 190, 200, hwnd, (HMENU)(INT_PTR)ID_MODLIST_SORT, hInst, NULL);
        SendMessageW(g_hModSortCombo, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);
        SendMessageW(g_hModSortCombo, CB_ADDSTRING, 0, (LPARAM)L"Sort: Newest");
        SendMessageW(g_hModSortCombo, CB_ADDSTRING, 0, (LPARAM)L"Sort: Most Liked");
        SendMessageW(g_hModSortCombo, CB_ADDSTRING, 0, (LPARAM)L"Sort: Most Viewed");
        SendMessageW(g_hModSortCombo, CB_SETCURSEL, (WPARAM)g_modSortMode, 0);
        SetWindowTheme(g_hModSortCombo, g_darkMode ? L"DarkMode_Explorer" : L"Explorer", NULL);

        MakeButton(hwnd, hInst, L"Download From Link", 448, 8, 160, 32, ID_MODLIST_DOWNLOAD_LINK, g_fontSmall);
        return 0;
    }

    case WM_DRAWITEM:
        /* the "Download From Link" button above is a real owner-draw
           button (via MakeButton) - unlike this window's mod-card
           Install/Redirect "buttons", which are just hand-painted
           rectangles inside WM_PAINT below, a BS_OWNERDRAW control
           paints nothing on its own and relies entirely on its parent
           handling WM_DRAWITEM. Without this case the button is a real,
           fully clickable window that's simply never painted - which is
           exactly what made it look like it didn't exist. */
        if (((LPDRAWITEMSTRUCT)lParam)->CtlType == ODT_BUTTON) {
            DrawThemedOwnerButton((LPDRAWITEMSTRUCT)lParam);
            return TRUE;
        }
        return FALSE;

    case WM_SIZE: {
        RECT rc; GetClientRect(hwnd, &rc);
        g_modCols = ListColsForWidth(rc.right, MODLIST_PAD, MODLIST_COL_GAP, 330);
        int pageStart = g_modCurrentPage * MODLIST_PAGE_SIZE;
        int pageEnd = pageStart + MODLIST_PAGE_SIZE;
        if (pageEnd > g_modFilteredCount) pageEnd = g_modFilteredCount;
        int rowsOnPage = pageEnd - pageStart;
        if (rowsOnPage < 0) rowsOnPage = 0;
        int gridRows = (rowsOnPage + MODLIST_COLS - 1) / MODLIST_COLS;
        int maxScroll = MODLIST_HEADER_H + gridRows * MODLIST_ROW_H - (rc.bottom - MODLIST_PAGEBAR_H);
        if (maxScroll < 0) maxScroll = 0;
        if (g_modScrollY > maxScroll) g_modScrollY = maxScroll;
        InvalidateRect(hwnd, NULL, FALSE);
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
            RECT card = ModListCardRect(rc, ri, g_modScrollY);
            if (card.bottom < MODLIST_HEADER_H || card.top > contentBottom) continue;

            ModEntry* e = &g_modEntries[g_modFilteredIndices[i]];
            RECT thumbRc = { card.left + MODLIST_PAD, card.top + MODLIST_PAD,
                              card.left + MODLIST_PAD + MODLIST_THUMB, card.top + MODLIST_PAD + MODLIST_THUMB };
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

            RECT textRc = { thumbRc.right + MODLIST_PAD, card.top + MODLIST_PAD,
                             card.right - MODLIST_PAD - MODLIST_BTN_W - MODLIST_PAD, card.top + MODLIST_PAD + 20 };
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

            BOOL hasLink = (e->profileUrl[0] != 0);
            RECT redirectRc = ModListRedirectBtnRect(card);
            COLORREF redirectPenColor = hasLink ? textColor : RGB(140, 140, 140);
            HBRUSH redirectBrush = CreateSolidBrush(g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL);
            HPEN redirectPen = CreatePen(PS_SOLID, 1, redirectPenColor);
            HGDIOBJ oldRedirBrush = SelectObject(hdc, redirectBrush);
            HGDIOBJ oldRedirPen = SelectObject(hdc, redirectPen);
            RoundRect(hdc, redirectRc.left, redirectRc.top, redirectRc.right, redirectRc.bottom, 6, 6);
            SelectObject(hdc, oldRedirBrush);
            SelectObject(hdc, oldRedirPen);
            DeleteObject(redirectBrush);
            DeleteObject(redirectPen);
            SetTextColor(hdc, redirectPenColor);
            SelectObject(hdc, g_fontSmall);
            DrawTextW(hdc, L"Redirect", -1, &redirectRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            RECT btnRc = ModListInstallBtnRect(card);
            HBRUSH btnBrush = CreateSolidBrush(g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL);
            HPEN btnPen = CreatePen(PS_SOLID, 1, textColor);
            HGDIOBJ oldBtnBrush = SelectObject(hdc, btnBrush);
            HGDIOBJ oldBtnPen = SelectObject(hdc, btnPen);
            RoundRect(hdc, btnRc.left, btnRc.top, btnRc.right, btnRc.bottom, 6, 6);
            SelectObject(hdc, oldBtnBrush);
            SelectObject(hdc, oldBtnPen);
            DeleteObject(btnBrush);
            DeleteObject(btnPen);
            SetTextColor(hdc, textColor);
            SelectObject(hdc, g_fontSmall);
            DrawTextW(hdc, L"Install", -1, &btnRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            /* one separator per grid row (drawn once, from the left
               card), spanning the full width under both columns */
            if (ri % MODLIST_COLS == 0) {
                HPEN sepPen = CreatePen(PS_SOLID, 1, SEP_COLOR);
                HGDIOBJ oldSepPen = SelectObject(hdc, sepPen);
                MoveToEx(hdc, MODLIST_PAD, card.bottom, NULL);
                LineTo(hdc, rc.right - MODLIST_PAD, card.bottom);
                SelectObject(hdc, oldSepPen);
                DeleteObject(sepPen);
            }
        }

        DeleteDC(memDC);

        /* empty-state messaging, drawn in the row area (not the page bar) */
        BOOL lookingForType = (g_modTypeFilter != MODTYPE_ALL && g_modCount == 0 && g_modLoadState != MODLOAD_ERROR &&
                               (g_modLoadState == MODLOAD_LOADING || g_modHasMore));
        if (lookingForType) {
            /* nothing of this type yet, but the feed hasn't been read to the end - say so instead of "none found" */
            RECT emptyRc = { 20, MODLIST_HEADER_H, rc.right - 20, contentBottom };
            SetTextColor(hdc, textColor);
            SelectObject(hdc, g_fontRegular);
            wchar_t msg[300];
            if (g_modLoadState != MODLOAD_LOADING && g_modAutoLeft == 0)
                wsprintfW(msg, L"No %s mods found in the %ld newest mods checked so far.\nPress Next to keep looking.",
                    g_modTypeLabels[g_modTypeFilter], (long)g_modScanned);
            else
                wsprintfW(msg, L"Looking for %s mods\u2026\n%ld mods checked so far",
                    g_modTypeLabels[g_modTypeFilter], (long)g_modScanned);
            DrawTextW(hdc, msg, -1, &emptyRc, DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
        } else if (g_modLoadState != MODLOAD_LOADING && g_modLoadState != MODLOAD_ERROR) {
            RECT emptyRc = { 20, MODLIST_HEADER_H, rc.right - 20, contentBottom };
            SetTextColor(hdc, textColor);
            SelectObject(hdc, g_fontRegular);
            if (g_modCount == 0) {
                const wchar_t* zeroMsg = L"No mods found for this game yet.";
                if (g_modTypeFilter != MODTYPE_ALL)
                    zeroMsg = (g_subfeedState == 2 && g_gameFilterMode == 2)
                        ? L"Only the Dub Mode category could be loaded here, so this type can't be listed."
                        : L"No mods of this type were found.";
                DrawTextW(hdc, zeroMsg, -1, &emptyRc, DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
            } else if (g_modFilteredCount == 0) {
                DrawTextW(hdc, L"No mods match your search.", -1, &emptyRc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }

        /* fixed header: hide anything that scrolled up under it, then draw the type chips */
        {
            RECT hdr = { 0, 0, rc.right, MODLIST_HEADER_H };
            HBRUSH hb = CreateSolidBrush(g_darkMode ? COLOR_DARK_BG : COLOR_LIGHT_BG);
            FillRect(hdc, &hdr, hb);
            DeleteObject(hb);
            RECT chips[MODTYPE_COUNT];
            ModTypeChipLayout(hdc, chips);
            HGDIOBJ chipFont = SelectObject(hdc, g_fontSmall);
            for (int i = 0; i < MODTYPE_COUNT; i++) {
                BOOL sel = (i == g_modTypeFilter);
                COLORREF fill = sel ? THEME_ACCENT : (g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL);
                COLORREF border = sel ? THEME_ACCENT : (g_darkMode ? COLOR_DARK_BORDER : COLOR_LIGHT_BORDER);
                DrawRoundedAA(hdc, chips[i], MODLIST_CHIP_H / 2, fill, 255, border, 1);
                SetTextColor(hdc, sel ? (g_darkMode ? RGB(18, 18, 22) : RGB(255, 255, 255)) : textColor);
                DrawTextW(hdc, g_modTypeLabels[i], -1, &chips[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            SelectObject(hdc, chipFont);
        }

        /* page bar - fixed at the bottom, not part of the scrolling area */
        RECT barRc = { 0, contentBottom, rc.right, rc.bottom };
        HBRUSH barBg = CreateSolidBrush(g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL);
        FillRect(hdc, &barRc, barBg);
        DeleteObject(barBg);
        HPEN barLinePen = CreatePen(PS_SOLID, 1, SEP_COLOR);
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
            RoundRect(hdc, lay.prevRc.left, lay.prevRc.top, lay.prevRc.right, lay.prevRc.bottom, 6, 6);
            DrawTextW(hdc, L"< Prev", -1, &lay.prevRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            for (int i = 0; i < lay.visibleCount; i++) {
                BOOL isCurrent = (lay.pageNum[i] == g_modCurrentPage);
                SetTextColor(hdc, textColor);
                if (isCurrent) {
                    HBRUSH curBg = CreateSolidBrush(g_darkMode ? COLOR_DARK_BG : COLOR_LIGHT_BG);
                    HGDIOBJ oldCurBrush = SelectObject(hdc, curBg);
                    RoundRect(hdc, lay.pageRc[i].left, lay.pageRc[i].top, lay.pageRc[i].right, lay.pageRc[i].bottom, 6, 6);
                    SelectObject(hdc, oldCurBrush);
                    DeleteObject(curBg);
                } else {
                    RoundRect(hdc, lay.pageRc[i].left, lay.pageRc[i].top, lay.pageRc[i].right, lay.pageRc[i].bottom, 6, 6);
                }
                wchar_t num[8];
                wsprintfW(num, L"%d", lay.pageNum[i] + 1);
                DrawTextW(hdc, num, -1, &lay.pageRc[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            SetTextColor(hdc, canNext ? textColor : RGB(140, 140, 140));
            RoundRect(hdc, lay.nextRc.left, lay.nextRc.top, lay.nextRc.right, lay.nextRc.bottom, 6, 6);
            DrawTextW(hdc, L"Next >", -1, &lay.nextRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdc, oldCtrlBrush);
            SelectObject(hdc, oldCtrlPen);
            DeleteObject(ctrlPen);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT cp; GetCursorPos(&cp);
            ScreenToClient(hwnd, &cp);
            if (cp.y >= MODLIST_CHIP_Y && cp.y < MODLIST_CHIP_Y + MODLIST_CHIP_H) {
                HDC cdc = GetDC(hwnd);
                RECT chips[MODTYPE_COUNT];
                ModTypeChipLayout(cdc, chips);
                ReleaseDC(hwnd, cdc);
                for (int i = 0; i < MODTYPE_COUNT; i++) {
                    if (cp.x >= chips[i].left && cp.x < chips[i].right) { SetCursor(LoadCursorW(NULL, IDC_HAND)); return TRUE; }
                }
            }
        }
        break;

    case WM_LBUTTONDOWN: {
        int mouseX = GET_X_LPARAM(lParam);
        int mouseY = GET_Y_LPARAM(lParam);
        RECT rc; GetClientRect(hwnd, &rc);
        int contentBottom = rc.bottom - MODLIST_PAGEBAR_H;

        if (mouseY >= MODLIST_CHIP_Y && mouseY < MODLIST_CHIP_Y + MODLIST_CHIP_H) {
            HDC cdc = GetDC(hwnd);
            RECT chips[MODTYPE_COUNT];
            ModTypeChipLayout(cdc, chips);
            ReleaseDC(hwnd, cdc);
            for (int i = 0; i < MODTYPE_COUNT; i++) {
                if (mouseX >= chips[i].left && mouseX < chips[i].right) { ModSetTypeFilter(hwnd, i); break; }
            }
            return 0;
        }

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

        for (int ri = 0; ri < rowsOnPage; ri++) {
            RECT card = ModListCardRect(rc, ri, g_modScrollY);
            if (mouseX < card.left || mouseX > card.right || mouseY < card.top || mouseY > card.bottom) continue;

            ModEntry* e = &g_modEntries[g_modFilteredIndices[pageStart + ri]];
            RECT redirectRc = ModListRedirectBtnRect(card);
            RECT installRc = ModListInstallBtnRect(card);
            POINT pt = { mouseX, mouseY };
            if (PtInRect(&redirectRc, pt)) {
                if (e->profileUrl[0]) ShellExecuteW(NULL, L"open", e->profileUrl, NULL, NULL, SW_SHOWNORMAL);
            } else if (PtInRect(&installRc, pt)) {
                ModListDoInstall(hwnd, g_modFilteredIndices[pageStart + ri]);
            }
            break;
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

        int gridRows = (rowsOnPage + MODLIST_COLS - 1) / MODLIST_COLS;
        int totalH = MODLIST_HEADER_H + gridRows * MODLIST_ROW_H;
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
        if (LOWORD(wParam) == ID_MODLIST_DOWNLOAD_LINK) {
            ShowDownloadFromLinkPicker(GetParent(hwnd), g_hInstance);
            return 0;
        }
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

            /* Every sort mode is a genuinely different server-side
               ordering (or, for Newest, the default order) - what's
               cached was fetched under whichever mode was active
               before, so it's the top N of THAT ordering, not a
               representative sample of the new one. Re-sorting it
               locally (e.g. "newest among an arbitrary most-liked-
               biased sample") isn't the same thing as actually asking
               for newest, and leaves g_modNextPage pointing at a page
               sequence from the OLD query, corrupting pagination for
               the new one. So any sort change clears the cache and
               starts a completely fresh fetch - not just Likes/Views. */
            for (int i = 0; i < g_modCount; i++) {
                if (g_modEntries[i].thumb) DeleteObject(g_modEntries[i].thumb);
            }
            g_modCount = 0;
            g_modFilteredCount = 0;
            g_modNextPage = 1;
            g_modPendingAdvance = FALSE;
            StartModFetch(g_hModListWnd, g_modNextPage, g_modSortMode, g_modSearchQuery);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;

    case WM_APP_MODS_PROGRESS:
        if (g_modLoadState == MODLOAD_LOADING || g_modCount == 0) InvalidateRect(hwnd, NULL, FALSE);
        return 0;

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
                /* a type chip with too few results so far: keep reading the feed in the background */
                if (g_modTypeFilter != MODTYPE_ALL && g_modHasMore && g_modFilteredCount < MODLIST_PAGE_SIZE &&
                    g_modCount < MAX_MOD_ENTRIES - MODS_PER_PAGE && g_modAutoLeft > 0) {
                    g_modAutoLeft--;
                    g_modAutoFetch = TRUE;
                    StartModFetch(hwnd, g_modNextPage, g_modSortMode, g_modSearchQuery);
                }
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
    return CreateWindowExW(0, CLASS_NAME, NULL,
        WS_CHILD | WS_CLIPCHILDREN,
        x, y, w, h, parent, NULL, hInst, NULL);
}

/* ===================== Installed Mods tab =====================
   Lists everything currently sitting in the 7 pack_### folders under the
   game's AppData dir, grouped by folder, with size/sort/delete/move. A
   mod installed through the Browse Mods list's Install button gets its
   title/author/coverphoto/description/link recorded in mod_meta.ini (see
   SaveModMetaRecord, called from InstallJobThreadProc) so its card here
   matches what Browse Mods showed; anything else (mods dropped in by
   hand) instead reads icon.png/info.ini out of its own folder, if
   present, and falls back to the bare folder name otherwise. */

/* ---- mod_meta.ini: title/author/coverphoto/description/link, keyed by
   "<packFolder>\<folderName>" ---- */

#define MAX_META_RECORDS 256

typedef struct {
    wchar_t key[256];
    wchar_t title[128];
    wchar_t author[64];
    wchar_t thumbUrl[512];
    wchar_t profileUrl[160];
    wchar_t description[400];
    long installedUnix;
} ModMetaRecord;

static void GetModMetaPath(wchar_t* out)
{
    wchar_t exeDir[MAX_PATH];
    GetExeDir(exeDir, MAX_PATH);
    wsprintfW(out, L"%s\\mod_meta.ini", exeDir);
}

static void SanitizeSingleLineW(wchar_t* s)
{
    for (wchar_t* p = s; *p; p++) {
        if (*p == L'\r' || *p == L'\n') *p = L' ';
    }
}

/* parses the whole file into an array of records (one per [section]),
   returns how many were found */
static int LoadModMetaAll(ModMetaRecord* out, int outCap)
{
    int n = 0;
    wchar_t path[MAX_PATH];
    GetModMetaPath(path);
    DWORD size = 0;
    BYTE* buf = ReadEntireFileW(path, &size);
    if (!buf) return 0;

    wchar_t* text = (wchar_t*)malloc(((size_t)size + 1) * sizeof(wchar_t));
    if (!text) { free(buf); return 0; }
    BOOL ok = DecodeUtf8ToWideBuf(buf, size, text, (int)size + 1);
    free(buf);
    if (!ok) { free(text); return 0; }

    ModMetaRecord* cur = NULL;
    wchar_t* p = text;
    while (*p && n < outCap) {
        wchar_t* lineStart = p;
        while (*p && *p != L'\n') p++;
        wchar_t* lineEnd = p;
        if (*p == L'\n') p++;
        while (lineEnd > lineStart && (lineEnd[-1] == L'\r' || lineEnd[-1] == L' ')) lineEnd--;
        *lineEnd = 0;

        if (lineStart[0] == L'[') {
            wchar_t* close = wcsrchr(lineStart, L']');
            if (close) *close = 0;
            cur = &out[n++];
            memset(cur, 0, sizeof(*cur));
            wcsncpy(cur->key, lineStart + 1, 255); cur->key[255] = 0;
        } else if (cur) {
            if (_wcsnicmp(lineStart, L"Title=", 6) == 0) {
                wcsncpy(cur->title, lineStart + 6, 127); cur->title[127] = 0;
            } else if (_wcsnicmp(lineStart, L"Author=", 7) == 0) {
                wcsncpy(cur->author, lineStart + 7, 63); cur->author[63] = 0;
            } else if (_wcsnicmp(lineStart, L"ThumbUrl=", 9) == 0) {
                wcsncpy(cur->thumbUrl, lineStart + 9, 511); cur->thumbUrl[511] = 0;
            } else if (_wcsnicmp(lineStart, L"ProfileUrl=", 11) == 0) {
                wcsncpy(cur->profileUrl, lineStart + 11, 159); cur->profileUrl[159] = 0;
            } else if (_wcsnicmp(lineStart, L"Description=", 12) == 0) {
                wcsncpy(cur->description, lineStart + 12, 399); cur->description[399] = 0;
            } else if (_wcsnicmp(lineStart, L"Installed=", 10) == 0) {
                cur->installedUnix = _wtol(lineStart + 10);
            }
        }
    }
    free(text);
    return n;
}

static void SaveModMetaAll(const ModMetaRecord* records, int count)
{
    wchar_t path[MAX_PATH];
    GetModMetaPath(path);

    /* generous fixed headroom per record rather than measuring exactly -
       the longest field (Description) is already capped at 400 wchars */
    size_t cap = (size_t)count * 1400 + 64;
    wchar_t* content = (wchar_t*)malloc(cap * sizeof(wchar_t));
    if (!content) return;
    content[0] = 0;
    size_t used = 0;

    for (int i = 0; i < count; i++) {
        wchar_t buf[1400];
        wsprintfW(buf,
            L"[%s]\r\nTitle=%s\r\nAuthor=%s\r\nThumbUrl=%s\r\nProfileUrl=%s\r\nDescription=%s\r\nInstalled=%ld\r\n",
            records[i].key, records[i].title, records[i].author, records[i].thumbUrl,
            records[i].profileUrl, records[i].description, records[i].installedUnix);
        size_t bl = wcslen(buf);
        if (used + bl < cap) {
            wcscpy(content + used, buf);
            used += bl;
        }
    }

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
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
    free(content);
}

static void SaveModMetaRecord(const wchar_t* pathKey, const wchar_t* title, const wchar_t* author,
    const wchar_t* thumbUrl, const wchar_t* profileUrl, const wchar_t* description)
{
    ModMetaRecord* all = (ModMetaRecord*)malloc(sizeof(ModMetaRecord) * MAX_META_RECORDS);
    if (!all) return;
    int n = LoadModMetaAll(all, MAX_META_RECORDS);

    int idx = -1;
    for (int i = 0; i < n; i++) {
        if (_wcsicmp(all[i].key, pathKey) == 0) { idx = i; break; }
    }
    if (idx < 0 && n < MAX_META_RECORDS) idx = n++;
    if (idx < 0) { free(all); return; } /* full - drop silently rather than crash */

    ModMetaRecord* r = &all[idx];
    memset(r, 0, sizeof(*r));
    wcsncpy(r->key, pathKey, 255); r->key[255] = 0;
    if (title) { wcsncpy(r->title, title, 127); r->title[127] = 0; }
    if (author) { wcsncpy(r->author, author, 63); r->author[63] = 0; }
    if (thumbUrl) { wcsncpy(r->thumbUrl, thumbUrl, 511); r->thumbUrl[511] = 0; }
    if (profileUrl) { wcsncpy(r->profileUrl, profileUrl, 159); r->profileUrl[159] = 0; }
    if (description) { wcsncpy(r->description, description, 399); r->description[399] = 0; }
    SanitizeSingleLineW(r->title);
    SanitizeSingleLineW(r->author);
    SanitizeSingleLineW(r->description);
    r->installedUnix = (long)time(NULL);

    SaveModMetaAll(all, n);
    free(all);
}

static void DeleteModMetaRecord(const wchar_t* pathKey)
{
    ModMetaRecord* all = (ModMetaRecord*)malloc(sizeof(ModMetaRecord) * MAX_META_RECORDS);
    if (!all) return;
    int n = LoadModMetaAll(all, MAX_META_RECORDS);
    int idx = -1;
    for (int i = 0; i < n; i++) {
        if (_wcsicmp(all[i].key, pathKey) == 0) { idx = i; break; }
    }
    if (idx >= 0) {
        for (int i = idx; i < n - 1; i++) all[i] = all[i + 1];
        n--;
        SaveModMetaAll(all, n);
    }
    free(all);
}

static void RenameModMetaRecordKey(const wchar_t* oldKey, const wchar_t* newKey)
{
    ModMetaRecord* all = (ModMetaRecord*)malloc(sizeof(ModMetaRecord) * MAX_META_RECORDS);
    if (!all) return;
    int n = LoadModMetaAll(all, MAX_META_RECORDS);
    BOOL found = FALSE;
    for (int i = 0; i < n; i++) {
        if (_wcsicmp(all[i].key, oldKey) == 0) {
            wcsncpy(all[i].key, newKey, 255); all[i].key[255] = 0;
            found = TRUE;
            break;
        }
    }
    if (found) SaveModMetaAll(all, n);
    free(all);
}

/* ---- local mod folder info: icon.png + info.ini (Title=/Subtitle=/
   Author=), for mods that have no mod_meta.ini entry ---- */

static void ReadLocalModInfo(const wchar_t* modDir, wchar_t* outTitle, wchar_t* outSubtitle, wchar_t* outAuthor)
{
    /* outTitle must be >=128, outSubtitle >=192, outAuthor >=128 wchars */
    outTitle[0] = 0; outSubtitle[0] = 0; outAuthor[0] = 0;
    wchar_t iniPath[MAX_PATH];
    wsprintfW(iniPath, L"%s\\info.ini", modDir);
    DWORD size = 0;
    BYTE* buf = ReadEntireFileW(iniPath, &size);
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

        if (_wcsnicmp(lineStart, L"Title=", 6) == 0) {
            wcsncpy(outTitle, lineStart + 6, 127); outTitle[127] = 0;
        } else if (_wcsnicmp(lineStart, L"Subtitle=", 9) == 0) {
            wcsncpy(outSubtitle, lineStart + 9, 191); outSubtitle[191] = 0;
        } else if (_wcsnicmp(lineStart, L"Author=", 7) == 0) {
            wcsncpy(outAuthor, lineStart + 7, 127); outAuthor[127] = 0;
        }
    }
    free(text);
}

/* ---- folder size / date-added helpers ---- */

static ULONGLONG GetDirSizeRecursiveW(const wchar_t* path)
{
    ULONGLONG total = 0;
    wchar_t search[MAX_PATH];
    wsprintfW(search, L"%s\\*", path);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        wchar_t full[MAX_PATH];
        wsprintfW(full, L"%s\\%s", path, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            total += GetDirSizeRecursiveW(full);
        } else {
            total += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

static long FileTimeToUnixTime(const FILETIME* ft)
{
    ULONGLONG t = ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    return (long)((t / 10000000ULL) - 11644473600ULL);
}

static long GetDirCreatedUnixTime(const wchar_t* path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return 0;
    return FileTimeToUnixTime(&fad.ftCreationTime);
}

static void UInt64ToStrW(ULONGLONG v, wchar_t* out)
{
    wchar_t tmp[24];
    int i = 0;
    if (v == 0) { out[0] = L'0'; out[1] = 0; return; }
    while (v > 0 && i < 23) { tmp[i++] = (wchar_t)(L'0' + (v % 10)); v /= 10; }
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = 0;
}

/* wsprintfW has no floating-point format specifier on this platform, so
   size/fraction math is done by hand rather than with %f */
static void FormatSizeShort(ULONGLONG bytes, wchar_t* out)
{
    wchar_t numStr[24];
    if (bytes >= 1073741824ULL) {
        ULONGLONG whole = bytes / 1073741824ULL;
        int frac = (int)(((bytes % 1073741824ULL) * 10) / 1073741824ULL);
        UInt64ToStrW(whole, numStr);
        wsprintfW(out, L"%s.%d GB", numStr, frac);
    } else if (bytes >= 1048576ULL) {
        ULONGLONG whole = bytes / 1048576ULL;
        int frac = (int)(((bytes % 1048576ULL) * 10) / 1048576ULL);
        UInt64ToStrW(whole, numStr);
        wsprintfW(out, L"%s.%d MB", numStr, frac);
    } else if (bytes >= 1024ULL) {
        UInt64ToStrW(bytes / 1024ULL, numStr);
        wsprintfW(out, L"%s KB", numStr);
    } else {
        UInt64ToStrW(bytes, numStr);
        wsprintfW(out, L"%s bytes", numStr);
    }
}

static void FormatUnixDateShort(long unixTime, wchar_t* out)
{
    static const wchar_t* MONTHS[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                        L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
    if (unixTime <= 0) { wcscpy(out, L"unknown date"); return; }
    ULONGLONG t = ((ULONGLONG)(unsigned long)unixTime + 11644473600ULL) * 10000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)(t & 0xFFFFFFFFULL);
    ft.dwHighDateTime = (DWORD)(t >> 32);
    FILETIME lft;
    FileTimeToLocalFileTime(&ft, &lft);
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&lft, &st)) { wcscpy(out, L"unknown date"); return; }
    int mi = (int)st.wMonth - 1;
    if (mi < 0 || mi > 11) mi = 0;
    wsprintfW(out, L"%s %d, %d", MONTHS[mi], (int)st.wDay, (int)st.wYear);
}

/* ---- installed-mods list state ----
   A directory directly under a pack folder counts as a MOD only if it
   has actual files sitting directly inside it (matching how mods land
   after an install - see BUILD.txt/InstallJobThreadProc). A directory
   that instead only has subfolders (no loose files of its own) is a
   user-made organizational FOLDER, and is recursed into so whatever
   mods/folders are nested inside it get found too. The list view lets
   you drill into those folders like a small file browser (see the
   navigation stack below) rather than flattening everything together. */

typedef enum { INSTSRC_LOCAL = 0, INSTSRC_BROWSER } InstModSource;
typedef enum { INSTSORT_NAME = 0, INSTSORT_SIZE, INSTSORT_DATE } InstSortMode;
typedef enum { INSTLOAD_IDLE, INSTLOAD_LOADING, INSTLOAD_LOADED } InstLoadState;
typedef enum { INSTROW_HEADER = 0, INSTROW_MOD = 1, INSTROW_FOLDER = 2 } InstRowKind;

#define MAX_INSTALLED_MODS    300
#define MAX_INSTALLED_FOLDERS 200
#define MAX_NAV_DEPTH 16
#define WM_APP_INSTALLED_LOADED (WM_APP + 4)

typedef struct {
    wchar_t packFolder[64];
    wchar_t parentPath[MAX_PATH]; /* the directory this mod lives directly inside */
    wchar_t folderName[160];
    wchar_t fullPath[MAX_PATH];
    wchar_t title[128];
    wchar_t subtitle[192];  /* local: info.ini Subtitle=; browser: 1st line of the GB description */
    wchar_t author[128];
    wchar_t profileUrl[160];
    wchar_t thumbUrl[512];
    ULONGLONG sizeBytes;
    long dateAddedUnix;
    InstModSource source;
    HBITMAP thumb;
} InstalledModEntry;

typedef struct {
    wchar_t packFolder[64];
    wchar_t parentPath[MAX_PATH]; /* the directory this folder lives directly inside */
    wchar_t folderName[160];
    wchar_t fullPath[MAX_PATH];
    ULONGLONG sizeBytes;  /* recursive total of everything nested inside */
    int modCount;         /* recursive count of actual mods nested inside */
    long dateAddedUnix;
} InstalledFolderEntry;

static InstalledModEntry g_instEntries[MAX_INSTALLED_MODS];
static int g_instCount = 0;
static InstalledFolderEntry g_instFolders[MAX_INSTALLED_FOLDERS];
static int g_instFolderCount = 0;

static InstSortMode g_instSortMode = INSTSORT_NAME;
static HWND g_hInstSortCombo = NULL;
static HWND g_hInstBreadcrumb = NULL;
static HWND g_hInstBackBtn = NULL;
static HWND g_hInstListWnd = NULL;
static int g_instScrollY = 0;
static InstLoadState g_instLoadState = INSTLOAD_IDLE;

/* navigation: depth 0 = top-level overview (grouped by all 7 pack
   folders); depth > 0 = browsing inside a specific folder, one level
   per stack entry */
static wchar_t g_instNavStack[MAX_NAV_DEPTH][MAX_PATH];
static wchar_t g_instNavNames[MAX_NAV_DEPTH][160];
static wchar_t g_instNavPack[64] = L"";
static int g_instNavDepth = 0;

#define ID_INSTLIST_SORT    5012
#define ID_INSTLIST_REFRESH 5013
#define ID_INSTLIST_BACK    5014
#define INSTLIST_ROW_H     104
#define INSTLIST_THUMB     64
#define INSTLIST_PAD       10
#define INSTLIST_HEADER_H  78
#define INSTLIST_GROUP_H   40
#define INSTLIST_BTN_W     72
#define INSTLIST_BTN_H     26
static int g_instCols = 2;
#define INSTLIST_COLS      g_instCols
#define INSTLIST_COL_GAP   14

/* flattened row list (group headers + mod rows + folder rows), rebuilt
   whenever entries/sort/navigation change. Mod/folder rows sit 2-per-row
   in a grid like Browse Mods - yOffset is the shared top of that grid
   row (both columns of one grid row have the same yOffset), gridCol says
   which of the two columns this particular entry occupies. Header rows
   span the full width, so gridCol is unused (0) for them, and each new
   header resets the grid back to column 0. */
typedef struct {
    InstRowKind kind;
    int packIdx;   /* index into PACK_FOLDERS - headers and top-level rows only */
    int entryIdx;  /* index into g_instEntries (MOD) or g_instFolders (FOLDER) */
    int yOffset;   /* top of this row within the scrollable content, before scroll subtraction */
    int gridCol;   /* 0 or 1 for MOD/FOLDER rows */
} InstRow;
#define MAX_INST_ROWS (NUM_PACK_FOLDERS + MAX_INSTALLED_MODS + MAX_INSTALLED_FOLDERS)
static InstRow g_instRows[MAX_INST_ROWS];
static int g_instRowCount = 0;
static int g_instContentH = 0; /* total scrollable content height, computed alongside the rows */

static InstSortMode g_instSortModeForCompare = INSTSORT_NAME;

static int InstalledModIndexCompare(const void* a, const void* b)
{
    int ia = *(const int*)a, ib = *(const int*)b;
    const InstalledModEntry* ea = &g_instEntries[ia];
    const InstalledModEntry* eb = &g_instEntries[ib];
    switch (g_instSortModeForCompare) {
    case INSTSORT_SIZE:
        if (eb->sizeBytes != ea->sizeBytes) return (eb->sizeBytes > ea->sizeBytes) ? 1 : -1;
        break;
    case INSTSORT_DATE:
        if (eb->dateAddedUnix != ea->dateAddedUnix) return (eb->dateAddedUnix > ea->dateAddedUnix) ? 1 : -1;
        break;
    case INSTSORT_NAME:
    default:
        break;
    }
    return _wcsicmp(ea->title[0] ? ea->title : ea->folderName, eb->title[0] ? eb->title : eb->folderName);
}

static int InstalledFolderIndexCompare(const void* a, const void* b)
{
    int ia = *(const int*)a, ib = *(const int*)b;
    const InstalledFolderEntry* fa = &g_instFolders[ia];
    const InstalledFolderEntry* fb = &g_instFolders[ib];
    switch (g_instSortModeForCompare) {
    case INSTSORT_SIZE:
        if (fb->sizeBytes != fa->sizeBytes) return (fb->sizeBytes > fa->sizeBytes) ? 1 : -1;
        break;
    case INSTSORT_DATE:
        if (fb->dateAddedUnix != fa->dateAddedUnix) return (fb->dateAddedUnix > fa->dateAddedUnix) ? 1 : -1;
        break;
    default:
        break;
    }
    return _wcsicmp(fa->folderName, fb->folderName);
}

/* builds the currently-visible row list: the top-level overview (grouped
   by pack folder, direct children only) when the nav stack is empty, or
   a flat listing of whatever folder is currently open otherwise. Folders
   are always listed before mods within a section, like a normal file
   browser. */
static void RebuildInstalledRows(void)
{
    g_instRowCount = 0;
    g_instSortModeForCompare = g_instSortMode;
    int yCursor = 0;

    if (g_instNavDepth == 0) {
        wchar_t gameDir[MAX_PATH];
        BOOL haveGameDir = GetAppDataGameDir(gameDir);

        for (int pi = 0; pi < NUM_PACK_FOLDERS; pi++) {
            wchar_t packRoot[MAX_PATH] = L"";
            if (haveGameDir) wsprintfW(packRoot, L"%s\\%s", gameDir, PACK_FOLDERS[pi]);

            int folderIdx[MAX_INSTALLED_FOLDERS]; int folderIdxCount = 0;
            for (int i = 0; i < g_instFolderCount; i++) {
                if (_wcsicmp(g_instFolders[i].packFolder, PACK_FOLDERS[pi]) == 0 &&
                    _wcsicmp(g_instFolders[i].parentPath, packRoot) == 0) {
                    folderIdx[folderIdxCount++] = i;
                }
            }
            int modIdx[MAX_INSTALLED_MODS]; int modIdxCount = 0;
            for (int i = 0; i < g_instCount; i++) {
                if (_wcsicmp(g_instEntries[i].packFolder, PACK_FOLDERS[pi]) == 0 &&
                    _wcsicmp(g_instEntries[i].parentPath, packRoot) == 0) {
                    modIdx[modIdxCount++] = i;
                }
            }
            if (folderIdxCount == 0 && modIdxCount == 0) continue; /* hide empty packs */

            if (g_instRowCount < MAX_INST_ROWS) {
                g_instRows[g_instRowCount].kind = INSTROW_HEADER;
                g_instRows[g_instRowCount].packIdx = pi;
                g_instRows[g_instRowCount].entryIdx = -1;
                g_instRows[g_instRowCount].yOffset = yCursor;
                g_instRows[g_instRowCount].gridCol = 0;
                g_instRowCount++;
            }
            yCursor += INSTLIST_GROUP_H;

            int gridCounter = 0;
            if (folderIdxCount > 1) qsort(folderIdx, (size_t)folderIdxCount, sizeof(int), InstalledFolderIndexCompare);
            for (int k = 0; k < folderIdxCount && g_instRowCount < MAX_INST_ROWS; k++) {
                g_instRows[g_instRowCount].kind = INSTROW_FOLDER;
                g_instRows[g_instRowCount].packIdx = pi;
                g_instRows[g_instRowCount].entryIdx = folderIdx[k];
                g_instRows[g_instRowCount].gridCol = gridCounter % INSTLIST_COLS;
                g_instRows[g_instRowCount].yOffset = yCursor + (gridCounter / INSTLIST_COLS) * INSTLIST_ROW_H;
                g_instRowCount++;
                gridCounter++;
            }
            if (modIdxCount > 1) qsort(modIdx, (size_t)modIdxCount, sizeof(int), InstalledModIndexCompare);
            for (int k = 0; k < modIdxCount && g_instRowCount < MAX_INST_ROWS; k++) {
                g_instRows[g_instRowCount].kind = INSTROW_MOD;
                g_instRows[g_instRowCount].packIdx = pi;
                g_instRows[g_instRowCount].entryIdx = modIdx[k];
                g_instRows[g_instRowCount].gridCol = gridCounter % INSTLIST_COLS;
                g_instRows[g_instRowCount].yOffset = yCursor + (gridCounter / INSTLIST_COLS) * INSTLIST_ROW_H;
                g_instRowCount++;
                gridCounter++;
            }
            int gridRowsUsed = (gridCounter + INSTLIST_COLS - 1) / INSTLIST_COLS;
            yCursor += gridRowsUsed * INSTLIST_ROW_H;
        }
    } else {
        const wchar_t* curDir = g_instNavStack[g_instNavDepth - 1];
        int pi = -1;
        for (int i = 0; i < NUM_PACK_FOLDERS; i++) {
            if (_wcsicmp(PACK_FOLDERS[i], g_instNavPack) == 0) { pi = i; break; }
        }

        int folderIdx[MAX_INSTALLED_FOLDERS]; int folderIdxCount = 0;
        for (int i = 0; i < g_instFolderCount; i++) {
            if (_wcsicmp(g_instFolders[i].parentPath, curDir) == 0) folderIdx[folderIdxCount++] = i;
        }
        int modIdx[MAX_INSTALLED_MODS]; int modIdxCount = 0;
        for (int i = 0; i < g_instCount; i++) {
            if (_wcsicmp(g_instEntries[i].parentPath, curDir) == 0) modIdx[modIdxCount++] = i;
        }

        int gridCounter = 0;
        if (folderIdxCount > 1) qsort(folderIdx, (size_t)folderIdxCount, sizeof(int), InstalledFolderIndexCompare);
        for (int k = 0; k < folderIdxCount && g_instRowCount < MAX_INST_ROWS; k++) {
            g_instRows[g_instRowCount].kind = INSTROW_FOLDER;
            g_instRows[g_instRowCount].packIdx = pi;
            g_instRows[g_instRowCount].entryIdx = folderIdx[k];
            g_instRows[g_instRowCount].gridCol = gridCounter % INSTLIST_COLS;
            g_instRows[g_instRowCount].yOffset = yCursor + (gridCounter / INSTLIST_COLS) * INSTLIST_ROW_H;
            g_instRowCount++;
            gridCounter++;
        }
        if (modIdxCount > 1) qsort(modIdx, (size_t)modIdxCount, sizeof(int), InstalledModIndexCompare);
        for (int k = 0; k < modIdxCount && g_instRowCount < MAX_INST_ROWS; k++) {
            g_instRows[g_instRowCount].kind = INSTROW_MOD;
            g_instRows[g_instRowCount].packIdx = pi;
            g_instRows[g_instRowCount].entryIdx = modIdx[k];
            g_instRows[g_instRowCount].gridCol = gridCounter % INSTLIST_COLS;
            g_instRows[g_instRowCount].yOffset = yCursor + (gridCounter / INSTLIST_COLS) * INSTLIST_ROW_H;
            g_instRowCount++;
            gridCounter++;
        }
        int gridRowsUsed = (gridCounter + INSTLIST_COLS - 1) / INSTLIST_COLS;
        yCursor += gridRowsUsed * INSTLIST_ROW_H;
    }

    g_instContentH = yCursor;

    if (g_hInstListWnd) {
        RECT rc; GetClientRect(g_hInstListWnd, &rc);
        int maxScroll = (INSTLIST_HEADER_H + g_instContentH) - rc.bottom;
        if (maxScroll < 0) maxScroll = 0;
        if (g_instScrollY > maxScroll) g_instScrollY = maxScroll;
        if (g_instScrollY < 0) g_instScrollY = 0;
    }
}

static void UpdateInstBreadcrumbUI(void)
{
    if (!g_hInstBreadcrumb) return;
    if (g_instNavDepth == 0) {
        SetWindowTextW(g_hInstBreadcrumb, L"All Packs");
        if (g_hInstBackBtn) EnableWindow(g_hInstBackBtn, FALSE);
    } else {
        wchar_t text[600];
        wcscpy(text, g_instNavPack);
        for (int i = 0; i < g_instNavDepth; i++) {
            wcscat(text, L"  \u203A  ");
            wcscat(text, g_instNavNames[i]);
        }
        SetWindowTextW(g_hInstBreadcrumb, text);
        if (g_hInstBackBtn) EnableWindow(g_hInstBackBtn, TRUE);
    }
}

static void InstListNavigateInto(int folderIdx)
{
    if (folderIdx < 0 || folderIdx >= g_instFolderCount) return;
    InstalledFolderEntry* fe = &g_instFolders[folderIdx];
    if (g_instNavDepth == 0) {
        wcsncpy(g_instNavPack, fe->packFolder, 63); g_instNavPack[63] = 0;
    }
    if (g_instNavDepth < MAX_NAV_DEPTH) {
        wcscpy(g_instNavStack[g_instNavDepth], fe->fullPath);
        wcsncpy(g_instNavNames[g_instNavDepth], fe->folderName, 159); g_instNavNames[g_instNavDepth][159] = 0;
        g_instNavDepth++;
    }
    g_instScrollY = 0;
    RebuildInstalledRows();
    UpdateInstBreadcrumbUI();
}

static void InstListNavigateBack(void)
{
    if (g_instNavDepth <= 0) return;
    g_instNavDepth--;
    g_instScrollY = 0;
    RebuildInstalledRows();
    UpdateInstBreadcrumbUI();
}

/* mod/folder cards sit 2-per-row in a grid, same idea as Browse Mods -
   this is the single source of truth for that geometry */
static int InstListColWidth(RECT clientRc)
{
    int w = (clientRc.right - INSTLIST_PAD * 2 - INSTLIST_COL_GAP * (INSTLIST_COLS - 1)) / INSTLIST_COLS;
    return w > 0 ? w : 1;
}

static RECT InstListCardRect(RECT clientRc, int yOffset, int gridCol, int scrollY)
{
    int colW = InstListColWidth(clientRc);
    RECT r;
    r.left = INSTLIST_PAD + gridCol * (colW + INSTLIST_COL_GAP);
    r.right = r.left + colW;
    r.top = INSTLIST_HEADER_H + yOffset - scrollY;
    r.bottom = r.top + INSTLIST_ROW_H;
    return r;
}

/* Redirect (mod cards only, opens the mod's GameBanana page) sits above
   Move, which sits above Delete - three stacked buttons in the same
   column. Folder cards only ever draw the bottom two. */
static RECT InstListRedirectBtnRect(RECT cardRc)
{
    RECT r;
    r.right = cardRc.right - INSTLIST_PAD;
    r.left = r.right - INSTLIST_BTN_W;
    r.top = cardRc.top + INSTLIST_PAD;
    r.bottom = r.top + INSTLIST_BTN_H;
    return r;
}
static RECT InstListMoveBtnRect(RECT cardRc)
{
    RECT r = InstListRedirectBtnRect(cardRc);
    r.top = r.bottom + 6;
    r.bottom = r.top + INSTLIST_BTN_H;
    return r;
}
static RECT InstListDeleteBtnRect(RECT cardRc)
{
    RECT r = InstListMoveBtnRect(cardRc);
    r.top = r.bottom + 6;
    r.bottom = r.top + INSTLIST_BTN_H;
    return r;
}

/* folder cards only show Move/Delete (no Redirect) - they sit in the
   same two slots a mod card's own Move/Delete occupy, leaving the top
   slot (where Redirect would be) empty rather than re-centering, so a
   folder and mod card side by side still align */
static RECT InstListFolderMoveBtnRect(RECT cardRc)
{
    RECT r;
    r.right = cardRc.right - INSTLIST_PAD;
    r.left = r.right - INSTLIST_BTN_W;
    r.top = cardRc.top + INSTLIST_PAD + (INSTLIST_BTN_H + 6);
    r.bottom = r.top + INSTLIST_BTN_H;
    return r;
}
static RECT InstListFolderDeleteBtnRect(RECT cardRc)
{
    RECT r = InstListFolderMoveBtnRect(cardRc);
    r.top = r.bottom + 6;
    r.bottom = r.top + INSTLIST_BTN_H;
    return r;
}

/* ---- background scan ---- */

static BOOL DirHasDirectFile(const wchar_t* path)
{
    wchar_t search[MAX_PATH];
    wsprintfW(search, L"%s\\*", path);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    BOOL found = FALSE;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { found = TRUE; break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static BOOL DirHasDirectSubdir(const wchar_t* path)
{
    wchar_t search[MAX_PATH];
    wsprintfW(search, L"%s\\*", path);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    BOOL found = FALSE;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { found = TRUE; break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

/* recursively totals up mods/size nested under a container folder, for
   that folder row's own summary line */
static void CountModsRecursive(const wchar_t* path, int depth, int* outCount, ULONGLONG* outSize)
{
    if (depth > 8) return;
    wchar_t search[MAX_PATH];
    wsprintfW(search, L"%s\\*", path);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        wchar_t childPath[MAX_PATH];
        wsprintfW(childPath, L"%s\\%s", path, fd.cFileName);
        if (DirHasDirectFile(childPath)) {
            (*outCount)++;
            *outSize += GetDirSizeRecursiveW(childPath);
        } else if (DirHasDirectSubdir(childPath)) {
            CountModsRecursive(childPath, depth + 1, outCount, outSize);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* the main recursive walk: a directory's direct children each get
   classified as a MOD (has files directly inside) or a FOLDER (subfolders
   only, no loose files) - folders get recursed into so whatever's nested
   inside them is found too; a directory with neither files nor subfolders
   is simply skipped */
/* ---- auto-generated icon.png from a voice mod's dub video ----
   A local mod (no mod_meta.ini entry) with no icon.png of its own often
   still ships a "dub" video. If one's found directly in the mod's
   folder, its opening frame is extracted via Media Foundation and saved
   as icon.png right there - so it behaves like any hand-placed icon
   from then on, and later scans just read it normally without needing
   to touch the video again. */

static const wchar_t* const VIDEO_EXTENSIONS[] = {
    L".mp4", L".webm", L".mov", L".mkv", L".avi", L".wmv", L".m4v"
};
#define NUM_VIDEO_EXTENSIONS (sizeof(VIDEO_EXTENSIONS) / sizeof(VIDEO_EXTENSIONS[0]))

static BOOL ContainsCaseInsensitiveW(const wchar_t* haystack, const wchar_t* needle)
{
    size_t hLen = wcslen(haystack), nLen = wcslen(needle);
    if (nLen == 0 || nLen > hLen) return FALSE;
    for (size_t i = 0; i + nLen <= hLen; i++) {
        BOOL match = TRUE;
        for (size_t j = 0; j < nLen; j++) {
            if (towlower(haystack[i + j]) != towlower(needle[j])) { match = FALSE; break; }
        }
        if (match) return TRUE;
    }
    return FALSE;
}

/* looks directly inside dirPath (not subfolders) for a video file -
   preferring one with "dub" in the name when there's a choice, since a
   mod can ship more than one video (previews, bloopers, etc.) */
static BOOL FindDubVideoInDir(const wchar_t* dirPath, wchar_t* outPath)
{
    wchar_t search[MAX_PATH];
    wsprintfW(search, L"%s\\*", dirPath);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    wchar_t firstMatch[MAX_PATH] = L"";
    wchar_t dubMatch[MAX_PATH] = L"";
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const wchar_t* dot = wcsrchr(fd.cFileName, L'.');
        if (!dot) continue;
        BOOL isVideo = FALSE;
        for (size_t i = 0; i < NUM_VIDEO_EXTENSIONS; i++) {
            if (_wcsicmp(dot, VIDEO_EXTENSIONS[i]) == 0) { isVideo = TRUE; break; }
        }
        if (!isVideo) continue;
        if (!firstMatch[0]) wsprintfW(firstMatch, L"%s\\%s", dirPath, fd.cFileName);
        if (!dubMatch[0] && ContainsCaseInsensitiveW(fd.cFileName, L"dub")) {
            wsprintfW(dubMatch, L"%s\\%s", dirPath, fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (dubMatch[0]) { wcscpy(outPath, dubMatch); return TRUE; }
    if (firstMatch[0]) { wcscpy(outPath, firstMatch); return TRUE; }
    return FALSE;
}

/* extracts the first frame of videoPath as 32bpp BGRA, saves it to
   iconOutPath as a PNG (via a temp file + rename, so a failed/partial
   encode never leaves a broken icon.png behind), and returns a
   ready-to-display HBITMAP of that same frame. Returns NULL - without
   creating any file - if Media Foundation can't open or decode the
   video (unsupported codec, DRM, corrupt file, etc.); callers should
   treat that exactly like "no icon available". */
static HBITMAP GenerateVideoThumbnail(const wchar_t* videoPath, const wchar_t* iconOutPath)
{
    HBITMAP result = NULL;
    IMFSourceReader* reader = NULL;
    IMFMediaType* outType = NULL;
    IMFMediaType* actualType = NULL;
    IMFSample* sample = NULL;
    IMFMediaBuffer* buf = NULL;
    BOOL bufLocked = FALSE;

    HRESULT hr = MFCreateSourceReaderFromURL(videoPath, NULL, &reader);
    if (FAILED(hr)) goto done;

    reader->lpVtbl->SetStreamSelection(reader, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    hr = reader->lpVtbl->SetStreamSelection(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    if (FAILED(hr)) goto done;

    hr = MFCreateMediaType(&outType);
    if (FAILED(hr)) goto done;
    outType->lpVtbl->SetGUID(outType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    outType->lpVtbl->SetGUID(outType, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    hr = reader->lpVtbl->SetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, outType);
    if (FAILED(hr)) goto done;

    hr = reader->lpVtbl->GetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actualType);
    if (FAILED(hr)) goto done;
    {
        UINT64 packedSize = 0;
        actualType->lpVtbl->GetUINT64(actualType, &MF_MT_FRAME_SIZE, &packedSize);
        UINT32 width = (UINT32)(packedSize >> 32);
        UINT32 height = (UINT32)(packedSize & 0xFFFFFFFFu);
        if (width == 0 || height == 0 || width > 8192 || height > 8192) goto done;

        DWORD streamIndex = 0, flags = 0;
        LONGLONG timestamp = 0;
        hr = reader->lpVtbl->ReadSample(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
            &streamIndex, &flags, &timestamp, &sample);
        if (FAILED(hr) || !sample) goto done;

        hr = sample->lpVtbl->ConvertToContiguousBuffer(sample, &buf);
        if (FAILED(hr)) goto done;

        BYTE* data = NULL;
        DWORD maxLen = 0, curLen = 0;
        hr = buf->lpVtbl->Lock(buf, &data, &maxLen, &curLen);
        if (FAILED(hr)) goto done;
        bufLocked = TRUE;

        LONG stride = (LONG)(width * 4);
        if (curLen < (DWORD)stride * height) goto done;

        BITMAPINFO bmi; memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = (LONG)width;
        bmi.bmiHeader.biHeight = -(LONG)height; /* negative = top-down DIB */
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* dibBits = NULL;
        HBITMAP hbmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &dibBits, NULL, 0);
        if (hbmp && dibBits) {
            memcpy(dibBits, data, (size_t)stride * height);

            wchar_t tmpPath[MAX_PATH];
            wsprintfW(tmpPath, L"%s.tmp", iconOutPath);
            BOOL pngOk = FALSE;

            IWICImagingFactory* pFactory = NULL;
            if (SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                    &IID_IWICImagingFactory, (void**)&pFactory)) && pFactory) {
                IWICStream* pStream = NULL;
                IWICBitmapEncoder* pEncoder = NULL;
                IWICBitmapFrameEncode* pFrame = NULL;
                if (SUCCEEDED(pFactory->lpVtbl->CreateStream(pFactory, &pStream)) &&
                    SUCCEEDED(pStream->lpVtbl->InitializeFromFilename(pStream, tmpPath, GENERIC_WRITE)) &&
                    SUCCEEDED(pFactory->lpVtbl->CreateEncoder(pFactory, &GUID_ContainerFormatPng, NULL, &pEncoder)) &&
                    SUCCEEDED(pEncoder->lpVtbl->Initialize(pEncoder, (IStream*)pStream, WICBitmapEncoderNoCache)) &&
                    SUCCEEDED(pEncoder->lpVtbl->CreateNewFrame(pEncoder, &pFrame, NULL)) &&
                    SUCCEEDED(pFrame->lpVtbl->Initialize(pFrame, NULL)) &&
                    SUCCEEDED(pFrame->lpVtbl->SetSize(pFrame, width, height))) {
                    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
                    if (SUCCEEDED(pFrame->lpVtbl->SetPixelFormat(pFrame, &fmt)) &&
                        SUCCEEDED(pFrame->lpVtbl->WritePixels(pFrame, height, (UINT)stride,
                            (UINT)((DWORD)stride * height), data)) &&
                        SUCCEEDED(pFrame->lpVtbl->Commit(pFrame)) &&
                        SUCCEEDED(pEncoder->lpVtbl->Commit(pEncoder))) {
                        pngOk = TRUE;
                    }
                }
                if (pFrame) pFrame->lpVtbl->Release(pFrame);
                if (pEncoder) pEncoder->lpVtbl->Release(pEncoder);
                if (pStream) pStream->lpVtbl->Release(pStream);
                pFactory->lpVtbl->Release(pFactory);
            }

            if (pngOk) {
                MoveFileExW(tmpPath, iconOutPath, MOVEFILE_REPLACE_EXISTING);
            } else {
                DeleteFileW(tmpPath);
            }

            result = hbmp;
        }
    }

done:
    if (bufLocked) buf->lpVtbl->Unlock(buf);
    if (buf) buf->lpVtbl->Release(buf);
    if (sample) sample->lpVtbl->Release(sample);
    if (actualType) actualType->lpVtbl->Release(actualType);
    if (outType) outType->lpVtbl->Release(outType);
    if (reader) reader->lpVtbl->Release(reader);
    return result;
}

static void ScanDirLevel(const wchar_t* dirPath, const wchar_t* packFolder, const wchar_t* relPrefix, int depth,
    InstalledModEntry* modEntries, int* modCount,
    InstalledFolderEntry* folderEntries, int* folderCount,
    ModMetaRecord* metaAll, int metaCount, IWICImagingFactory* pFactory)
{
    if (depth > 8) return;
    wchar_t search[MAX_PATH];
    wsprintfW(search, L"%s\\*", dirPath);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (*modCount >= MAX_INSTALLED_MODS && *folderCount >= MAX_INSTALLED_FOLDERS) break;

        wchar_t childPath[MAX_PATH];
        wsprintfW(childPath, L"%s\\%s", dirPath, fd.cFileName);

        if (DirHasDirectFile(childPath)) {
            if (*modCount >= MAX_INSTALLED_MODS) continue;
            InstalledModEntry* e = &modEntries[*modCount];
            memset(e, 0, sizeof(*e));
            wcsncpy(e->packFolder, packFolder, 63); e->packFolder[63] = 0;
            wcsncpy(e->parentPath, dirPath, MAX_PATH - 1); e->parentPath[MAX_PATH - 1] = 0;
            wcsncpy(e->folderName, fd.cFileName, 159); e->folderName[159] = 0;
            wcscpy(e->fullPath, childPath);

            wchar_t metaKey[256];
            wsprintfW(metaKey, L"%s\\%s", relPrefix, fd.cFileName);
            ModMetaRecord* meta = NULL;
            for (int mi = 0; mi < metaCount; mi++) {
                if (_wcsicmp(metaAll[mi].key, metaKey) == 0) { meta = &metaAll[mi]; break; }
            }

            if (meta) {
                e->source = INSTSRC_BROWSER;
                wcscpy(e->title, meta->title[0] ? meta->title : e->folderName);
                wcscpy(e->author, meta->author);
                wcscpy(e->profileUrl, meta->profileUrl);
                wcscpy(e->thumbUrl, meta->thumbUrl);
                wcsncpy(e->subtitle, meta->description, 191); e->subtitle[191] = 0;
                for (wchar_t* c = e->subtitle; *c; c++) {
                    if (*c == L'\r' || *c == L'\n') { *c = 0; break; }
                }
                e->dateAddedUnix = meta->installedUnix;

                if (e->thumbUrl[0] && pFactory) {
                    wchar_t imgErr[256] = L"";
                    DWORD imgSize = 0;
                    BYTE* imgData = HttpGetToMemory(e->thumbUrl, &imgSize, imgErr);
                    if (imgData) {
                        e->thumb = DecodeImageToBitmap(pFactory, imgData, imgSize);
                        free(imgData);
                    }
                }
            } else {
                e->source = INSTSRC_LOCAL;
                wchar_t title[128] = L"", subtitle[192] = L"", author[128] = L"";
                ReadLocalModInfo(e->fullPath, title, subtitle, author);
                wcscpy(e->title, title[0] ? title : e->folderName);
                wcscpy(e->subtitle, subtitle);
                wcscpy(e->author, author);
                e->dateAddedUnix = GetDirCreatedUnixTime(e->fullPath);

                wchar_t iconPath[MAX_PATH];
                wsprintfW(iconPath, L"%s\\icon.png", e->fullPath);
                DWORD iconSize = 0;
                BYTE* iconData = ReadEntireFileW(iconPath, &iconSize);
                if (iconData) {
                    if (pFactory) e->thumb = DecodeImageToBitmap(pFactory, iconData, iconSize);
                    free(iconData);
                } else {
                    /* no icon.png - if this mod ships a dub video, use its
                       opening frame instead (and save it as icon.png so
                       future scans just find it like any other icon) */
                    wchar_t videoPath[MAX_PATH];
                    if (FindDubVideoInDir(e->fullPath, videoPath)) {
                        e->thumb = GenerateVideoThumbnail(videoPath, iconPath);
                    }
                }
            }

            e->sizeBytes = GetDirSizeRecursiveW(e->fullPath);
            (*modCount)++;
        } else if (DirHasDirectSubdir(childPath)) {
            if (*folderCount < MAX_INSTALLED_FOLDERS) {
                InstalledFolderEntry* fe = &folderEntries[*folderCount];
                memset(fe, 0, sizeof(*fe));
                wcsncpy(fe->packFolder, packFolder, 63); fe->packFolder[63] = 0;
                wcsncpy(fe->parentPath, dirPath, MAX_PATH - 1); fe->parentPath[MAX_PATH - 1] = 0;
                wcsncpy(fe->folderName, fd.cFileName, 159); fe->folderName[159] = 0;
                wcscpy(fe->fullPath, childPath);
                fe->dateAddedUnix = GetDirCreatedUnixTime(childPath);
                int mc = 0; ULONGLONG sz = 0;
                CountModsRecursive(childPath, 0, &mc, &sz);
                fe->modCount = mc;
                fe->sizeBytes = sz;
                (*folderCount)++;
            }
            wchar_t childRelPrefix[300];
            wsprintfW(childRelPrefix, L"%s\\%s", relPrefix, fd.cFileName);
            ScanDirLevel(childPath, packFolder, childRelPrefix, depth + 1, modEntries, modCount,
                folderEntries, folderCount, metaAll, metaCount, pFactory);
        }
        /* else: empty folder (no files, no subfolders) - nothing to show */
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

typedef struct { HWND listWnd; } InstScanThreadParam;
typedef struct {
    InstalledModEntry* entries; int count;
    InstalledFolderEntry* folders; int folderCount;
} InstScanResult;

static DWORD WINAPI ScanInstalledModsThreadProc(LPVOID lpParam)
{
    InstScanThreadParam* tp = (InstScanThreadParam*)lpParam;
    HWND targetWnd = tp->listWnd;
    free(tp);

    HRESULT coHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IWICImagingFactory* pFactory = NULL;
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
        &IID_IWICImagingFactory, (void**)&pFactory);
    /* needed for GenerateVideoThumbnail (dub-video icon fallback); if this
       fails, those calls just fail gracefully too and mods fall back to
       the usual colored-initial placeholder like any other missing icon */
    HRESULT mfHr = MFStartup(MF_VERSION, MFSTARTUP_LITE);

    InstScanResult* result = (InstScanResult*)calloc(1, sizeof(InstScanResult));
    if (!result) {
        if (SUCCEEDED(mfHr)) MFShutdown();
        if (pFactory) pFactory->lpVtbl->Release(pFactory);
        if (coHr == S_OK || coHr == S_FALSE) CoUninitialize();
        return 0;
    }

    InstalledModEntry* entries = (InstalledModEntry*)calloc(MAX_INSTALLED_MODS, sizeof(InstalledModEntry));
    InstalledFolderEntry* folders = (InstalledFolderEntry*)calloc(MAX_INSTALLED_FOLDERS, sizeof(InstalledFolderEntry));
    int count = 0, folderCount = 0;

    wchar_t gameDir[MAX_PATH];
    if (entries && folders && GetAppDataGameDir(gameDir) && PathIsDirW(gameDir)) {
        ModMetaRecord* metaAll = (ModMetaRecord*)malloc(sizeof(ModMetaRecord) * MAX_META_RECORDS);
        int metaCount = metaAll ? LoadModMetaAll(metaAll, MAX_META_RECORDS) : 0;

        for (int pi = 0; pi < NUM_PACK_FOLDERS; pi++) {
            wchar_t packDir[MAX_PATH];
            wsprintfW(packDir, L"%s\\%s", gameDir, PACK_FOLDERS[pi]);
            if (!PathIsDirW(packDir)) continue;
            ScanDirLevel(packDir, PACK_FOLDERS[pi], PACK_FOLDERS[pi], 0, entries, &count, folders, &folderCount,
                metaAll, metaCount, pFactory);
        }
        if (metaAll) free(metaAll);
    }

    if (pFactory) pFactory->lpVtbl->Release(pFactory);
    if (SUCCEEDED(mfHr)) MFShutdown();
    if (coHr == S_OK || coHr == S_FALSE) CoUninitialize();

    result->entries = entries;
    result->count = count;
    result->folders = folders;
    result->folderCount = folderCount;

    if (IsWindow(targetWnd)) {
        PostMessageW(targetWnd, WM_APP_INSTALLED_LOADED, 0, (LPARAM)result);
    } else {
        if (result->entries) {
            for (int i = 0; i < result->count; i++) {
                if (result->entries[i].thumb) DeleteObject(result->entries[i].thumb);
            }
            free(result->entries);
        }
        if (result->folders) free(result->folders);
        free(result);
    }
    return 0;
}

static void StartInstalledModsScan(HWND listWnd)
{
    InstScanThreadParam* tp = (InstScanThreadParam*)malloc(sizeof(InstScanThreadParam));
    if (!tp) return;
    tp->listWnd = listWnd;
    g_instLoadState = INSTLOAD_LOADING;
    HANDLE hThread = CreateThread(NULL, 0, ScanInstalledModsThreadProc, tp, 0, NULL);
    if (hThread) CloseHandle(hThread); else free(tp);
}

static void InstListRemoveEntryAt(int idx)
{
    if (idx < 0 || idx >= g_instCount) return;
    if (g_instEntries[idx].thumb) DeleteObject(g_instEntries[idx].thumb);
    for (int i = idx; i < g_instCount - 1; i++) g_instEntries[i] = g_instEntries[i + 1];
    g_instCount--;
}

static void InstListRemoveFolderAt(int idx)
{
    if (idx < 0 || idx >= g_instFolderCount) return;
    for (int i = idx; i < g_instFolderCount - 1; i++) g_instFolders[i] = g_instFolders[i + 1];
    g_instFolderCount--;
}

/* kind: INSTROW_MOD or INSTROW_FOLDER */
static void InstListDoDelete(HWND listWnd, InstRowKind kind, int idx)
{
    wchar_t path[MAX_PATH] = L"", displayName[192] = L"", packFolder[64] = L"", parentPath[MAX_PATH] = L"";
    InstModSource source = INSTSRC_LOCAL;
    wchar_t modMetaFolderName[160] = L"";

    if (kind == INSTROW_MOD) {
        if (idx < 0 || idx >= g_instCount) return;
        InstalledModEntry* e = &g_instEntries[idx];
        wcscpy(path, e->fullPath);
        wcscpy(displayName, e->title[0] ? e->title : e->folderName);
        wcscpy(packFolder, e->packFolder);
        wcscpy(parentPath, e->parentPath);
        wcscpy(modMetaFolderName, e->folderName);
        source = e->source;
    } else {
        if (idx < 0 || idx >= g_instFolderCount) return;
        InstalledFolderEntry* fe = &g_instFolders[idx];
        wcscpy(path, fe->fullPath);
        wcscpy(displayName, fe->folderName);
        wcscpy(packFolder, fe->packFolder);
    }

    wchar_t msg[440];
    if (kind == INSTROW_FOLDER) {
        wsprintfW(msg, L"Delete the \"%s\" folder from %s?\n\nThis permanently removes it and everything inside it from disk.",
            displayName, packFolder);
    } else {
        wsprintfW(msg, L"Delete \"%s\" from %s?\n\nThis permanently removes the mod folder from disk.",
            displayName, packFolder);
    }
    HWND mainWnd = GetParent(listWnd);
    if (MessageBoxW(mainWnd, msg, kind == INSTROW_FOLDER ? L"Delete Folder" : L"Delete Mod",
            MB_ICONWARNING | MB_YESNO) != IDYES) return;

    DeleteDirectoryRecursiveW(path);
    if (kind == INSTROW_MOD && source == INSTSRC_BROWSER) {
        wchar_t gameDir[MAX_PATH];
        if (GetAppDataGameDir(gameDir)) {
            wchar_t relParent[MAX_PATH];
            GetPathRelativeToGameDir(gameDir, parentPath, relParent, MAX_PATH);
            wchar_t metaKey[256];
            wsprintfW(metaKey, L"%s\\%s", relParent, modMetaFolderName);
            DeleteModMetaRecord(metaKey);
        }
    }

    if (kind == INSTROW_MOD) InstListRemoveEntryAt(idx);
    else InstListRemoveFolderAt(idx);

    RebuildInstalledRows();
    InvalidateRect(listWnd, NULL, FALSE);
}

/* ===================== folder-tree pickers =====================
   Every "pick a destination folder" popup in the app - Move, Choose
   Destination (installing from Browse Mods), and Download From Link -
   shares one Explorer-style tree view. The tree is built by walking the
   pack folders on disk directly (rather than from the Installed Mods
   scan cache) so it's always current, works even if the Installed Mods
   tab was never opened, and includes empty folders you made yourself.
   A folder counts as a destination unless it's a mod (has files
   directly inside it), since you can't install into or move into a mod. */

#define MAX_FOLDER_TREE_PATHS 1024
static wchar_t g_folderTreePaths[MAX_FOLDER_TREE_PATHS][MAX_PATH];
static HTREEITEM g_folderTreeItems[MAX_FOLDER_TREE_PATHS];
static int g_folderTreePathCount = 0;

static int AddFolderTreePath(const wchar_t* path)
{
    if (g_folderTreePathCount >= MAX_FOLDER_TREE_PATHS) return -1;
    wcscpy(g_folderTreePaths[g_folderTreePathCount], path);
    return g_folderTreePathCount++;
}

static HWND MakeFolderTree(HWND parent, HINSTANCE hInst, int x, int y, int w, int h, int id)
{
    HWND hTree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
        TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
        x, y, w, h, parent, (HMENU)(INT_PTR)id, hInst, NULL);
    SendMessageW(hTree, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);
    SetWindowTheme(hTree, g_darkMode ? L"DarkMode_Explorer" : L"Explorer", NULL);
    SendMessageW(hTree, TVM_SETBKCOLOR, 0, (LPARAM)(g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL));
    SendMessageW(hTree, TVM_SETTEXTCOLOR, 0, (LPARAM)(g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT));
    return hTree;
}

static void InsertFolderTreeChildrenFromDisk(HWND hTree, HTREEITEM parentItem, const wchar_t* parentPath,
    int depth, const wchar_t* excludePath)
{
    if (depth > 8) return;
    wchar_t search[MAX_PATH];
    wsprintfW(search, L"%s\\*", parentPath);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        wchar_t childPath[MAX_PATH];
        wsprintfW(childPath, L"%s\\%s", parentPath, fd.cFileName);
        if (excludePath[0] && _wcsicmp(childPath, excludePath) == 0) continue; /* skips the whole excluded subtree */
        if (DirHasDirectFile(childPath)) continue; /* a mod, not a place to put things */

        int pathIdx = AddFolderTreePath(childPath);
        if (pathIdx < 0) break;

        TVINSERTSTRUCTW tvis; memset(&tvis, 0, sizeof(tvis));
        tvis.hParent = parentItem;
        tvis.hInsertAfter = TVI_LAST;
        tvis.item.mask = TVIF_TEXT | TVIF_PARAM;
        tvis.item.pszText = fd.cFileName;
        tvis.item.lParam = (LPARAM)pathIdx;
        HTREEITEM item = (HTREEITEM)SendMessageW(hTree, TVM_INSERTITEMW, 0, (LPARAM)&tvis);
        g_folderTreeItems[pathIdx] = item;

        InsertFolderTreeChildrenFromDisk(hTree, item, childPath, depth + 1, excludePath);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* fills hTree with the 7 pack roots and every destination folder under
   them; excludePath (or "") is a folder to leave out along with
   everything inside it - used when moving a folder, since it can't land
   inside itself. Returns the pack-root nodes through topItems. */
static void BuildFolderTree(HWND hTree, const wchar_t* excludePath, HTREEITEM* topItems, int* topItemCount)
{
    g_folderTreePathCount = 0;
    *topItemCount = 0;
    SendMessageW(hTree, WM_SETREDRAW, FALSE, 0);
    SendMessageW(hTree, TVM_DELETEITEM, 0, (LPARAM)TVI_ROOT);

    wchar_t gameDir[MAX_PATH];
    if (GetAppDataGameDir(gameDir)) {
        for (int pi = 0; pi < NUM_PACK_FOLDERS; pi++) {
            wchar_t fullPath[MAX_PATH];
            wsprintfW(fullPath, L"%s\\%s", gameDir, PACK_FOLDERS[pi]);
            if (excludePath[0] && _wcsicmp(fullPath, excludePath) == 0) continue;

            int pathIdx = AddFolderTreePath(fullPath);
            if (pathIdx < 0) break;

            TVINSERTSTRUCTW tvis; memset(&tvis, 0, sizeof(tvis));
            tvis.hParent = TVI_ROOT;
            tvis.hInsertAfter = TVI_LAST;
            tvis.item.mask = TVIF_TEXT | TVIF_PARAM;
            tvis.item.pszText = (wchar_t*)PACK_FOLDERS[pi];
            tvis.item.lParam = (LPARAM)pathIdx;
            HTREEITEM item = (HTREEITEM)SendMessageW(hTree, TVM_INSERTITEMW, 0, (LPARAM)&tvis);
            g_folderTreeItems[pathIdx] = item;
            topItems[(*topItemCount)++] = item;

            InsertFolderTreeChildrenFromDisk(hTree, item, fullPath, 0, excludePath);
        }
    }
    for (int i = 0; i < *topItemCount; i++) {
        SendMessageW(hTree, TVM_EXPAND, TVE_EXPAND, (LPARAM)topItems[i]);
    }
    SendMessageW(hTree, WM_SETREDRAW, TRUE, 0);
}

static BOOL GetFolderTreeSelectedPath(HWND hTree, wchar_t* out, int outCap)
{
    if (!hTree) return FALSE;
    HTREEITEM sel = (HTREEITEM)SendMessageW(hTree, TVM_GETNEXTITEM, TVGN_CARET, 0);
    if (!sel) return FALSE;
    TVITEMW tvi; memset(&tvi, 0, sizeof(tvi));
    tvi.mask = TVIF_PARAM;
    tvi.hItem = sel;
    SendMessageW(hTree, TVM_GETITEMW, 0, (LPARAM)&tvi);
    int pathIdx = (int)tvi.lParam;
    if (pathIdx < 0 || pathIdx >= g_folderTreePathCount) return FALSE;
    wcsncpy(out, g_folderTreePaths[pathIdx], outCap - 1);
    out[outCap - 1] = 0;
    return TRUE;
}

/* selects and scrolls to a specific folder if it's in the tree, else
   falls back to the first pack root */
static void SelectFolderTreePath(HWND hTree, const wchar_t* path, HTREEITEM* topItems, int topItemCount)
{
    for (int i = 0; i < g_folderTreePathCount; i++) {
        if (path[0] && _wcsicmp(g_folderTreePaths[i], path) == 0) {
            SendMessageW(hTree, TVM_SELECTITEM, TVGN_CARET, (LPARAM)g_folderTreeItems[i]);
            SendMessageW(hTree, TVM_ENSUREVISIBLE, 0, (LPARAM)g_folderTreeItems[i]);
            return;
        }
    }
    if (topItemCount > 0) SendMessageW(hTree, TVM_SELECTITEM, TVGN_CARET, (LPARAM)topItems[0]);
}

/* the remembered install destination is stored relative to the game
   dir (so it survives the game dir moving); this turns it back into an
   absolute path for selecting it in a freshly built tree */
static void GetLastInstallFolderAbs(wchar_t* out, int outCap)
{
    out[0] = 0;
    wchar_t gameDir[MAX_PATH];
    if (g_lastPackFolder[0] && GetAppDataGameDir(gameDir)) {
        _snwprintf(out, outCap, L"%s\\%s", gameDir, g_lastPackFolder);
        out[outCap - 1] = 0;
    }
}

/* NM_DBLCLK also fires for double-clicks on empty space in the tree,
   which shouldn't count as "pick the selected folder" */
static BOOL TreeDoubleClickedOnItem(HWND hTree)
{
    POINT pt; GetCursorPos(&pt);
    ScreenToClient(hTree, &pt);
    TVHITTESTINFO ht; memset(&ht, 0, sizeof(ht));
    ht.pt = pt;
    SendMessageW(hTree, TVM_HITTEST, 0, (LPARAM)&ht);
    return (ht.flags & TVHT_ONITEM) != 0;
}

/* ---- Choose Destination popup (installing from the Browse Mods list) ---- */

#define ID_DESTPICKER_TREE ID_DESTPICKER_COMBO
static HWND g_hDestPickerTree = NULL;
static HWND g_hDestPickerOwner = NULL;

static void CloseDestPicker(HWND hwnd, BOOL doInstall)
{
    wchar_t chosenAbs[MAX_PATH] = L"", chosenRel[MAX_PATH] = L"";
    if (doInstall && GetFolderTreeSelectedPath(g_hDestPickerTree, chosenAbs, MAX_PATH)) {
        wchar_t gameDir[MAX_PATH];
        if (GetAppDataGameDir(gameDir)) GetPathRelativeToGameDir(gameDir, chosenAbs, chosenRel, MAX_PATH);
    }
    HWND owner = g_hDestPickerOwner;
    g_hDestPickerTree = NULL;
    DestroyWindow(hwnd);
    if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
    if (doInstall && chosenRel[0]) {
        wcsncpy(g_lastPackFolder, chosenRel, MAX_PATH - 1); g_lastPackFolder[MAX_PATH - 1] = 0;
        SaveSettings();
        StartInstallJob(owner, TRUE, g_destPickerUrl, chosenRel,
            g_destPickerTitle, g_destPickerAuthor, g_destPickerThumbUrl, g_destPickerDescription);
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
    case WM_DRAWITEM:
        if (((LPDRAWITEMSTRUCT)lParam)->CtlType == ODT_BUTTON) {
            DrawThemedOwnerButton((LPDRAWITEMSTRUCT)lParam);
            return TRUE;
        }
        return FALSE;
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT);
        return (LRESULT)(g_hBrushBg ? g_hBrushBg : GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_NOTIFY: {
        LPNMHDR nm = (LPNMHDR)lParam;
        if (nm->idFrom == ID_DESTPICKER_TREE && nm->code == NM_DBLCLK && TreeDoubleClickedOnItem(nm->hwndFrom)) {
            CloseDestPicker(hwnd, TRUE);
            return 0;
        }
        break;
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

    const int w = 400, h = 440;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
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

    HWND hMsg = CreateWindowW(L"STATIC", L"Install this mod to which folder?",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 20, w - 40, 20, hPop, NULL, hInst, NULL);
    SendMessageW(hMsg, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);

    g_hDestPickerTree = MakeFolderTree(hPop, hInst, 20, 46, w - 40, h - 116, ID_DESTPICKER_TREE);
    HTREEITEM topItems[NUM_PACK_FOLDERS]; int topItemCount = 0;
    BuildFolderTree(g_hDestPickerTree, L"", topItems, &topItemCount);
    wchar_t lastAbs[MAX_PATH];
    GetLastInstallFolderAbs(lastAbs, MAX_PATH);
    SelectFolderTreePath(g_hDestPickerTree, lastAbs, topItems, topItemCount);

    HWND hInstallBtn = MakeButton(hPop, hInst, L"Install",
        20, h - 60, 140, 32, ID_DESTPICKER_INSTALL, g_fontRegular);
    (void)hInstallBtn;

    HWND hCancelBtn = MakeButton(hPop, hInst, L"Cancel",
        w - 20 - 100, h - 60, 100, 32, ID_DESTPICKER_CANCEL, g_fontRegular);
    (void)hCancelBtn;

    EnableWindow(hOwner, FALSE);
    ShowWindow(hPop, SW_SHOW);
    UpdateWindow(hPop);
    SetForegroundWindow(hPop);
    SetFocus(g_hDestPickerTree);
}

/* ---- "Move to..." popup ---- */

static InstRowKind g_moveKind = INSTROW_MOD;
static int g_moveEntryIdx = -1;
static HWND g_hMovePickerTree = NULL;
static HWND g_hMovePickerOwner = NULL;
static HWND g_hMovePickerListWnd = NULL;

#define ID_MOVEPICKER_TREE   7030
#define ID_MOVEPICKER_MOVE   7031
#define ID_MOVEPICKER_CANCEL 7032

static void CloseMovePicker(HWND hwnd, BOOL doMove)
{
    wchar_t chosenPath[MAX_PATH] = L"";
    BOOL haveChosen = doMove && GetFolderTreeSelectedPath(g_hMovePickerTree, chosenPath, MAX_PATH);
    HWND owner = g_hMovePickerOwner;
    HWND listWnd = g_hMovePickerListWnd;
    InstRowKind kind = g_moveKind;
    int entryIdx = g_moveEntryIdx;
    g_hMovePickerTree = NULL;
    DestroyWindow(hwnd);
    if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }

    if (doMove && haveChosen) {
        wchar_t srcPath[MAX_PATH] = L"", srcName[160] = L"", srcParent[MAX_PATH] = L"";
        InstModSource source = INSTSRC_LOCAL;
        BOOL valid = FALSE;

        if (kind == INSTROW_MOD && entryIdx >= 0 && entryIdx < g_instCount) {
            InstalledModEntry* e = &g_instEntries[entryIdx];
            wcscpy(srcPath, e->fullPath); wcscpy(srcName, e->folderName); wcscpy(srcParent, e->parentPath);
            source = e->source;
            valid = TRUE;
        } else if (kind == INSTROW_FOLDER && entryIdx >= 0 && entryIdx < g_instFolderCount) {
            InstalledFolderEntry* fe = &g_instFolders[entryIdx];
            wcscpy(srcPath, fe->fullPath); wcscpy(srcName, fe->folderName); wcscpy(srcParent, fe->parentPath);
            valid = TRUE;
        }

        if (valid && _wcsicmp(chosenPath, srcParent) != 0) {
            wchar_t gameDir[MAX_PATH];
            if (GetAppDataGameDir(gameDir)) {
                EnsureDirW(chosenPath);

                wchar_t destPath[MAX_PATH];
                wsprintfW(destPath, L"%s\\%s", chosenPath, srcName);
                wchar_t uniqueDest[MAX_PATH];
                MakeUniquePathW(destPath, uniqueDest);

                if (MoveFileW(srcPath, uniqueDest)) {
                    if (kind == INSTROW_MOD && source == INSTSRC_BROWSER) {
                        wchar_t newFolderName[160];
                        const wchar_t* lastSlash = wcsrchr(uniqueDest, L'\\');
                        wcsncpy(newFolderName, lastSlash ? lastSlash + 1 : uniqueDest, 159);
                        newFolderName[159] = 0;

                        wchar_t relOldParent[MAX_PATH], relNewParent[MAX_PATH];
                        GetPathRelativeToGameDir(gameDir, srcParent, relOldParent, MAX_PATH);
                        GetPathRelativeToGameDir(gameDir, chosenPath, relNewParent, MAX_PATH);
                        wchar_t oldKey[300], newKey[300];
                        wsprintfW(oldKey, L"%s\\%s", relOldParent, srcName);
                        wsprintfW(newKey, L"%s\\%s", relNewParent, newFolderName);
                        RenameModMetaRecordKey(oldKey, newKey);
                    }
                    /* a move can shuffle an entire nested subtree around,
                       so refresh from disk rather than patching every
                       affected path in memory - and back out to the
                       top-level overview, since the folder we might have
                       been browsing could be the one that just moved */
                    g_instNavDepth = 0;
                    g_instScrollY = 0;
                    if (listWnd) StartInstalledModsScan(listWnd);
                } else {
                    MessageBoxW(owner, L"Couldn't move that.", L"Move", MB_ICONERROR | MB_OK);
                }
            }
        }
        if (listWnd) InvalidateRect(listWnd, NULL, FALSE);
    }
    g_moveEntryIdx = -1;
}

static LRESULT CALLBACK MovePickerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
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
        SetTextColor(hdc, g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT);
        return (LRESULT)(g_hBrushBg ? g_hBrushBg : GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_NOTIFY: {
        LPNMHDR nm = (LPNMHDR)lParam;
        if (nm->idFrom == ID_MOVEPICKER_TREE && nm->code == NM_DBLCLK && TreeDoubleClickedOnItem(nm->hwndFrom)) {
            CloseMovePicker(hwnd, TRUE);
            return 0;
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_MOVEPICKER_MOVE:   CloseMovePicker(hwnd, TRUE); return 0;
        case ID_MOVEPICKER_CANCEL: CloseMovePicker(hwnd, FALSE); return 0;
        }
        break;
    case WM_CLOSE:
        CloseMovePicker(hwnd, FALSE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowMovePicker(HWND hOwner, HWND listWnd, HINSTANCE hInst, InstRowKind kind, int entryIdx)
{
    wchar_t curParentPath[MAX_PATH] = L"", excludePath[MAX_PATH] = L"";
    if (kind == INSTROW_MOD) {
        if (entryIdx < 0 || entryIdx >= g_instCount) return;
        wcscpy(curParentPath, g_instEntries[entryIdx].parentPath);
    } else {
        if (entryIdx < 0 || entryIdx >= g_instFolderCount) return;
        wcscpy(curParentPath, g_instFolders[entryIdx].parentPath);
        wcscpy(excludePath, g_instFolders[entryIdx].fullPath); /* can't move a folder into itself */
    }
    g_moveKind = kind;
    g_moveEntryIdx = entryIdx;
    g_hMovePickerListWnd = listWnd;

    static BOOL classRegistered = FALSE;
    const wchar_t CLASS_NAME[] = L"CVLauncherMovePickerWnd";
    if (!classRegistered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc   = MovePickerWndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = CLASS_NAME;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
        RegisterClassW(&wc);
        classRegistered = TRUE;
    }

    const int w = 400, h = 440;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    RECT rc = {0, 0, w, h};
    AdjustWindowRect(&rc, style, FALSE);
    int winW = rc.right - rc.left, winH = rc.bottom - rc.top;

    RECT prc; GetWindowRect(hOwner, &prc);
    int posX = prc.left + ((prc.right - prc.left) - winW) / 2;
    int posY = prc.top + ((prc.bottom - prc.top) - winH) / 2;

    g_hMovePickerOwner = hOwner;

    HWND hPop = CreateWindowExW(WS_EX_DLGMODALFRAME, CLASS_NAME,
        kind == INSTROW_FOLDER ? L"Move Folder" : L"Move Mod",
        style, posX, posY, winW, winH, hOwner, NULL, hInst, NULL);
    if (!hPop) return;
    ApplyDarkTitlebar(hPop, g_darkMode);

    HWND hMsg = CreateWindowW(L"STATIC",
        kind == INSTROW_FOLDER ? L"Move this folder to which folder?" : L"Move this mod to which folder?",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 20, w - 40, 20, hPop, NULL, hInst, NULL);
    SendMessageW(hMsg, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);

    g_hMovePickerTree = MakeFolderTree(hPop, hInst, 20, 46, w - 40, h - 116, ID_MOVEPICKER_TREE);
    HTREEITEM topItems[NUM_PACK_FOLDERS]; int topItemCount = 0;
    BuildFolderTree(g_hMovePickerTree, excludePath, topItems, &topItemCount);
    SelectFolderTreePath(g_hMovePickerTree, curParentPath, topItems, topItemCount);

    HWND hMoveBtn = MakeButton(hPop, hInst, L"Move",
        20, h - 60, 140, 32, ID_MOVEPICKER_MOVE, g_fontRegular);
    (void)hMoveBtn;

    HWND hCancelBtn = MakeButton(hPop, hInst, L"Cancel",
        w - 20 - 100, h - 60, 100, 32, ID_MOVEPICKER_CANCEL, g_fontRegular);
    (void)hCancelBtn;

    EnableWindow(hOwner, FALSE);
    ShowWindow(hPop, SW_SHOW);
    UpdateWindow(hPop);
    SetForegroundWindow(hPop);
    SetFocus(g_hMovePickerTree);
}

/* ---- "Download From Link" popup ----
   The drag-and-drop mod downloader (drop a .zip/.rar/.7z, or drag a
   GameBanana link) - it used to sit at the bottom of the Launcher tab
   with a flat pack-folder dropdown; now it's opened from a button on the
   Browse Mods tab and picks its destination from the same folder tree as
   the other pickers. It stays open across drops (so several mods can go
   to different folders in one visit) and closes via its Close button.
   Drag-and-drop is registered on this popup only while it's open. */

#define ID_DOWNLOADPICKER_TREE  8001
#define ID_DOWNLOADPICKER_CLOSE 8002

static HWND g_hDownloadPickerWnd = NULL;
static HWND g_hDownloadPickerTree = NULL;
static HWND g_hDownloadPickerOwner = NULL;

/* where the next drop should install to, as a path relative to the game
   dir - whatever's selected in the popup's tree right now */
static void GetDownloadDestFolder(wchar_t* out, int outCap)
{
    wchar_t fullPath[MAX_PATH], gameDir[MAX_PATH];
    if (GetFolderTreeSelectedPath(g_hDownloadPickerTree, fullPath, MAX_PATH) && GetAppDataGameDir(gameDir)) {
        GetPathRelativeToGameDir(gameDir, fullPath, out, outCap);
    } else {
        wcsncpy(out, PACK_FOLDERS[0], outCap - 1);
        out[outCap - 1] = 0;
    }
}

static void CloseDownloadPicker(HWND hwnd)
{
    HWND owner = g_hDownloadPickerOwner;
    if (g_pDropTarget) {
        RevokeDragDrop(hwnd);
        ((IDropTarget*)g_pDropTarget)->lpVtbl->Release((IDropTarget*)g_pDropTarget);
        g_pDropTarget = NULL;
    }
    g_hDownloadPickerTree = NULL;
    g_hDownloadPickerWnd = NULL;
    DestroyWindow(hwnd);
    if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
}

static LRESULT CALLBACK DownloadPickerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
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
        SetTextColor(hdc, g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT);
        return (LRESULT)(g_hBrushBg ? g_hBrushBg : GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_DOWNLOADPICKER_CLOSE) { CloseDownloadPicker(hwnd); return 0; }
        break;
    case WM_CLOSE:
        CloseDownloadPicker(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowDownloadFromLinkPicker(HWND hOwner, HINSTANCE hInst)
{
    if (g_hDownloadPickerWnd) { SetForegroundWindow(g_hDownloadPickerWnd); return; }

    static BOOL classRegistered = FALSE;
    const wchar_t CLASS_NAME[] = L"CVLauncherDownloadPickerWnd";
    if (!classRegistered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc   = DownloadPickerWndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = CLASS_NAME;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
        RegisterClassW(&wc);
        classRegistered = TRUE;
    }

    const int w = 460, h = 540;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    RECT rc = {0, 0, w, h};
    AdjustWindowRect(&rc, style, FALSE);
    int winW = rc.right - rc.left, winH = rc.bottom - rc.top;

    RECT prc; GetWindowRect(hOwner, &prc);
    int posX = prc.left + ((prc.right - prc.left) - winW) / 2;
    int posY = prc.top + ((prc.bottom - prc.top) - winH) / 2;

    g_hDownloadPickerOwner = hOwner;

    HWND hPop = CreateWindowExW(WS_EX_DLGMODALFRAME, CLASS_NAME, L"Download From Link",
        style, posX, posY, winW, winH, hOwner, NULL, hInst, NULL);
    if (!hPop) return;
    g_hDownloadPickerWnd = hPop;
    ApplyDarkTitlebar(hPop, g_darkMode);

    HWND hMsg = CreateWindowW(L"STATIC", L"Drag a .zip/.rar/.7z file or a GameBanana link into the box below.",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 18, w - 40, 20, hPop, NULL, hInst, NULL);
    SendMessageW(hMsg, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);

    HWND hDrop = CreateWindowW(L"STATIC",
        L"Drag a .zip/.rar/.7z file or a GameBanana mod link here",
        WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE | WS_BORDER,
        20, 46, w - 40, 90, hPop, NULL, hInst, NULL);
    SendMessageW(hDrop, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);
    g_dropZoneRect.left = 20; g_dropZoneRect.top = 46;
    g_dropZoneRect.right = w - 20; g_dropZoneRect.bottom = 46 + 90;

    HWND hTip = CreateWindowW(L"STATIC",
        L"Tip: For the link method to work, add a https:// in front of the "
        L"'gamebanana.com/mods' link. The downloader won't work without it. "
        L"Download links [https://gamebanana.com/dl/######] also work.",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 142, w - 40, 62, hPop, NULL, hInst, NULL);
    SendMessageW(hTip, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);

    HWND hLabel = CreateWindowW(L"STATIC", L"Install to which folder?",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 212, w - 40, 20, hPop, NULL, hInst, NULL);
    SendMessageW(hLabel, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);

    g_hDownloadPickerTree = MakeFolderTree(hPop, hInst, 20, 236, w - 40, h - 236 - 72, ID_DOWNLOADPICKER_TREE);
    HTREEITEM topItems[NUM_PACK_FOLDERS]; int topItemCount = 0;
    BuildFolderTree(g_hDownloadPickerTree, L"", topItems, &topItemCount);
    wchar_t lastAbs[MAX_PATH];
    GetLastInstallFolderAbs(lastAbs, MAX_PATH);
    SelectFolderTreePath(g_hDownloadPickerTree, lastAbs, topItems, topItemCount);

    HWND hCloseBtn = MakeButton(hPop, hInst, L"Close",
        w - 20 - 110, h - 56, 110, 34, ID_DOWNLOADPICKER_CLOSE, g_fontRegular);
    (void)hCloseBtn;

    /* drag-and-drop lives on this popup only while it's open */
    g_pDropTarget = (DropTargetImpl*)malloc(sizeof(DropTargetImpl));
    if (g_pDropTarget) {
        g_pDropTarget->base.lpVtbl = &g_dropTargetVtbl;
        g_pDropTarget->refCount = 1;
        g_pDropTarget->hwnd = hPop;
        RegisterDragDrop(hPop, (IDropTarget*)g_pDropTarget);
    }

    EnableWindow(hOwner, FALSE);
    ShowWindow(hPop, SW_SHOW);
    UpdateWindow(hPop);
    SetForegroundWindow(hPop);
}

/* draws a simple manila-folder shape (a tab + body) rather than relying
   on a font glyph, so folder rows render consistently regardless of what
   symbol fonts happen to be installed */
/* the app's one hand-drawn folder glyph: a rounded tab sitting just
   above (and slightly overlapping) a rounded body, so the two pieces
   read as a single folder silhouette rather than two flush, sharp-
   cornered boxes. Proportions were tuned by eye at a 48px reference
   size and scale from there, so this one glyph can be reused anywhere
   a folder icon is needed - the sidebar's Modding Folder button and the
   Installed Mods list's folder cards both call this now, instead of
   each having their own slightly different version. */
static void DrawFolderGlyph(HDC hdc, RECT r, COLORREF color)
{
    int cx = (r.left + r.right) / 2;
    int cy = (r.top + r.bottom) / 2;
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    int size = (w < h) ? w : h;

    HBRUSH br = CreateSolidBrush(color);
    HGDIOBJ oldBr = SelectObject(hdc, br);
    HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));

    int rnd1 = (3 * size) / 48; if (rnd1 < 2) rnd1 = 2;
    int rnd2 = (4 * size) / 48; if (rnd2 < 2) rnd2 = 2;

    RoundRect(hdc, cx + (-9 * size) / 48, cy + (-8 * size) / 48,
                   cx + (1 * size) / 48,  cy + (-2 * size) / 48, rnd1, rnd1);
    RoundRect(hdc, cx + (-11 * size) / 48, cy + (-4 * size) / 48,
                   cx + (11 * size) / 48,  cy + (8 * size) / 48, rnd2, rnd2);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBr);
    DeleteObject(br);
}

/* ---- the Installed Mods list window itself ---- */

static LRESULT CALLBACK InstalledModsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
        g_hInstSortCombo = CreateWindowW(L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            12, 6, 260, 200, hwnd, (HMENU)(INT_PTR)ID_INSTLIST_SORT, hInst, NULL);
        SendMessageW(g_hInstSortCombo, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);
        SendMessageW(g_hInstSortCombo, CB_ADDSTRING, 0, (LPARAM)L"Sort: Name (A-Z)");
        SendMessageW(g_hInstSortCombo, CB_ADDSTRING, 0, (LPARAM)L"Sort: Size (largest first)");
        SendMessageW(g_hInstSortCombo, CB_ADDSTRING, 0, (LPARAM)L"Sort: Date Added (newest first)");
        SendMessageW(g_hInstSortCombo, CB_SETCURSEL, (WPARAM)g_instSortMode, 0);
        SetWindowTheme(g_hInstSortCombo, g_darkMode ? L"DarkMode_Explorer" : L"Explorer", NULL);

        HWND hRefresh = MakeButton(hwnd, hInst, L"Refresh", 282, 5, 84, 26, ID_INSTLIST_REFRESH, g_fontSmall);
        (void)hRefresh;

        g_hInstBackBtn = MakeButton(hwnd, hInst, L"\u25C0 Back", 12, 42, 70, 26, ID_INSTLIST_BACK, g_fontSmall);
        EnableWindow(g_hInstBackBtn, FALSE);

        g_hInstBreadcrumb = CreateWindowW(L"STATIC", L"All Packs",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
            90, 47, 400, 20, hwnd, NULL, hInst, NULL);
        SendMessageW(g_hInstBreadcrumb, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
        return 0;
    }

    case WM_SIZE: {
        RECT rc; GetClientRect(hwnd, &rc);
        int c = ListColsForWidth(rc.right, INSTLIST_PAD, INSTLIST_COL_GAP, 330);
        if (c != g_instCols) {
            g_instCols = c;
            RebuildInstalledRows();        /* in-memory only: re-flows the cards into the new column count */
        } else {
            int maxScroll = (INSTLIST_HEADER_H + g_instContentH) - rc.bottom;
            if (maxScroll < 0) maxScroll = 0;
            if (g_instScrollY > maxScroll) g_instScrollY = maxScroll;
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_DRAWITEM:
        if (((LPDRAWITEMSTRUCT)lParam)->CtlType == ODT_BUTTON) {
            DrawThemedOwnerButton((LPDRAWITEMSTRUCT)lParam);
            return TRUE;
        }
        return FALSE;

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT);
        return (LRESULT)(g_hBrushBg ? g_hBrushBg : GetSysColorBrush(COLOR_BTNFACE));
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        HBRUSH bg = CreateSolidBrush(g_darkMode ? COLOR_DARK_BG : COLOR_LIGHT_BG);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        SetBkMode(hdc, TRANSPARENT);
        COLORREF textColor = g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT;
        HDC memDC = CreateCompatibleDC(hdc);

        for (int ri = 0; ri < g_instRowCount; ri++) {
            InstRow* row = &g_instRows[ri];

            if (row->kind == INSTROW_HEADER) {
                int y = INSTLIST_HEADER_H + row->yOffset - g_instScrollY;
                if (y + INSTLIST_GROUP_H >= 0 && y <= rc.bottom) {
                    RECT hRc = { 0, y, rc.right, y + INSTLIST_GROUP_H };
                    HBRUSH hBg = CreateSolidBrush(g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL);
                    FillRect(hdc, &hRc, hBg);
                    DeleteObject(hBg);

                    RECT accentRc = { 0, y, 4, y + INSTLIST_GROUP_H };
                    HBRUSH accentBrush = CreateSolidBrush(THEME_ACCENT);
                    FillRect(hdc, &accentRc, accentBrush);
                    DeleteObject(accentBrush);

                    int modCount = 0; ULONGLONG groupSize = 0;
                    for (int i = 0; i < g_instCount; i++) {
                        if (_wcsicmp(g_instEntries[i].packFolder, PACK_FOLDERS[row->packIdx]) == 0) {
                            modCount++; groupSize += g_instEntries[i].sizeBytes;
                        }
                    }
                    wchar_t sizeStr[32]; FormatSizeShort(groupSize, sizeStr);
                    wchar_t headerText[128];
                    wsprintfW(headerText, L"%s  \u2014  %d mod%s, %s",
                        PACK_FOLDERS[row->packIdx], modCount, modCount == 1 ? L"" : L"s", sizeStr);

                    RECT tRc = { INSTLIST_PAD + 6, y, rc.right - INSTLIST_PAD, y + INSTLIST_GROUP_H };
                    SetTextColor(hdc, textColor);
                    SelectObject(hdc, g_fontGroupHeader);
                    DrawTextW(hdc, headerText, -1, &tRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                }
                continue;
            }

            RECT card = InstListCardRect(rc, row->yOffset, row->gridCol, g_instScrollY);
            if (card.bottom < 0 || card.top > rc.bottom) continue;

            RECT thumbRc = { card.left + INSTLIST_PAD, card.top + INSTLIST_PAD,
                              card.left + INSTLIST_PAD + INSTLIST_THUMB, card.top + INSTLIST_PAD + INSTLIST_THUMB };
            RECT textRc = { thumbRc.right + INSTLIST_PAD, card.top + INSTLIST_PAD,
                             card.right - INSTLIST_PAD - INSTLIST_BTN_W - INSTLIST_PAD, card.top + INSTLIST_PAD + 20 };
            RECT subRc = textRc; subRc.top += 22; subRc.bottom += 22;
            RECT metaRc = subRc; metaRc.top += 18; metaRc.bottom += 18;

            wchar_t linkUrl[160] = L"";

            if (row->kind == INSTROW_FOLDER) {
                InstalledFolderEntry* fe = &g_instFolders[row->entryIdx];
                DrawFolderGlyph(hdc, thumbRc, RGB(241, 196, 15));

                SetTextColor(hdc, textColor);
                HGDIOBJ oldFont = SelectObject(hdc, g_fontRegular);
                DrawTextW(hdc, fe->folderName, -1, &textRc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

                SelectObject(hdc, g_fontSmall);
                DrawTextW(hdc, L"Folder", -1, &subRc, DT_LEFT | DT_TOP | DT_SINGLELINE);

                wchar_t sizeStr[32], dateStr[32], metaLine[256];
                FormatSizeShort(fe->sizeBytes, sizeStr);
                FormatUnixDateShort(fe->dateAddedUnix, dateStr);
                wsprintfW(metaLine, L"%d mod%s  \u2022  %s  \u2022  Added %s",
                    fe->modCount, fe->modCount == 1 ? L"" : L"s", sizeStr, dateStr);
                DrawTextW(hdc, metaLine, -1, &metaRc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(hdc, oldFont);
            } else {
                InstalledModEntry* e = &g_instEntries[row->entryIdx];
                wcscpy(linkUrl, e->profileUrl);

                if (e->thumb) {
                    HGDIOBJ oldBmp = SelectObject(memDC, e->thumb);
                    BITMAP bm; GetObjectW(e->thumb, sizeof(bm), &bm);
                    SetStretchBltMode(hdc, HALFTONE);
                    SetBrushOrgEx(hdc, 0, 0, NULL);
                    StretchBlt(hdc, thumbRc.left, thumbRc.top, INSTLIST_THUMB, INSTLIST_THUMB,
                        memDC, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                    SelectObject(memDC, oldBmp);
                } else {
                    const wchar_t* nameForInitial = e->title[0] ? e->title : e->folderName;
                    wchar_t initial[2] = { nameForInitial[0] ? towupper(nameForInitial[0]) : L'?', 0 };
                    COLORREF palette[6] = {
                        RGB(230, 126, 34), RGB(52, 152, 219), RGB(155, 89, 182),
                        RGB(46, 204, 113), RGB(231, 76, 60), RGB(26, 188, 156)
                    };
                    unsigned int hash = 0;
                    for (const wchar_t* c = nameForInitial; *c; c++) hash = hash * 31u + (unsigned int)*c;
                    HBRUSH ph = CreateSolidBrush(palette[hash % 6]);
                    FillRect(hdc, &thumbRc, ph);
                    DeleteObject(ph);
                    SetTextColor(hdc, RGB(255, 255, 255));
                    HGDIOBJ oldInitFont = SelectObject(hdc, g_fontTitle);
                    DrawTextW(hdc, initial, 1, &thumbRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdc, oldInitFont);
                }
                HPEN pen = CreatePen(PS_SOLID, 1, textColor);
                HGDIOBJ oldPen = SelectObject(hdc, pen);
                HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, thumbRc.left, thumbRc.top, thumbRc.right, thumbRc.bottom);
                SelectObject(hdc, oldBrush);
                SelectObject(hdc, oldPen);
                DeleteObject(pen);

                SetTextColor(hdc, textColor);
                HGDIOBJ oldFont = SelectObject(hdc, g_fontRegular);
                DrawTextW(hdc, e->title[0] ? e->title : e->folderName, -1, &textRc,
                    DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

                SelectObject(hdc, g_fontSmall);
                DrawTextW(hdc, e->subtitle[0] ? e->subtitle : e->folderName, -1, &subRc,
                    DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

                wchar_t sizeStr[32], dateStr[32], metaLine[256];
                FormatSizeShort(e->sizeBytes, sizeStr);
                FormatUnixDateShort(e->dateAddedUnix, dateStr);
                if (e->author[0]) {
                    wsprintfW(metaLine, L"by %s  \u2022  %s  \u2022  Added %s", e->author, sizeStr, dateStr);
                } else {
                    wsprintfW(metaLine, L"%s  \u2022  Added %s", sizeStr, dateStr);
                }
                DrawTextW(hdc, metaLine, -1, &metaRc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(hdc, oldFont);
            }

            /* button stack: mods get Redirect/Move/Delete, folders just
               get Move/Delete (in the same two slots a mod's own
               Move/Delete occupy) since a folder has no single GameBanana
               link to redirect to */
            RECT moveRc, delRc;
            if (row->kind == INSTROW_MOD) {
                RECT redirectRc = InstListRedirectBtnRect(card);
                BOOL hasLink = (linkUrl[0] != 0);
                COLORREF redirectPenColor = hasLink ? textColor : RGB(140, 140, 140);
                HBRUSH redirectBrush = CreateSolidBrush(g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL);
                HPEN redirectPen = CreatePen(PS_SOLID, 1, redirectPenColor);
                HGDIOBJ oldRedirBrush = SelectObject(hdc, redirectBrush);
                HGDIOBJ oldRedirPen = SelectObject(hdc, redirectPen);
                RoundRect(hdc, redirectRc.left, redirectRc.top, redirectRc.right, redirectRc.bottom, 6, 6);
                SelectObject(hdc, oldRedirBrush);
                SelectObject(hdc, oldRedirPen);
                DeleteObject(redirectBrush);
                DeleteObject(redirectPen);
                SetTextColor(hdc, redirectPenColor);
                SelectObject(hdc, g_fontSmall);
                DrawTextW(hdc, L"Redirect", -1, &redirectRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                moveRc = InstListMoveBtnRect(card);
                delRc = InstListDeleteBtnRect(card);
            } else {
                moveRc = InstListFolderMoveBtnRect(card);
                delRc = InstListFolderDeleteBtnRect(card);
            }

            const wchar_t* labels[2] = { L"Move", L"Delete" };
            RECT* rects[2] = { &moveRc, &delRc };
            for (int bi = 0; bi < 2; bi++) {
                HBRUSH btnBrush = CreateSolidBrush(g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL);
                HPEN btnPen = CreatePen(PS_SOLID, 1, textColor);
                HGDIOBJ oldBtnBrush = SelectObject(hdc, btnBrush);
                HGDIOBJ oldBtnPen = SelectObject(hdc, btnPen);
                RoundRect(hdc, rects[bi]->left, rects[bi]->top, rects[bi]->right, rects[bi]->bottom, 6, 6);
                SelectObject(hdc, oldBtnBrush);
                SelectObject(hdc, oldBtnPen);
                DeleteObject(btnBrush);
                DeleteObject(btnPen);
                SetTextColor(hdc, textColor);
                SelectObject(hdc, g_fontSmall);
                DrawTextW(hdc, labels[bi], -1, rects[bi], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            /* one separator per grid row, drawn once from the left
               column, spanning the full width under both columns */
            if (row->gridCol == 0) {
                HPEN sepPen = CreatePen(PS_SOLID, 1, SEP_COLOR);
                HGDIOBJ oldSepPen = SelectObject(hdc, sepPen);
                MoveToEx(hdc, INSTLIST_PAD, card.bottom, NULL);
                LineTo(hdc, rc.right - INSTLIST_PAD, card.bottom);
                SelectObject(hdc, oldSepPen);
                DeleteObject(sepPen);
            }
        }
        DeleteDC(memDC);

        if (g_instLoadState == INSTLOAD_LOADING) {
            RECT emptyRc = { 0, INSTLIST_HEADER_H, rc.right, rc.bottom };
            SetTextColor(hdc, textColor);
            SelectObject(hdc, g_fontRegular);
            DrawTextW(hdc, L"Scanning installed mods\u2026", -1, &emptyRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (g_instLoadState == INSTLOAD_LOADED && g_instRowCount == 0) {
            RECT emptyRc = { 0, INSTLIST_HEADER_H, rc.right, rc.bottom };
            SetTextColor(hdc, textColor);
            SelectObject(hdc, g_fontRegular);
            DrawTextW(hdc, g_instNavDepth == 0 ? L"No mods installed yet." : L"This folder is empty.",
                -1, &emptyRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int mouseX = GET_X_LPARAM(lParam);
        int mouseY = GET_Y_LPARAM(lParam);
        RECT rc; GetClientRect(hwnd, &rc);

        for (int ri = 0; ri < g_instRowCount; ri++) {
            InstRow* row = &g_instRows[ri];
            if (row->kind == INSTROW_HEADER) continue;

            RECT card = InstListCardRect(rc, row->yOffset, row->gridCol, g_instScrollY);
            if (mouseX < card.left || mouseX > card.right || mouseY < card.top || mouseY > card.bottom) continue;

            POINT pt = { mouseX, mouseY };
            RECT moveRc, delRc;
            if (row->kind == INSTROW_MOD) {
                RECT redirectRc = InstListRedirectBtnRect(card);
                if (PtInRect(&redirectRc, pt)) {
                    InstalledModEntry* e = &g_instEntries[row->entryIdx];
                    if (e->profileUrl[0]) ShellExecuteW(NULL, L"open", e->profileUrl, NULL, NULL, SW_SHOWNORMAL);
                    return 0;
                }
                moveRc = InstListMoveBtnRect(card);
                delRc = InstListDeleteBtnRect(card);
            } else {
                moveRc = InstListFolderMoveBtnRect(card);
                delRc = InstListFolderDeleteBtnRect(card);
            }

            if (PtInRect(&moveRc, pt)) {
                HWND mainWnd = GetParent(hwnd);
                ShowMovePicker(mainWnd, hwnd, g_hInstance, row->kind, row->entryIdx);
                return 0;
            }
            if (PtInRect(&delRc, pt)) {
                InstListDoDelete(hwnd, row->kind, row->entryIdx);
                return 0;
            }
            if (row->kind == INSTROW_FOLDER) {
                InstListNavigateInto(row->entryIdx);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        RECT rc; GetClientRect(hwnd, &rc);
        int totalH = INSTLIST_HEADER_H + g_instContentH;
        int maxScroll = totalH - rc.bottom;
        if (maxScroll < 0) maxScroll = 0;
        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        g_instScrollY -= (delta / WHEEL_DELTA) * INSTLIST_ROW_H;
        if (g_instScrollY < 0) g_instScrollY = 0;
        if (g_instScrollY > maxScroll) g_instScrollY = maxScroll;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_INSTLIST_SORT && HIWORD(wParam) == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(g_hInstSortCombo, CB_GETCURSEL, 0, 0);
            if (sel >= 0) g_instSortMode = (InstSortMode)sel;
            RebuildInstalledRows();
            g_instScrollY = 0;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (LOWORD(wParam) == ID_INSTLIST_REFRESH && g_instLoadState != INSTLOAD_LOADING) {
            StartInstalledModsScan(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (LOWORD(wParam) == ID_INSTLIST_BACK) {
            InstListNavigateBack();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;

    case WM_APP_INSTALLED_LOADED: {
        InstScanResult* result = (InstScanResult*)lParam;
        if (result) {
            for (int i = 0; i < g_instCount; i++) {
                if (g_instEntries[i].thumb) DeleteObject(g_instEntries[i].thumb);
            }
            g_instCount = 0;
            g_instFolderCount = 0;
            if (result->entries) {
                int toCopy = result->count < MAX_INSTALLED_MODS ? result->count : MAX_INSTALLED_MODS;
                for (int i = 0; i < toCopy; i++) g_instEntries[g_instCount++] = result->entries[i];
                for (int i = toCopy; i < result->count; i++) {
                    if (result->entries[i].thumb) DeleteObject(result->entries[i].thumb);
                }
                free(result->entries);
            }
            if (result->folders) {
                int toCopy = result->folderCount < MAX_INSTALLED_FOLDERS ? result->folderCount : MAX_INSTALLED_FOLDERS;
                for (int i = 0; i < toCopy; i++) g_instFolders[g_instFolderCount++] = result->folders[i];
                free(result->folders);
            }
            free(result);
        }
        g_instLoadState = INSTLOAD_LOADED;
        g_instNavDepth = 0;
        RebuildInstalledRows();
        UpdateInstBreadcrumbUI();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_DESTROY:
        for (int i = 0; i < g_instCount; i++) {
            if (g_instEntries[i].thumb) DeleteObject(g_instEntries[i].thumb);
        }
        g_instCount = 0;
        g_instFolderCount = 0;
        g_instRowCount = 0;
        g_instNavDepth = 0;
        g_instLoadState = INSTLOAD_IDLE;
        g_instScrollY = 0;
        g_hInstSortCombo = NULL;
        g_hInstBreadcrumb = NULL;
        g_hInstBackBtn = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static HWND CreateInstalledModsWnd(HWND parent, HINSTANCE hInst, int x, int y, int w, int h)
{
    static BOOL classRegistered = FALSE;
    const wchar_t CLASS_NAME[] = L"CVLauncherInstalledModsWnd";
    if (!classRegistered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc   = InstalledModsWndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = CLASS_NAME;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        RegisterClassW(&wc);
        classRegistered = TRUE;
    }
    return CreateWindowExW(0, CLASS_NAME, NULL,
        WS_CHILD | WS_CLIPCHILDREN,
        x, y, w, h, parent, NULL, hInst, NULL);
}

#include "dubrender.inc"
#include "extras.inc"
#include "settings.inc"

/* the short description shown when you hover a button */
static const wchar_t* ButtonTipText(int id)
{
    switch (id) {
    /* sidebar */
    case ID_TAB_LAUNCHER:   return L"Launcher: pick a game version and launch it.";
    case ID_TAB_BROWSE:     return L"Browse Mods: find mods on GameBanana and install them.";
    case ID_TAB_INSTALLED:  return L"Installed Mods: see, sort and move the mods you've installed.";
    case ID_TAB_DUB:        return L"Dub Renderer: turn an exported dub recording into a finished MP4.";
    case ID_FULLSCREEN:     return L"Toggle full screen (F11). Press Esc to leave full screen.";
    case ID_MUSIC:          return L"Turn the background music on or off. It loops while on.";
    case ID_CAP_MIN:        return L"Minimize";
    case ID_CAP_MAX:        return L"Maximize or restore the window";
    case ID_CAP_CLOSE:      return L"Close the launcher";
    case ID_TAB_SETTINGS:   return L"Settings: dark mode, music and volume, the modding folder, and info.";
    case ID_SET_DARK:       return L"Switch between light and dark mode.";
    case ID_SET_FOLDER:     return L"Open the game's modding folder in File Explorer.";
    case ID_SET_README:     return L"Open the README on GitHub in your browser.";
    case ID_SET_CREDITS:    return L"See who made the game, the Dub Renderer, the music and this launcher.";
    case ID_SET_BACK:       return L"Go back.";
    case ID_SET_ABOUT:      return L"About this launcher: what it does, how to get started, and useful links.";
    case ID_AB_GAME:        return L"Open The Choicer Voicer's page on itch.io.";
    case ID_AB_GB:          return L"Open the game's page on GameBanana.";
    case ID_SET_THEME0 + 0: return L"Colors: Default (neutral gray)";
    case ID_SET_THEME0 + 1: return L"Colors: Purple (dark purple / light purple)";
    case ID_SET_THEME0 + 2: return L"Colors: Red (dark red / light red)";
    case ID_SET_THEME0 + 3: return L"Colors: Green (dark green / light green)";
    case ID_SET_THEME0 + 4: return L"Colors: Blue (dark blue / light blue)";
    case IDC_DARKMODE_CHECK:return L"Switch between light and dark mode.";
    case ID_MODDING:        return L"Open the game's modding folder in File Explorer.";
    case ID_README_INFO:    return L"Show the welcome message and the README link again.";
    /* launcher tab */
    case ID_LAUNCH_NORMAL:  return L"Launch the selected Normal Mode version of the game.";
    case ID_LAUNCH_COMPAT:  return L"Launch the selected Compatibility Mode version of the game.";
    case ID_ADD_NORMAL_VERSION: return L"Add another Normal Mode version to the launcher's list.";
    case ID_ADD_COMPAT_VERSION: return L"Add another Compatibility Mode version to the launcher's list.";
    case ID_OPEN_BROWSE_FROM_LAUNCHER: return L"Open the Browse Mods tab.";
    /* browse + installed tabs */
    case ID_MODLIST_DOWNLOAD_LINK: return L"Install a mod from a GameBanana download link, or from a local .zip, .rar or .7z file.";
    case ID_INSTLIST_REFRESH: return L"Rescan your pack folders for installed mods.";
    case ID_INSTLIST_BACK:    return L"Go back up one folder.";
    /* add version window */
    case ID_ADDVER_BROWSE:  return L"Choose the game's .exe file for this version.";
    case ID_ADDVER_CHECK1:
    case ID_ADDVER_CHECK2:  return L"Click to tick or untick this confirmation. Both must be ticked to add the version.";
    case ID_ADDVER_ADD:     return L"Add this version to the launcher.";
    /* install / move / download windows */
    case ID_DESTPICKER_INSTALL: return L"Install the mod into the selected folder.";
    case ID_MOVEPICKER_MOVE:    return L"Move it into the selected folder.";
    case ID_ADDVER_CANCEL:
    case ID_DESTPICKER_CANCEL:
    case ID_MOVEPICKER_CANCEL:  return L"Close this window without making any changes.";
    case ID_DOWNLOADPICKER_CLOSE:
    case ID_README_CLOSE:
    case ID_DUB_CREDIT_CLOSE:   return L"Close this window.";
    case ID_README_OPEN:    return L"Open the README on GitHub in your browser.";
    /* dub renderer */
    case ID_DUB_INFO:       return L"About this section, and who made the original renderer.";
    case ID_DUB_REFRESH:    return L"Rescan for dub recording exports.";
    case ID_DUB_BROWSE_REC: return L"Pick an export folder (one holding _dubrecord_ files) yourself.";
    case ID_DUB_BROWSE_PACK:return L"Pick the original dub pack folder yourself.";
    case ID_DUB_BROWSE_OUT: return L"Choose where to save the finished MP4.";
    case ID_DUB_RENDER:     return L"Mix your recording with the pack's video and backing track into an MP4.";
    case ID_DUB_CANCEL:     return L"Stop the render that's in progress.";
    case ID_DUB_OPEN:       return L"Show the finished MP4 in File Explorer.";
    }
    return NULL;
}

/* Shows the Launcher tab's own content (real child windows + the overlay-
   drawn text), the Browse Mods tab's list window, or the Installed Mods
   tab's list window, and kicks off the first fetch/scan the first time
   each of the latter two is opened. */
static BOOL CALLBACK ClipSiblingsProc(HWND h, LPARAM parent)
{
    if (GetParent(h) == (HWND)parent) {
        SetWindowLongPtrW(h, GWL_STYLE, GetWindowLongPtrW(h, GWL_STYLE) | WS_CLIPSIBLINGS);
        SetWindowPos(h, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    return TRUE;
}

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
    /* label 0 is the text fallback for the wordmark logo - only ever
       shown if the logo image itself failed to load, regardless of tab */
    if (g_hLogoBitmap) g_overlayLabels[0].visible = FALSE;
    g_logoVisible = showLauncher;
    if (g_hModListWnd) {
        ShowWindow(g_hModListWnd, (newTab == TAB_BROWSE) ? SW_SHOW : SW_HIDE);
    }
    if (g_hInstListWnd) {
        ShowWindow(g_hInstListWnd, (newTab == TAB_INSTALLED) ? SW_SHOW : SW_HIDE);
    }
    if (g_hDubWnd) {
        ShowWindow(g_hDubWnd, (newTab == TAB_DUB) ? SW_SHOW : SW_HIDE);
    }
    if (g_hSettingsWnd) {
        ShowWindow(g_hSettingsWnd, (newTab == TAB_SETTINGS) ? SW_SHOW : SW_HIDE);
    }
    if (newTab == TAB_DUB) DubOnTabShown();

    if (newTab == TAB_BROWSE && g_modLoadState == MODLOAD_IDLE) {
        StartModFetch(g_hModListWnd, g_modNextPage, g_modSortMode, g_modSearchQuery);
    }
    if (newTab == TAB_INSTALLED && g_instLoadState == INSTLOAD_IDLE) {
        StartInstalledModsScan(g_hInstListWnd);
    }

    if (g_hTabLauncherBtn) InvalidateRect(g_hTabLauncherBtn, NULL, TRUE);
    if (g_hTabBrowseBtn) InvalidateRect(g_hTabBrowseBtn, NULL, TRUE);
    if (g_hTabInstalledBtn) InvalidateRect(g_hTabInstalledBtn, NULL, TRUE);
    if (g_hTabDubBtn) InvalidateRect(g_hTabDubBtn, NULL, TRUE);
    if (g_hTabSettingsBtn) InvalidateRect(g_hTabSettingsBtn, NULL, TRUE);
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

        PickUiFontFace(hInst);
        /* Anton is a single, already-heavy weight, so everything is FW_NORMAL
           (FW_BOLD would just make Windows fake an even heavier one). Sizes
           are ~14% smaller than the old Segoe UI ones because Anton is taller
           and wider at the same nominal size. */
        BOOL isAnton = (wcscmp(g_uiFace, L"Anton") == 0);
        int wTitle  = isAnton ? FW_NORMAL : FW_BOLD;
        int szTitle = isAnton ? -22 : -25, szReg = isAnton ? -14 : -16;
        int szSmall = isAnton ? -12 : -14, szGroup = isAnton ? -15 : -17;
        g_fontTitle = CreateFontW(szTitle, 0, 0, 0, wTitle, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, g_uiFace);
        g_fontRegular = CreateFontW(szReg, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, g_uiFace);
        g_fontSmall = CreateFontW(szSmall, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, g_uiFace);
        g_fontIcon = CreateFontW(-26, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Symbol");
        g_fontSymbol = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Symbol");
        g_fontGroupHeader = CreateFontW(szGroup, 0, 0, 0, wTitle, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, g_uiFace);

        g_fontCaption = CreateFontW(-10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
        LoadSettings();
        g_hInstance = hInst;

        /* ===== left sidebar: persistent icon navigation, always visible
           regardless of which tab is active (same as the old tab/header
           buttons were) ===== */
        g_hSidebarPanel = CreateWindowW(L"STATIC", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER,
            0, 0, SIDEBAR_W, BASE_CLIENT_H, hwnd, NULL, hInst, NULL);

        g_hTabLauncherBtn = MakeButton(hwnd, hInst, L"\u25B6", 10, 24, 56, 56, ID_TAB_LAUNCHER, g_fontIcon);
        g_hTabBrowseBtn = MakeButton(hwnd, hInst, L"\u25A6", 10, 94, 56, 56, ID_TAB_BROWSE, g_fontIcon);
        g_hTabSettingsBtn = MakeButton(hwnd, hInst, L"", 14, 560, 48, 48, ID_TAB_SETTINGS, g_fontIcon);

        /* Dark Mode moved into the sidebar (was floating in the content
           area at x=675,y=20) - it used to sit directly under where the
           Browse Mods tab's list window gets created (also y=20), and
           since that list window is a real, opaque sibling window
           created afterward (later siblings default to a higher
           z-order), it was drawing right over the Dark Mode button
           whenever the Browse Mods tab was open. The sidebar is the one
           part of the window already guaranteed to stay visible and
           unobstructed on every tab, so this is a permanent fix rather
           than just nudging coordinates around the list window again. */
        g_hTabInstalledBtn = MakeButton(hwnd, hInst, L"\u2261", 10, 164, 56, 56, ID_TAB_INSTALLED, g_fontIcon);
        g_hTabDubBtn = MakeButton(hwnd, hInst, L"", 10, 234, 56, 56, ID_TAB_DUB, g_fontIcon);
        /* full screen toggle sits right under the tabs (dark mode, music, the modding folder and
           the info text all live on the Settings page now) */
        g_hBtnFullscreen = MakeButton(hwnd, hInst, L"", 10, 304, 56, 56, ID_FULLSCREEN, g_fontIcon);
        g_hCapMin   = MakeButton(hwnd, hInst, L"", 0, 4, 44, 28, ID_CAP_MIN, g_fontIcon);
        g_hCapMax   = MakeButton(hwnd, hInst, L"", 0, 4, 44, 28, ID_CAP_MAX, g_fontIcon);
        g_hCapClose = MakeButton(hwnd, hInst, L"", 0, 4, 44, 28, ID_CAP_CLOSE, g_fontIcon);
        {
            /* ask Windows 11 for rounded corners (DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUND = 2);
               on Windows 10 the call fails and ApplyWindowRounding() clips a rounded region instead */
            int cornerPref = 2;
            g_dwmRound = SUCCEEDED(DwmSetWindowAttribute(hwnd, 33, &cornerPref, sizeof(cornerPref)));
        }

        /* side image */
        g_hBitmap = LoadBitmapW(hInst, MAKEINTRESOURCEW(IDB_SIDEIMAGE));
        HWND hImg = CreateWindowW(L"STATIC", NULL,
            WS_CHILD | WS_VISIBLE | SS_BITMAP,
            CONTENT_LEFT, CONTENT_TOP, 290, 386, hwnd, NULL, hInst, NULL);
        SendMessageW(hImg, STM_SETIMAGE, (WPARAM)IMAGE_BITMAP, (LPARAM)g_hBitmap);
        g_hSideImage = hImg;
        TrackLauncherPageWnd(hImg);

        /* custom button-icon art + wordmark logo - PNGs embedded as RCDATA
           resources (see resource.rc), decoded via WIC. COM is already
           initialized on this thread by the OleInitialize() call in
           wWinMain, so it's safe to create a WIC factory here directly. */
        {
            IWICImagingFactory* pIconFactory = NULL;
            CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                &IID_IWICImagingFactory, (void**)&pIconFactory);
            if (pIconFactory) {
                g_hIconHomeBlack   = LoadPngResourceBitmap(hInst, IDP_HOME_BLACK, pIconFactory);
                g_hIconHomeWhite   = LoadPngResourceBitmap(hInst, IDP_HOME_WHITE, pIconFactory);
                g_hIconBrowseBlack = LoadPngResourceBitmap(hInst, IDP_BROWSE_BLACK, pIconFactory);
                g_hIconBrowseWhite = LoadPngResourceBitmap(hInst, IDP_BROWSE_WHITE, pIconFactory);
                g_hIconModsBlack   = LoadPngResourceBitmap(hInst, IDP_MODS_BLACK, pIconFactory);
                g_hIconModsWhite   = LoadPngResourceBitmap(hInst, IDP_MODS_WHITE, pIconFactory);
                g_hIconThemeBlack  = LoadPngResourceBitmap(hInst, IDP_THEME_BLACK, pIconFactory);
                g_hIconThemeWhite  = LoadPngResourceBitmap(hInst, IDP_THEME_WHITE, pIconFactory);
                g_hIconFolderBlack = LoadPngResourceBitmap(hInst, IDP_FOLDER_BLACK, pIconFactory);
                g_hIconFolderWhite = LoadPngResourceBitmap(hInst, IDP_FOLDER_WHITE, pIconFactory);
                g_hIconInfoBlack   = LoadPngResourceBitmap(hInst, IDP_INFO_BLACK, pIconFactory);
                g_hIconInfoWhite   = LoadPngResourceBitmap(hInst, IDP_INFO_WHITE, pIconFactory);
                g_hLogoBitmap      = LoadPngResourceBitmap(hInst, IDP_LOGO, pIconFactory);
                pIconFactory->lpVtbl->Release(pIconFactory);
            }
        }
        int rx = CONTENT_LEFT + 290 + 20;   /* right column x */

        /* wordmark logo, sized to keep its 3:1 aspect ratio within the
           right column's width; falls back to plain text (the old
           title) if the logo image didn't load for some reason, so the
           header is never left blank */
        int logoW = 320;
        int logoH = (logoW * 400) / 1200;
        g_logoRect = (RECT){ rx, CONTENT_TOP + 4, rx + logoW, CONTENT_TOP + 4 + logoH };

        g_overlayLabels[0].text = L"The Choicer Voicer Launcher";
        g_overlayLabels[0].rect = (RECT){ rx, CONTENT_TOP + 4, rx + 404, CONTENT_TOP + 4 + 38 };
        g_overlayLabels[0].font = g_fontTitle;
        g_overlayLabels[0].visible = (g_hLogoBitmap == NULL);
        g_logoVisible = TRUE; /* Launcher is the active tab by default at startup */

        /* the "Choose a version..." instructional text used to live here;
           it now lives in the (i) info popup instead (see
           ShowFirstRunPopup) */

        /* Normal Mode card: bordered panel using more of the width/height
           alongside the side image, instead of a thin single-height row */
        g_cardPanel[0] = MakeGroupBox(hwnd, hInst, L"Normal Mode", rx, CONTENT_TOP + 130, 404, 112, g_fontRegular);
        g_cardTitle[0] = g_lastGroupTitle;
        g_hNormalCombo = MakeCombo(hwnd, hInst, rx + 18, CONTENT_TOP + 166, 245, 200, IDC_NORMAL_COMBO, g_fontRegular);
        g_hLaunchNormalBtn = MakeButton(hwnd, hInst, L"Launch", rx + 278, CONTENT_TOP + 166, 108, 34, ID_LAUNCH_NORMAL, g_fontRegular);
        TrackLauncherPageWnd(g_hNormalCombo);
        TrackLauncherPageWnd(g_hLaunchNormalBtn);
        {
            HWND hAddNormalBtn = MakeButton(hwnd, hInst, L"+ Add Version",
                rx + 18, CONTENT_TOP + 208, 140, 28, ID_ADD_NORMAL_VERSION, g_fontSmall);
            g_hAddNormalBtn = hAddNormalBtn;
            TrackLauncherPageWnd(hAddNormalBtn);
        }

        /* Compatibility Mode card */
        g_cardPanel[1] = MakeGroupBox(hwnd, hInst, L"Compatibility Mode", rx, CONTENT_TOP + 260, 404, 112, g_fontRegular);
        g_cardTitle[1] = g_lastGroupTitle;
        g_hCompatCombo = MakeCombo(hwnd, hInst, rx + 18, CONTENT_TOP + 296, 245, 200, IDC_COMPAT_COMBO, g_fontRegular);
        g_hLaunchCompatBtn = MakeButton(hwnd, hInst, L"Launch", rx + 278, CONTENT_TOP + 296, 108, 34, ID_LAUNCH_COMPAT, g_fontRegular);
        TrackLauncherPageWnd(g_hCompatCombo);
        TrackLauncherPageWnd(g_hLaunchCompatBtn);
        {
            HWND hAddCompatBtn = MakeButton(hwnd, hInst, L"+ Add Version",
                rx + 18, CONTENT_TOP + 338, 140, 28, ID_ADD_COMPAT_VERSION, g_fontSmall);
            g_hAddCompatBtn = hAddCompatBtn;
            TrackLauncherPageWnd(hAddCompatBtn);
        }

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

        #define CONTENT_W (820 - CONTENT_LEFT - 15)

        /* Browse Mods tab: a single scrollable list window covering the
           same footprint the Launcher tab's content uses, so switching
           tabs is just show/hide of two mutually-exclusive halves */
        {
            RECT crc; GetClientRect(hwnd, &crc);
            int listH = crc.bottom - 20 - CONTENT_TOP;
            g_hModListWnd = CreateModListWnd(hwnd, hInst, CONTENT_LEFT, CONTENT_TOP, CONTENT_W, listH);
            g_hInstListWnd = CreateInstalledModsWnd(hwnd, hInst, CONTENT_LEFT, CONTENT_TOP, CONTENT_W, listH);
            CreateDubRenderWnd(hwnd, hInst, CONTENT_LEFT, CONTENT_TOP, CONTENT_W, listH);
            CreateSettingsWnd(hwnd, hInst, CONTENT_LEFT, CONTENT_TOP, CONTENT_W, listH);
            SettingsRefresh();      /* (its own WM_CREATE ran before g_hSettingsWnd was set, so refresh the captions now) */
        }

        /* animated backdrop: tinted image + slow pulsing/drifting stars */
        g_hBgLightBitmap = LoadBitmapW(hInst, MAKEINTRESOURCEW(IDB_BGLIGHT));
        g_hBgDarkBitmap  = LoadBitmapW(hInst, MAKEINTRESOURCEW(IDB_BGDARK));
        {
            RECT crc; GetClientRect(hwnd, &crc);
            InitStars(crc.right, crc.bottom);
        }

        if (g_starCount > 0) SetTimer(hwnd, ANIM_TIMER_ID, ANIM_INTERVAL_MS, NULL);   /* nothing else animates, so no stars = no timer */

        /* Without WS_CLIPSIBLINGS a child that is repainted (the Normal/Compatibility card
           panels, say) paints over siblings sitting on top of it - that is what made the
           version dropdowns vanish after the window was resized. */
        EnumChildWindows(hwnd, ClipSiblingsProc, (LPARAM)hwnd);

        /* Child windows created later sit BELOW earlier ones, so the sidebar panel (the first thing
           created) was on top of every sidebar button. That only went unnoticed because siblings
           weren't clipped; now that they are, the panel must go to the very bottom explicitly. */
        SetWindowPos(g_hSidebarPanel, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        g_hMainWnd = hwnd;
        {
            RECT lrc; GetClientRect(hwnd, &lrc);
            g_layoutW = lrc.right; g_layoutH = lrc.bottom;
        }
        LayoutMainWindow(hwnd);
        if (g_musicOn && !MusicStart(hwnd)) g_musicOn = FALSE;

        ApplyThemeColors(hwnd);

        /* drag-and-drop is no longer registered on the main window - it
           lives on the "Download From Link" popup instead, registered
           only while that popup is open (see ShowDownloadFromLinkPicker) */
        return 0;
    }

    case WM_ENTERSIZEMOVE:
        g_sizing = TRUE;                 /* dragging: scale artwork/backdrop the fast way */
        return 0;

    case WM_EXITSIZEMOVE:
        g_sizing = FALSE;                /* released: rebuild everything once at full quality */
        g_bgCacheW = 0;
        g_sideScaledW = 0;
        LayoutMainWindow(hwnd);
        return 0;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            LayoutMainWindow(hwnd);
            ApplyWindowRounding(hwnd);
            if (g_hCapMax) InvalidateRect(g_hCapMax, NULL, TRUE);     /* maximize <-> restore glyph */
        }
        return 0;

    case WM_NCCALCSIZE:
        /* no standard frame: the whole window is client area (our own title bar is painted in it) */
        return 0;

    case WM_NCACTIVATE:
        /* lParam -1 stops Windows repainting a (non-existent) frame when focus changes */
        return DefWindowProcW(hwnd, msg, wParam, (LPARAM)-1);

    case WM_NCHITTEST: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ScreenToClient(hwnd, &pt);
        RECT wr; GetClientRect(hwnd, &wr);
        if (!IsZoomed(hwnd) && !g_fullscreen) {
            const int b = 7;                       /* invisible resize border */
            BOOL L = pt.x < b, R = pt.x >= wr.right - b, T = pt.y < b, B = pt.y >= wr.bottom - b;
            if (T && L) return HTTOPLEFT;
            if (T && R) return HTTOPRIGHT;
            if (B && L) return HTBOTTOMLEFT;
            if (B && R) return HTBOTTOMRIGHT;
            if (T) return HTTOP;
            if (B) return HTBOTTOM;
            if (L) return HTLEFT;
            if (R) return HTRIGHT;
        }
        if (pt.y < g_titleH) return HTCAPTION;     /* drag the window / double-click to maximize */
        return HTCLIENT;
    }

    case WM_GETMINMAXINFO: {
        /* the window can be made bigger but not smaller than the original layout; when maximized it
           fills the work area (not under the taskbar) */
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = BASE_CLIENT_W;
        mmi->ptMinTrackSize.y = BASE_CLIENT_H;
        MONITORINFO mi; memset(&mi, 0, sizeof(mi)); mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
            mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
            mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
        }
        return 0;
    }

    case MM_MCINOTIFY:
        /* the track reached its end - start it over (this is what loops the music) */
        if (g_musicOn && wParam == MCI_NOTIFY_SUCCESSFUL)
            mciSendStringW(L"play cvbgm from 0 notify", NULL, 0, hwnd);
        return 0;

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

            /* the tab buttons show which tab is active by staying in the
               "pressed" state permanently, like a selected tab */
            if ((dis->CtlID == ID_TAB_LAUNCHER && g_activeTab == TAB_LAUNCHER) ||
                (dis->CtlID == ID_TAB_BROWSE && g_activeTab == TAB_BROWSE) ||
                (dis->CtlID == ID_TAB_INSTALLED && g_activeTab == TAB_INSTALLED) ||
                (dis->CtlID == ID_TAB_DUB && g_activeTab == TAB_DUB) ||
                (dis->CtlID == ID_FULLSCREEN && g_fullscreen)) {
                pressed = TRUE;
            }

            if (dis->CtlID == ID_CAP_MIN || dis->CtlID == ID_CAP_MAX || dis->CtlID == ID_CAP_CLOSE) {
                DrawCaptionButton(dis, IsZoomed(hwnd));
                return TRUE;
            }
            if (dis->CtlID == ID_TAB_SETTINGS) {
                DrawGearIcon(dis->hDC, dis->rcItem, hovered, (dis->itemState & ODS_SELECTED) != 0,
                    g_activeTab == TAB_SETTINGS);
                return TRUE;
            }

            if (dis->CtlID == ID_TAB_LAUNCHER || dis->CtlID == ID_TAB_BROWSE || dis->CtlID == ID_TAB_INSTALLED ||
                dis->CtlID == ID_TAB_DUB || dis->CtlID == ID_FULLSCREEN) {
                /* `pressed` is also TRUE for the active tab (see above) */
                DrawSidebarTabIcon(dis, hovered, pressed);
                return TRUE;
            }

            HBITMAP iconBmp = NULL;
            BOOL isCircleIcon = FALSE;
            switch (dis->CtlID) {
                case ID_TAB_LAUNCHER:    iconBmp = g_darkMode ? g_hIconHomeBlack   : g_hIconHomeWhite;   break;
                case ID_TAB_BROWSE:      iconBmp = g_darkMode ? g_hIconBrowseBlack : g_hIconBrowseWhite; break;
                case ID_TAB_INSTALLED:   iconBmp = g_darkMode ? g_hIconModsBlack   : g_hIconModsWhite;   break;
                case IDC_DARKMODE_CHECK: iconBmp = g_darkMode ? g_hIconThemeBlack  : g_hIconThemeWhite;  isCircleIcon = TRUE; break;
                case ID_MODDING:         iconBmp = g_darkMode ? g_hIconFolderBlack : g_hIconFolderWhite; isCircleIcon = TRUE; break;
                case ID_README_INFO:     iconBmp = g_darkMode ? g_hIconInfoBlack   : g_hIconInfoWhite;   isCircleIcon = TRUE; break;
            }

            if (iconBmp) {
                /* custom art: background, border and glyph are all baked
                   into the image itself - just blend it in and add a
                   ring on top for hover/pressed feedback */
                DrawIconButtonBitmap(dis->hDC, dis->rcItem, iconBmp, isCircleIcon, hovered, pressed, disabled);
                if (dis->itemState & ODS_FOCUS) {
                    RECT focusRect = dis->rcItem;
                    InflateRect(&focusRect, -3, -3);
                    DrawFocusRect(dis->hDC, &focusRect);
                }
                return TRUE;
            }

            /* fallback: the original hand-drawn rendering, used if a
               custom icon bitmap failed to load, or for any other
               owner-draw button that doesn't have custom art */
            COLORREF fillNormal = g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL;
            COLORREF fillHover = BlendColor(fillNormal, g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT, 0.10f);
            COLORREF fillPressed = g_darkMode ? COLOR_DARK_PRESSED : COLOR_LIGHT_PRESSED;
            COLORREF border = g_darkMode ? COLOR_DARK_BORDER : COLOR_LIGHT_BORDER;
            COLORREF textColor = disabled
                ? BlendColor(fillNormal, g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT, g_darkMode ? 0.40f : 0.50f)
                : (g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT);

            COLORREF fillColor = pressed ? fillPressed : (hovered ? fillHover : fillNormal);
            HBRUSH hFill = CreateSolidBrush(fillColor);
            HPEN hPen = CreatePen(PS_SOLID, 1, border);
            HGDIOBJ oldPen = SelectObject(dis->hDC, hPen);
            HGDIOBJ oldBrush = SelectObject(dis->hDC, hFill);

            BOOL isCircleFallback = (dis->CtlID == ID_MODDING || dis->CtlID == ID_README_INFO || dis->CtlID == IDC_DARKMODE_CHECK);
            if (isCircleFallback) {
                COLORREF sidebarBg = g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL;
                HBRUSH sidebarBrush = CreateSolidBrush(sidebarBg);
                FillRect(dis->hDC, &dis->rcItem, sidebarBrush);
                DeleteObject(sidebarBrush);
                Ellipse(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
            } else {
                RoundRect(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom, 6, 6);
            }
            SelectObject(dis->hDC, oldBrush);
            SelectObject(dis->hDC, oldPen);
            DeleteObject(hFill);
            DeleteObject(hPen);

            BOOL isSidebarIcon = (dis->CtlID == ID_TAB_LAUNCHER || dis->CtlID == ID_TAB_BROWSE ||
                                   dis->CtlID == ID_TAB_INSTALLED || dis->CtlID == ID_TAB_DUB || dis->CtlID == ID_FULLSCREEN ||
                                   dis->CtlID == ID_MODDING || dis->CtlID == ID_README_INFO);

            if (dis->CtlID == ID_FULLSCREEN) {
                DrawFullscreenGlyph(dis->hDC, dis->rcItem, textColor, g_fullscreen);
            } else if (dis->CtlID == ID_TAB_DUB) {
                DrawMicGlyph(dis->hDC, dis->rcItem, textColor);
            } else if (dis->CtlID == ID_MODDING) {
                DrawFolderGlyph(dis->hDC, dis->rcItem, textColor);
            } else if (dis->CtlID == ID_README_INFO) {
                int cx = (dis->rcItem.left + dis->rcItem.right) / 2;
                int cy = (dis->rcItem.top + dis->rcItem.bottom) / 2;
                HBRUSH glyphBrush = CreateSolidBrush(textColor);
                HGDIOBJ oldGB = SelectObject(dis->hDC, glyphBrush);
                HGDIOBJ oldGP = SelectObject(dis->hDC, GetStockObject(NULL_PEN));
                Ellipse(dis->hDC, cx - 3, cy - 10, cx + 3, cy - 4);
                RoundRect(dis->hDC, cx - 3, cy - 2, cx + 3, cy + 10, 3, 3);
                SelectObject(dis->hDC, oldGP);
                SelectObject(dis->hDC, oldGB);
                DeleteObject(glyphBrush);
            } else {
                wchar_t buf[128];
                GetWindowTextW(dis->hwndItem, buf, 128);
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, textColor);
                HGDIOBJ oldFont = SelectObject(dis->hDC, isSidebarIcon ? g_fontIcon : g_fontRegular);
                RECT textRect = dis->rcItem;
                DrawTextW(dis->hDC, buf, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dis->hDC, oldFont);
            }

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
            case ID_README_INFO:   ShowFirstRunPopup(hwnd, g_hInstance); break;
            case ID_TAB_LAUNCHER:  SwitchTab(hwnd, TAB_LAUNCHER); break;
            case ID_TAB_BROWSE:    SwitchTab(hwnd, TAB_BROWSE); break;
            case ID_TAB_INSTALLED: SwitchTab(hwnd, TAB_INSTALLED); break;
            case ID_TAB_DUB:       SwitchTab(hwnd, TAB_DUB); break;
            case ID_FULLSCREEN:    ToggleFullscreen(hwnd); break;
            case ID_TAB_SETTINGS:  SwitchTab(hwnd, TAB_SETTINGS); break;
            case ID_CAP_MIN:       ShowWindow(hwnd, SW_MINIMIZE); break;
            case ID_CAP_MAX:       ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); break;
            case ID_CAP_CLOSE:     PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
            case ID_OPEN_BROWSE_FROM_LAUNCHER: SwitchTab(hwnd, TAB_BROWSE); break;
            case ID_ADD_NORMAL_VERSION: ShowAddVersionPopup(hwnd, g_hInstance, TRUE); break;
            case ID_ADD_COMPAT_VERSION: ShowAddVersionPopup(hwnd, g_hInstance, FALSE); break;
            }
        }
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, ANIM_TIMER_ID);
        MusicStop();
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
        if (g_fontGroupHeader) DeleteObject(g_fontGroupHeader);
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
    case WM_DRAWITEM:
        if (((LPDRAWITEMSTRUCT)lParam)->CtlType == ODT_BUTTON) {
            DrawThemedOwnerButton((LPDRAWITEMSTRUCT)lParam);
            return TRUE;
        }
        return FALSE;
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

    const int w = 380, h = 260;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
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
        L"Choose a version from the dropdown windows to play the version you want.\n\n"
        L"Before dropping in your game files, please read the README on GitHub - "
        L"it explains exactly where your game/version folders need to go so the "
        L"launcher can find them.",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 20, w - 40, 150, hPop, NULL, hInst, NULL);
    SendMessageW(hMsg, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);

    HWND hOpenBtn = MakeButton(hPop, hInst, L"Open README",
        20, h - 60, 160, 32, ID_README_OPEN, g_fontRegular);

    HWND hCloseBtn = MakeButton(hPop, hInst, L"Close",
        w - 20 - 100, h - 60, 100, 32, ID_README_CLOSE, g_fontRegular);
    (void)hCloseBtn;

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

    /* no standard title bar or frame: WM_NCCALCSIZE gives the whole window to us, the title bar is
       painted by the launcher itself, and WS_THICKFRAME is kept only so edge-resizing, snapping and
       minimize/maximize keep working */
    DWORD style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN;
    int winW = BASE_CLIENT_W;
    int winH = BASE_CLIENT_H;
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
        /* F11 toggles full screen, Esc leaves it - caught here so it works whichever control has focus,
           but only while the main window itself is active (not over a popup) */
        if (msg.message == WM_KEYDOWN && g_hMainWnd &&
            (msg.wParam == VK_F11 || (msg.wParam == VK_ESCAPE && g_fullscreen)) &&
            GetAncestor(msg.hwnd, GA_ROOT) == g_hMainWnd && IsWindowEnabled(g_hMainWnd)) {
            if (msg.wParam == VK_F11 || g_fullscreen) ToggleFullscreen(g_hMainWnd);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    OleUninitialize();
    return 0;
}
