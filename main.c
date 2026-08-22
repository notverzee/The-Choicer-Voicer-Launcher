#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <wchar.h>
#include <string.h>
#include <stdlib.h>

#include "miniz.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

/* ---- control / resource IDs ---- */
#define ID_LAUNCH_NORMAL 1001
#define ID_LAUNCH_COMPAT 1002
#define ID_MODDING        1005
#define ID_GET_MODS       1006

#define IDC_NORMAL_COMBO  3001
#define IDC_COMPAT_COMBO  3002
#define IDC_PACK_COMBO    2001
#define IDC_DROPZONE      2002

#define IDB_SIDEIMAGE   100
#define IDI_APPICON     101

/* ---- folders (relative to the launcher's own exe) that hold versions ---- */
static const wchar_t* NORMAL_DIR_NAME = L"Normal";
static const wchar_t* COMPAT_DIR_NAME = L"Compatibility";

static const wchar_t* GAME_DIR_SUBPATH = L"\\YeahMaybe\\ChoicerVoicer\\game";
static const wchar_t* GAMEBANANA_URL = L"https://gamebanana.com/games/20674";

static const wchar_t* PACK_FOLDERS[] = {
    L"packs_chatter", L"packs_host", L"packs_judges",
    L"packs_menu", L"packs_player", L"packs_studio", L"packs_voice"
};
#define NUM_PACK_FOLDERS 7

static HFONT g_fontTitle    = NULL;
static HFONT g_fontRegular  = NULL;
static HFONT g_fontSmall    = NULL;
static HBITMAP g_hBitmap    = NULL;
static HWND   g_hPackCombo  = NULL;
static RECT   g_dropZoneRect = {0, 0, 0, 0};

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

/* Reads a whole .txt file as the display name (UTF-8 with ANSI fallback),
   trimming surrounding whitespace/newlines. */
static BOOL ReadDisplayNameTxt(const wchar_t* txtPath, wchar_t* out, int outCount)
{
    DWORD size = 0;
    BYTE* buf = ReadEntireFileW(txtPath, &size);
    if (!buf) return FALSE;

    BYTE* p = buf;
    DWORD n = size;
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { p += 3; n -= 3; }

    int wn = MultiByteToWideChar(CP_UTF8, 0, (char*)p, (int)n, out, outCount - 1);
    if (wn <= 0) {
        wn = MultiByteToWideChar(CP_ACP, 0, (char*)p, (int)n, out, outCount - 1);
    }
    free(buf);
    if (wn <= 0) return FALSE;
    out[wn] = 0;

    int len = (int)wcslen(out);
    while (len > 0 && (out[len-1] == L'\r' || out[len-1] == L'\n' ||
                       out[len-1] == L' '  || out[len-1] == L'\t')) {
        out[--len] = 0;
    }
    int start = 0;
    while (out[start] == L' ' || out[start] == L'\t') start++;
    if (start > 0) memmove(out, out + start, (size_t)(len - start + 1) * sizeof(wchar_t));

    return out[0] != 0;
}

/* A "version folder" is any subfolder of Normal\ or Compatibility\ containing
   exactly one .exe (the game build) and, ideally, one .txt (its display name). */
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

    if (!exePath[0]) return; /* no exe in here -> not a usable version, skip */

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

static void LaunchSelectedVersion(HWND hwnd, HWND combo, VersionList* list)
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

/* ===================== drag & drop entry point ===================== */

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

/* ===================== UI helpers ===================== */

static HWND MakeStatic(HWND parent, HINSTANCE hInst, const wchar_t* text,
                        int x, int y, int w, int ht, DWORD extraStyle, HFONT font)
{
    HWND ctl = CreateWindowW(L"STATIC", text,
        WS_CHILD | WS_VISIBLE | extraStyle,
        x, y, w, ht, parent, NULL, hInst, NULL);
    if (font) SendMessageW(ctl, WM_SETFONT, (WPARAM)font, TRUE);
    return ctl;
}

static HWND MakeButton(HWND parent, HINSTANCE hInst, const wchar_t* text,
                        int x, int y, int w, int ht, int id, HFONT font)
{
    HWND ctl = CreateWindowW(L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x, y, w, ht, parent, (HMENU)(INT_PTR)id, hInst, NULL);
    if (font) SendMessageW(ctl, WM_SETFONT, (WPARAM)font, TRUE);
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

        /* side image */
        g_hBitmap = LoadBitmapW(hInst, MAKEINTRESOURCEW(IDB_SIDEIMAGE));
        HWND hImg = CreateWindowW(L"STATIC", NULL,
            WS_CHILD | WS_VISIBLE | SS_BITMAP,
            15, 15, 250, 333, hwnd, NULL, hInst, NULL);
        SendMessageW(hImg, STM_SETIMAGE, (WPARAM)IMAGE_BITMAP, (LPARAM)g_hBitmap);

        int rx = 285;   /* right column x */

        MakeStatic(hwnd, hInst, L"The Choicer Voicer", rx, 20, 350, 34, SS_LEFT, g_fontTitle);
        MakeStatic(hwnd, hInst, L"Launcher; Choose a version from the dropdown windows to play the version you want.",
                   rx, 58, 350, 54, SS_LEFT, g_fontRegular);

        /* Normal Mode row: label + dropdown (built from Normal\<version>\) + Launch.
           Rows are spaced out to use the same vertical span as the side image (down to y=348),
           instead of clustering at the top with empty space below. */
        MakeStatic(hwnd, hInst, L"Normal Mode:", rx, 176, 95, 20, SS_LEFT, g_fontRegular);
        g_hNormalCombo = MakeCombo(hwnd, hInst, rx + 99, 172, 145, 200, IDC_NORMAL_COMBO, g_fontRegular);
        g_hLaunchNormalBtn = MakeButton(hwnd, hInst, L"Launch", rx + 252, 172, 90, 28, ID_LAUNCH_NORMAL, g_fontRegular);

        /* Compatibility Mode row: label + dropdown (built from Compatibility\<version>\) + Launch */
        MakeStatic(hwnd, hInst, L"Compat. Mode:", rx, 264, 95, 20, SS_LEFT, g_fontRegular);
        g_hCompatCombo = MakeCombo(hwnd, hInst, rx + 99, 260, 145, 200, IDC_COMPAT_COMBO, g_fontRegular);
        g_hLaunchCompatBtn = MakeButton(hwnd, hInst, L"Launch", rx + 252, 260, 90, 28, ID_LAUNCH_COMPAT, g_fontRegular);

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
        }

        /* Everything below here spans the full window width, so it must start
           below BOTH columns - the side image bottom (15+333=348) is the taller
           of the two, not just the right column's own content. */
        #define MOD_SECTION_TOP 364

        /* Get Mods + Modding - open external link / folder, don't launch a game version */
        MakeButton(hwnd, hInst, L"Get Mods", 15, MOD_SECTION_TOP, 300, 36, ID_GET_MODS, g_fontRegular);
        MakeButton(hwnd, hInst, L"(Modding)", 335, MOD_SECTION_TOP, 300, 36, ID_MODDING, g_fontRegular);

        MakeStatic(hwnd, hInst, L"Install downloaded mod to:", 15, MOD_SECTION_TOP + 50, 190, 20, SS_LEFT, g_fontRegular);
        g_hPackCombo = MakeCombo(hwnd, hInst, 210, MOD_SECTION_TOP + 46, 200, 200, IDC_PACK_COMBO, g_fontRegular);
        for (int i = 0; i < NUM_PACK_FOLDERS; i++) {
            SendMessageW(g_hPackCombo, CB_ADDSTRING, 0, (LPARAM)PACK_FOLDERS[i]);
        }
        SendMessageW(g_hPackCombo, CB_SETCURSEL, 0, 0);

        HWND hDrop = CreateWindowW(L"STATIC", L"Drag a .zip or .rar mod file here",
            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE | WS_BORDER,
            15, MOD_SECTION_TOP + 78, 620, 70, hwnd, (HMENU)(INT_PTR)IDC_DROPZONE, hInst, NULL);
        SendMessageW(hDrop, WM_SETFONT, (WPARAM)g_fontRegular, TRUE);
        g_dropZoneRect.left = 15; g_dropZoneRect.top = MOD_SECTION_TOP + 78;
        g_dropZoneRect.right = 15 + 620; g_dropZoneRect.bottom = MOD_SECTION_TOP + 78 + 70;

        DragAcceptFiles(hwnd, TRUE);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        POINT pt;
        DragQueryPoint(hDrop, &pt);
        if (PtInRect(&g_dropZoneRect, pt)) {
            UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
            for (UINT i = 0; i < count; i++) {
                wchar_t path[MAX_PATH];
                if (DragQueryFileW(hDrop, i, path, MAX_PATH)) {
                    ProcessDroppedFile(hwnd, path);
                }
            }
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_COMMAND: {
        if (HIWORD(wParam) == BN_CLICKED) {
            switch (LOWORD(wParam)) {
            case ID_LAUNCH_NORMAL: LaunchSelectedVersion(hwnd, g_hNormalCombo, &g_normalVersions); break;
            case ID_LAUNCH_COMPAT: LaunchSelectedVersion(hwnd, g_hCompatCombo, &g_compatVersions); break;
            case ID_MODDING:       OpenModdingFolder(hwnd); break;
            case ID_GET_MODS:      OpenGetMods(hwnd); break;
            }
        }
        return 0;
    }

    case WM_DESTROY:
        VersionList_Free(&g_normalVersions);
        VersionList_Free(&g_compatVersions);
        if (g_hBitmap) DeleteObject(g_hBitmap);
        if (g_fontTitle) DeleteObject(g_fontTitle);
        if (g_fontRegular) DeleteObject(g_fontRegular);
        if (g_fontSmall) DeleteObject(g_fontSmall);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                     PWSTR pCmdLine, int nCmdShow)
{
    (void)hPrevInstance; (void)pCmdLine;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    const wchar_t CLASS_NAME[] = L"ChoicerVoicerLauncherWnd";

    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    RegisterClassW(&wc);

    RECT rc = {0, 0, 650, 527};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
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

    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {0};
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
