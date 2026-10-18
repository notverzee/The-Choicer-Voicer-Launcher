/* ===================================================================
   extras.inc - resizable window layout, full screen, background music.

   Included from main.c just before SwitchTab(). Holds:
     * LayoutMainWindow(): re-flows the sidebar, the three tab windows
       and the Launcher page whenever the window size changes
     * ToggleFullscreen(): borderless full screen on the current monitor
     * background music: the MP3 is compiled into the EXE (resource.rc,
       IDR_MUSIC), unpacked to the temp folder on first use, and played
       through MCI. MCI's "notify" flag tells us when the track ends so
       it can be restarted, which is what makes it loop.
     * vector art for the music button and the full screen button
   =================================================================== */

#include <mmsystem.h>

#define SIDEBAR_W     76
#define CONTENT_LEFT  (SIDEBAR_W + 15)
#define CONTENT_TOP   (g_titleH + 12)      /* g_titleH: height of the custom title bar (0 in full screen) */

#define ID_FULLSCREEN 5005
#define ID_MUSIC      5006
#define IDR_MUSIC     131

#define BASE_CLIENT_W 820          /* the original (and now minimum) client size */
#define BASE_CLIENT_H 648

/* g_musicOn and g_lastGroupTitle are declared near the top of main.c */
static BOOL g_dwmRound = FALSE;                  /* Windows 11 rounds the window corners itself */
static HWND g_hSidebarPanel = NULL, g_hSideImage = NULL;
static HWND g_hBtnFullscreen = NULL;
static HWND g_hSetMusicBtn = NULL;               /* the round music button on the Settings page */
static HDWP g_hdwp = NULL;                        /* batches the control moves of one layout pass */
static void SettingsRefresh(void);
static HWND g_cardPanel[2], g_cardTitle[2];      /* Normal, Compatibility */
static HWND g_hAddNormalBtn = NULL, g_hAddCompatBtn = NULL;
static HBITMAP g_hSideScaled = NULL;
static int g_sideScaledW = 0, g_sideScaledH = 0;
static int g_layoutW = 0, g_layoutH = 0;          /* client size at the last layout (for scaling the stars) */
static BOOL g_fullscreen = FALSE;
static WINDOWPLACEMENT g_wpPrev;
static LONG_PTR g_stylePrev = 0;

/* ---------------- layout ---------------- */

static HBITMAP ScaleBitmapHalftone(HBITMAP src, int w, int h)
{
    BITMAP bm;
    if (!src || w < 1 || h < 1 || !GetObjectW(src, sizeof(bm), &bm)) return NULL;
    HDC screen = GetDC(NULL);
    HDC s = CreateCompatibleDC(screen);
    HDC d = CreateCompatibleDC(screen);
    HBITMAP out = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ os = SelectObject(s, src);
    HGDIOBJ od = SelectObject(d, out);
    SetStretchBltMode(d, g_sizing ? COLORONCOLOR : HALFTONE);   /* fast while dragging, smooth once released */
    SetBrushOrgEx(d, 0, 0, NULL);
    StretchBlt(d, 0, 0, w, h, s, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
    SelectObject(s, os);
    SelectObject(d, od);
    DeleteDC(s);
    DeleteDC(d);
    ReleaseDC(NULL, screen);
    return out;
}

/* all the moves of one layout pass go through here so Windows applies them in a single
   step - moving ~30 controls one by one is what made the home page flicker while resizing.
   SWP_NOCOPYBITS: don't blit a control's old pixels to its new spot (that left ghost copies behind). */
static void LMove(HWND h, int x, int y, int w, int ht)
{
    if (!h) return;
    if (g_hdwp) {
        g_hdwp = DeferWindowPos(g_hdwp, h, NULL, x, y, w, ht, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        if (g_hdwp) return;                      /* (a failed defer invalidates the batch: fall back) */
    }
    MoveWindow(h, x, y, w, ht, TRUE);
}

static void MoveCard(int idx, int x, int y, int w, int h)
{
    /* the card's rounded body is painted into the backdrop (PaintAnimatedBackdrop), so its old
       rectangular panel window is kept at zero size */
    SetRect(&g_cardRect[idx], x, y, x + w, y + h);
    if (g_cardPanel[idx]) LMove(g_cardPanel[idx], x, y, 0, 0);
    /* Anton's line box is ~1.7x its size, so a fixed 20px title label clipped the descenders */
    if (g_cardTitle[idx]) LMove(g_cardTitle[idx], x + 10, y + 6, w - 20, DubFontH(g_fontRegular) + 2);
}

/* The Launcher page. With the Get Mods card gone the page is one composition: the artwork on the
   left and, on its right, the logo with the Normal and Compatibility cards spaced evenly down the
   artwork's height. The whole group scales with the window (keeping the artwork's proportions),
   stays centered, and is rounded to 8px steps so a window drag doesn't re-scale the artwork for
   every single pixel. Only a handful of controls move, which also keeps resizing cheap. */
static void LayoutLauncherPage(int cw, int ch)
{
    const int cardH = 112;
    const int top = CONTENT_TOP;

    /* artwork size: as tall as the area allows, but the right column (1.4x the artwork's width)
       must still fit beside it */
    int imgW = ch * 290 / 386;
    int wLimit = (int)((cw - 20) / 2.4);
    if (imgW > wLimit) imgW = wLimit;
    imgW &= ~7;
    if (imgW < 280) imgW = 280;
    int imgH = imgW * 386 / 290;

    int rightW = (int)(imgW * 1.4);
    if (rightW < 400) rightW = 400;
    if (rightW > cw - 20 - imgW) rightW = cw - 20 - imgW;

    /* centered horizontally; slightly above vertical center looks better than dead center */
    int groupW = imgW + 20 + rightW;
    int gx = CONTENT_LEFT + (cw - groupW) / 2;
    if (gx < CONTENT_LEFT) gx = CONTENT_LEFT;
    int gy = top + (ch - imgH) * 4 / 10;
    if (gy < top) gy = top;

    /* artwork */
    if (g_hSideImage && g_hBitmap) {
        LMove(g_hSideImage, gx, gy, imgW, imgH);
        if (imgW != g_sideScaledW || imgH != g_sideScaledH) {
            BITMAP bm; GetObjectW(g_hBitmap, sizeof(bm), &bm);
            HBITMAP use = g_hBitmap, fresh = NULL;
            if (imgW != bm.bmWidth || imgH != bm.bmHeight) {
                fresh = ScaleBitmapHalftone(g_hBitmap, imgW, imgH);
                if (fresh) use = fresh;
            }
            HBITMAP old = (HBITMAP)SendMessageW(g_hSideImage, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)use);
            if (old && old != g_hBitmap && old != use) DeleteObject(old);
            g_hSideScaled = fresh;
            SetWindowRgn(g_hSideImage, CreateRoundRectRgn(0, 0, imgW + 1, imgH + 1, 28, 28), TRUE);
            g_sideScaledW = imgW; g_sideScaledH = imgH;
        }
    }

    /* right column: logo, then the two cards with equal gaps down to the artwork's bottom edge */
    int rx = gx + imgW + 20;
    int logoW = rightW * 8 / 10;
    if (logoW < 300) logoW = 300;
    if (logoW > 520) logoW = 520;
    if (logoW > rightW) logoW = rightW;
    int logoH = logoW / 3;
    int logoX = rx + (rightW - logoW) / 2;
    SetRect(&g_logoRect, logoX, gy + 4, logoX + logoW, gy + 4 + logoH);
    SetRect(&g_overlayLabels[0].rect, rx, gy + 4, rx + rightW, gy + 4 + 38);

    int gap = (imgH - logoH - 2 * cardH) / 3;
    if (gap < 12) gap = 12;
    int cardY[2];
    cardY[0] = gy + logoH + gap;
    cardY[1] = cardY[0] + cardH + gap;

    HWND combos[2] = { g_hNormalCombo, g_hCompatCombo };
    HWND launches[2] = { g_hLaunchNormalBtn, g_hLaunchCompatBtn };
    HWND adds[2] = { g_hAddNormalBtn, g_hAddCompatBtn };
    for (int i = 0; i < 2; i++) {
        int y = cardY[i];
        MoveCard(i, rx, y, rightW, cardH);
        if (combos[i])   LMove(combos[i], rx + 18, y + 36, rightW - 159, 200);
        if (launches[i]) LMove(launches[i], rx + rightW - 18 - 108, y + 36, 108, 34);
        if (adds[i])     LMove(adds[i], rx + 18, y + 78, 140, 28);
    }
}

/* Windows 11 rounds the corners of the (title-bar-less) window itself; on Windows 10 we clip
   the window to a rounded region instead (not while maximized / full screen) */
static void ApplyWindowRounding(HWND hwnd)
{
    if (g_dwmRound) return;
    if (IsZoomed(hwnd) || g_fullscreen) { SetWindowRgn(hwnd, NULL, TRUE); return; }
    RECT r; GetWindowRect(hwnd, &r);
    SetWindowRgn(hwnd, CreateRoundRectRgn(0, 0, r.right - r.left + 1, r.bottom - r.top + 1, 22, 22), TRUE);
}

static void LayoutMainWindow(HWND hwnd)
{
    if (!g_hSidebarPanel || !g_hModListWnd || !g_hInstListWnd) return;   /* still being created */
    RECT crc; GetClientRect(hwnd, &crc);
    int W = crc.right, H = crc.bottom;
    if (W < 100 || H < 100) return;
    int cw = W - CONTENT_LEFT - 15;
    int ch = H - CONTENT_TOP - 20;
    if (cw < 100 || ch < 100) return;

    g_hdwp = BeginDeferWindowPos(90);

    /* sidebar: tab buttons hang under the title bar, the settings gear is anchored to the bottom */
    int ty = g_titleH + 14;
    LMove(g_hSidebarPanel, 0, g_titleH, SIDEBAR_W, H - g_titleH);
    LMove(g_hTabLauncherBtn,  10, ty,       56, 56);
    LMove(g_hTabBrowseBtn,    10, ty + 70,  56, 56);
    LMove(g_hTabInstalledBtn, 10, ty + 140, 56, 56);
    LMove(g_hTabDubBtn,       10, ty + 210, 56, 56);
    /* bottom of the sidebar: the settings gear, with the full screen button right above it */
    LMove(g_hTabSettingsBtn, 14, H - 12 - 48, 48, 48);
    LMove(g_hBtnFullscreen,  10, H - 12 - 48 - 12 - 56, 56, 56);

    /* title bar buttons, right-aligned */
    LMove(g_hCapClose, W - 8 - 44,          4, 44, 28);
    LMove(g_hCapMax,   W - 8 - 44 - 48,     4, 44, 28);
    LMove(g_hCapMin,   W - 8 - 44 - 96,     4, 44, 28);

    LMove(g_hModListWnd, CONTENT_LEFT, CONTENT_TOP, cw, ch);
    LMove(g_hInstListWnd, CONTENT_LEFT, CONTENT_TOP, cw, ch);
    LMove(g_hDubWnd, CONTENT_LEFT, CONTENT_TOP, cw, ch);
    LMove(g_hSettingsWnd, CONTENT_LEFT, CONTENT_TOP, cw, ch);
    LayoutLauncherPage(cw, ch);

    if (g_hdwp) { EndDeferWindowPos(g_hdwp); g_hdwp = NULL; }

    /* rounded corners for the four tab pages (a thin smooth outline is painted around them
       in the backdrop, which also hides the stair-stepping of the window regions) */
    SetRect(&g_contentRect, CONTENT_LEFT, CONTENT_TOP, CONTENT_LEFT + cw, CONTENT_TOP + ch);
    HWND pages[4] = { g_hModListWnd, g_hInstListWnd, g_hDubWnd, g_hSettingsWnd };
    for (int i = 0; i < 4; i++)
        if (pages[i]) SetWindowRgn(pages[i], CreateRoundRectRgn(0, 0, cw + 1, ch + 1, 30, 30), TRUE);

    /* keep the starfield spread over the new area instead of bunched in the old one */
    if (g_layoutW > 0 && g_layoutH > 0 && (g_layoutW != W || g_layoutH != H)) {
        for (int i = 0; i < NUM_STARS; i++) {
            g_stars[i].x *= (float)W / (float)g_layoutW;
            g_stars[i].y *= (float)H / (float)g_layoutH;
        }
    }
    g_layoutW = W; g_layoutH = H;
    /* A full erase + repaint (not just InvalidateRect(.., FALSE), which doesn't repaint the backdrop):
       otherwise pieces of the old layout stay on screen after a resize. */
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* ---------------- full screen ---------------- */

static void ToggleFullscreen(HWND hwnd)
{
    if (!g_fullscreen) {
        g_wpPrev.length = sizeof(g_wpPrev);
        GetWindowPlacement(hwnd, &g_wpPrev);
        g_stylePrev = GetWindowLongPtrW(hwnd, GWL_STYLE);
        MONITORINFO mi; memset(&mi, 0, sizeof(mi)); mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        SetWindowLongPtrW(hwnd, GWL_STYLE,
            (g_stylePrev & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU)) | WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_fullscreen = TRUE;
        g_titleH = 0;
    } else {
        SetWindowLongPtrW(hwnd, GWL_STYLE, g_stylePrev);
        SetWindowPlacement(hwnd, &g_wpPrev);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = FALSE;
        g_titleH = TITLE_H_DEFAULT;
    }
    {
        int sh = g_fullscreen ? SW_HIDE : SW_SHOW;
        ShowWindow(g_hCapMin, sh); ShowWindow(g_hCapMax, sh); ShowWindow(g_hCapClose, sh);
    }
    if (g_hBtnFullscreen) InvalidateRect(g_hBtnFullscreen, NULL, TRUE);
    LayoutMainWindow(hwnd);
    ApplyWindowRounding(hwnd);
}

/* ---------------- background music ---------------- */

static BOOL MusicExtractFile(wchar_t* outPath, size_t cap)
{
    HINSTANCE hInst = GetModuleHandleW(NULL);
    HRSRC hr = FindResourceW(hInst, MAKEINTRESOURCEW(IDR_MUSIC), (LPCWSTR)RT_RCDATA);
    if (!hr) return FALSE;
    HGLOBAL hg = LoadResource(hInst, hr);
    DWORD sz = SizeofResource(hInst, hr);
    void* data = hg ? LockResource(hg) : NULL;
    if (!data || !sz) return FALSE;

    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH) return FALSE;
    _snwprintf(outPath, cap - 1, L"%lsChoicerVoicerLauncher_bgm.mp3", tmp);
    outPath[cap - 1] = 0;

    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(outPath, GetFileExInfoStandard, &fa) && fa.nFileSizeHigh == 0 && fa.nFileSizeLow == sz)
        return TRUE;                                   /* already unpacked */
    HANDLE h = CreateFileW(outPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, sz, &written, NULL) && written == sz;
    CloseHandle(h);
    return ok;
}

/* 0..100 from the Settings slider -> MCI's 0..1000 */
static void MusicApplyVolume(void)
{
    if (!g_musicOn) return;
    wchar_t cmd[64];
    _snwprintf(cmd, 63, L"setaudio cvbgm volume to %d", g_musicVolume * 10);
    cmd[63] = 0;
    mciSendStringW(cmd, NULL, 0, NULL);
}

static void MusicStop(void)
{
    mciSendStringW(L"stop cvbgm", NULL, 0, NULL);
    mciSendStringW(L"close cvbgm", NULL, 0, NULL);
}

static BOOL MusicStart(HWND hwnd)
{
    wchar_t path[MAX_PATH];
    if (!MusicExtractFile(path, MAX_PATH)) return FALSE;
    mciSendStringW(L"close cvbgm", NULL, 0, NULL);
    wchar_t cmd[MAX_PATH + 96];
    _snwprintf(cmd, MAX_PATH + 95, L"open \"%ls\" type mpegvideo alias cvbgm", path);
    cmd[MAX_PATH + 95] = 0;
    if (mciSendStringW(cmd, NULL, 0, NULL) != 0) return FALSE;
    {
        wchar_t vol[64];
        _snwprintf(vol, 63, L"setaudio cvbgm volume to %d", g_musicVolume * 10);
        vol[63] = 0;
        mciSendStringW(vol, NULL, 0, NULL);
    }
    /* "notify" posts MM_MCINOTIFY to hwnd when the track finishes; WndProc restarts it */
    if (mciSendStringW(L"play cvbgm notify", NULL, 0, hwnd) != 0) {
        mciSendStringW(L"close cvbgm", NULL, 0, NULL);
        return FALSE;
    }
    return TRUE;
}

static void MusicSetOn(HWND hwnd, BOOL on)
{
    if (on) {
        if (!MusicStart(hwnd)) {
            MessageBoxW(hwnd, L"The background music couldn't be started (is an audio device available?).",
                L"Music", MB_OK | MB_ICONWARNING);
            on = FALSE;
        }
    } else {
        MusicStop();
    }
    g_musicOn = on;
    SaveSettings();
    SettingsRefresh();
}

/* ---------------- button art ---------------- */

/* Round music button: a pair of beamed notes in the same ring style as the other round
   buttons; a diagonal slash when the music is off, an accent ring when it's on. Drawn at
   4x and halftone-shrunk so the edges are smooth. */
static void DrawMusicIcon(HDC hdc, RECT rc, COLORREF bgc, BOOL hovered, BOOL pressed, BOOL on)
{
    const int S = 4;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int W = w * S, H = h * S;
    BOOL d = g_darkMode;
    (void)d;
    BtnColors bc = ThemeBtnColors();
    COLORREF fill = bc.fill, border = bc.border, glyph = bc.glyph;
    if (on || pressed) border = bc.accent;
    else if (hovered) border = bc.hoverBorder;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    HBRUSH bgBrush = CreateSolidBrush(bgc);
    RECT all = { 0, 0, W, H };
    FillRect(mem, &all, bgBrush);
    DeleteObject(bgBrush);

    int bw = (int)(W * 0.075);
    HPEN borderPen = CreatePen(PS_SOLID, bw, border);
    HBRUSH fillBrush = CreateSolidBrush(fill);
    HGDIOBJ op = SelectObject(mem, borderPen);
    HGDIOBJ ob = SelectObject(mem, fillBrush);
    Ellipse(mem, bw / 2, bw / 2, W - bw / 2, H - bw / 2);

    HBRUSH glyphBrush = CreateSolidBrush(glyph);
    SelectObject(mem, GetStockObject(NULL_PEN));
    SelectObject(mem, glyphBrush);
    #define MU(v) ((int)((v) * W))
    Ellipse(mem, MU(0.27), MU(0.565), MU(0.47), MU(0.715));            /* left note head  */
    Ellipse(mem, MU(0.53), MU(0.505), MU(0.73), MU(0.655));            /* right note head */
    Rectangle(mem, MU(0.44), MU(0.30), MU(0.485), MU(0.64));           /* left stem       */
    Rectangle(mem, MU(0.70), MU(0.24), MU(0.745), MU(0.58));           /* right stem      */
    POINT beam[4] = { { MU(0.44), MU(0.30) }, { MU(0.745), MU(0.24) }, { MU(0.745), MU(0.33) }, { MU(0.44), MU(0.39) } };
    Polygon(mem, beam, 4);                                              /* beam            */
    if (!on) {
        /* slash: a fill-colored stroke first so it cuts cleanly through the notes */
        HPEN cut = CreatePen(PS_SOLID, MU(0.15), fill);
        SelectObject(mem, cut);
        MoveToEx(mem, MU(0.24), MU(0.78), NULL); LineTo(mem, MU(0.78), MU(0.22));
        HPEN line = CreatePen(PS_SOLID, MU(0.07), glyph);
        SelectObject(mem, line);
        MoveToEx(mem, MU(0.24), MU(0.78), NULL); LineTo(mem, MU(0.78), MU(0.22));
        SelectObject(mem, GetStockObject(NULL_PEN));
        DeleteObject(cut);
        DeleteObject(line);
    }
    #undef MU

    SelectObject(mem, ob);
    SelectObject(mem, op);
    DeleteObject(borderPen); DeleteObject(fillBrush); DeleteObject(glyphBrush);

    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);
    StretchBlt(hdc, rc.left, rc.top, w, h, mem, 0, 0, W, H, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

/* corner brackets: pointing outward ("go full screen") or inward ("leave full screen") */
static void DrawFullscreenGlyph(HDC hdc, RECT rc, COLORREF color, BOOL isFull)
{
    int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2;
    HPEN pen = CreatePen(PS_SOLID, 3, color);
    HGDIOBJ op = SelectObject(hdc, pen);
    const int half = 14, arm = 9;
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            if (!isFull) {
                int px = cx + sx * half, py = cy + sy * half;
                MoveToEx(hdc, px - sx * arm, py, NULL); LineTo(hdc, px, py); LineTo(hdc, px, py - sy * arm);
            } else {
                int px = cx + sx * 5, py = cy + sy * 5;
                MoveToEx(hdc, px + sx * arm, py, NULL); LineTo(hdc, px, py); LineTo(hdc, px, py + sy * arm);
            }
        }
    }
    SelectObject(hdc, op);
    DeleteObject(pen);
}

/* Round settings gear for the bottom of the sidebar, in the same ring style as the other
   round buttons; the ring turns blue while the Settings tab is open. */
static void DrawGearIcon(HDC hdc, RECT rc, BOOL hovered, BOOL pressed, BOOL active)
{
    const int S = 4;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int W = w * S, H = h * S;
    BOOL d = g_darkMode;
    COLORREF bgc    = d ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL;      /* sidebar color behind the circle */
    BtnColors bc = ThemeBtnColors();
    COLORREF fill = bc.fill, border = bc.border, glyph = bc.glyph;
    if (active || pressed) border = bc.accent;
    else if (hovered) border = bc.hoverBorder;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    HBRUSH bgBrush = CreateSolidBrush(bgc);
    RECT all = { 0, 0, W, H };
    FillRect(mem, &all, bgBrush);
    DeleteObject(bgBrush);

    int bw = (int)(W * 0.075);
    HPEN borderPen = CreatePen(PS_SOLID, bw, border);
    HBRUSH fillBrush = CreateSolidBrush(fill);
    HGDIOBJ op = SelectObject(mem, borderPen);
    HGDIOBJ ob = SelectObject(mem, fillBrush);
    Ellipse(mem, bw / 2, bw / 2, W - bw / 2, H - bw / 2);

    /* gear: 8 teeth around a body, with a round hole */
    HBRUSH glyphBrush = CreateSolidBrush(glyph);
    SelectObject(mem, GetStockObject(NULL_PEN));
    SelectObject(mem, glyphBrush);
    POINT pts[32];
    double cx = W / 2.0, cy = H / 2.0;
    double ri = W * 0.235, ro = W * 0.325;
    const double PI = 3.14159265358979;
    int n = 0;
    for (int i = 0; i < 8; i++) {
        double a = i * PI / 4.0;
        double ang[4] = { a - 0.27, a - 0.15, a + 0.15, a + 0.27 };
        double rad[4] = { ri, ro, ro, ri };
        for (int k = 0; k < 4; k++) {
            pts[n].x = (LONG)(cx + rad[k] * cos(ang[k]));
            pts[n].y = (LONG)(cy + rad[k] * sin(ang[k]));
            n++;
        }
    }
    Polygon(mem, pts, 32);
    HBRUSH holeBrush = CreateSolidBrush(fill);
    SelectObject(mem, holeBrush);
    int hr = (int)(W * 0.095);
    Ellipse(mem, (int)cx - hr, (int)cy - hr, (int)cx + hr, (int)cy + hr);

    SelectObject(mem, ob);
    SelectObject(mem, op);
    DeleteObject(borderPen); DeleteObject(fillBrush); DeleteObject(glyphBrush); DeleteObject(holeBrush);

    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);
    StretchBlt(hdc, rc.left, rc.top, w, h, mem, 0, 0, W, H, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

/* ---------------- sidebar tab icons (drawn in code) ----------------
   Home, Browse, Installed, Dub Renderer and Full screen are all drawn the same way as the gear and
   music buttons: a soft rounded button with a simple line glyph, rendered at 4x and halftone-shrunk
   so every edge is smooth. Active tab = blue ring + a faint blue tint. */

enum { TABGLYPH_HOME, TABGLYPH_GLOBE, TABGLYPH_LIST, TABGLYPH_MIC, TABGLYPH_FULLSCREEN };

static HPEN TabPen(int width, COLORREF c)
{
    LOGBRUSH lb; lb.lbStyle = BS_SOLID; lb.lbColor = c; lb.lbHatch = 0;
    return ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, width, &lb, 0, NULL);
}

static void DrawTabGlyph(HDC m, int W, int kind, COLORREF c, COLORREF fill, BOOL fullNow)
{
    #define U(v) ((int)((v) * W))
    HPEN pen = TabPen(U(0.052), c);
    HGDIOBJ op = SelectObject(m, pen);
    HGDIOBJ ob = SelectObject(m, GetStockObject(NULL_BRUSH));
    switch (kind) {
    case TABGLYPH_HOME: {
        POINT roof[3] = { { U(0.22), U(0.50) }, { U(0.50), U(0.24) }, { U(0.78), U(0.50) } };
        Polyline(m, roof, 3);
        POINT walls[4] = { { U(0.31), U(0.45) }, { U(0.31), U(0.76) }, { U(0.69), U(0.76) }, { U(0.69), U(0.45) } };
        Polyline(m, walls, 4);
        POINT door[4] = { { U(0.445), U(0.76) }, { U(0.445), U(0.58) }, { U(0.555), U(0.58) }, { U(0.555), U(0.76) } };
        Polyline(m, door, 4);
        break;
    }
    case TABGLYPH_GLOBE: {
        HPEN thin = TabPen(U(0.04), c);
        SelectObject(m, thin);
        Ellipse(m, U(0.23), U(0.23), U(0.77), U(0.77));                       /* outline */
        Ellipse(m, U(0.39), U(0.23), U(0.61), U(0.77));                       /* meridian */
        MoveToEx(m, U(0.50), U(0.23), NULL); LineTo(m, U(0.50), U(0.77));     /* center line */
        MoveToEx(m, U(0.23), U(0.50), NULL); LineTo(m, U(0.77), U(0.50));     /* equator */
        double half = sqrt(0.27 * 0.27 - 0.14 * 0.14);
        MoveToEx(m, U(0.5 - half), U(0.36), NULL); LineTo(m, U(0.5 + half), U(0.36));
        MoveToEx(m, U(0.5 - half), U(0.64), NULL); LineTo(m, U(0.5 + half), U(0.64));
        SelectObject(m, pen);
        DeleteObject(thin);
        break;
    }
    case TABGLYPH_LIST: {
        HBRUSH dot = CreateSolidBrush(c);
        SelectObject(m, dot);
        double ys[3] = { 0.32, 0.50, 0.68 };
        for (int i = 0; i < 3; i++) {
            Ellipse(m, U(0.255), U(ys[i] - 0.036), U(0.325), U(ys[i] + 0.036));      /* bullet */
            MoveToEx(m, U(0.40), U(ys[i]), NULL); LineTo(m, U(0.74), U(ys[i]));      /* row    */
        }
        SelectObject(m, GetStockObject(NULL_BRUSH));
        DeleteObject(dot);
        break;
    }
    case TABGLYPH_MIC: {
        HBRUSH body = CreateSolidBrush(c);
        HGDIOBJ pb = SelectObject(m, body);
        HGDIOBJ pp = SelectObject(m, GetStockObject(NULL_PEN));
        RoundRect(m, U(0.40), U(0.21), U(0.60), U(0.57), U(0.20), U(0.20));       /* capsule */
        SelectObject(m, pp);
        SelectObject(m, pb);
        DeleteObject(body);
        Arc(m, U(0.31), U(0.34), U(0.69), U(0.70), U(0.31), U(0.52), U(0.69), U(0.52));   /* cradle */
        MoveToEx(m, U(0.50), U(0.70), NULL); LineTo(m, U(0.50), U(0.79));                 /* stem   */
        MoveToEx(m, U(0.40), U(0.79), NULL); LineTo(m, U(0.60), U(0.79));                 /* base   */
        break;
    }
    case TABGLYPH_FULLSCREEN: {
        double a = 0.14;
        for (int sx = -1; sx <= 1; sx += 2) {
            for (int sy = -1; sy <= 1; sy += 2) {
                POINT p[3];
                if (!fullNow) {     /* corners pointing outward: "go full screen" */
                    double cx = 0.5 + sx * 0.23, cy = 0.5 + sy * 0.23;
                    p[0].x = U(cx - sx * a); p[0].y = U(cy);
                    p[1].x = U(cx);          p[1].y = U(cy);
                    p[2].x = U(cx);          p[2].y = U(cy - sy * a);
                } else {            /* corners pointing inward: "leave full screen" */
                    double cx = 0.5 + sx * 0.10, cy = 0.5 + sy * 0.10;
                    p[0].x = U(cx + sx * a); p[0].y = U(cy);
                    p[1].x = U(cx);          p[1].y = U(cy);
                    p[2].x = U(cx);          p[2].y = U(cy + sy * a);
                }
                Polyline(m, p, 3);
            }
        }
        break;
    }
    }
    SelectObject(m, ob);
    SelectObject(m, op);
    DeleteObject(pen);
    (void)fill;
    #undef U
}

static void DrawSidebarTabIcon(LPDRAWITEMSTRUCT dis, BOOL hovered, BOOL active)
{
    int kind = TABGLYPH_HOME;
    switch (dis->CtlID) {
        case ID_TAB_BROWSE:    kind = TABGLYPH_GLOBE; break;
        case ID_TAB_INSTALLED: kind = TABGLYPH_LIST; break;
        case ID_TAB_DUB:       kind = TABGLYPH_MIC; break;
        case ID_FULLSCREEN:    kind = TABGLYPH_FULLSCREEN; break;
    }
    const int S = 4;
    RECT rc = dis->rcItem;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int W = w * S, H = h * S;
    BOOL d = g_darkMode;

    COLORREF sidebar = d ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL;
    BtnColors bc = ThemeBtnColors();
    COLORREF fill    = d ? UkLerp(Pal()->panel, RGB(0, 0, 0), 0.28) : Pal()->bg;
    COLORREF border  = bc.border;
    COLORREF glyph   = bc.glyph;
    if (hovered) { fill = UkLerp(fill, glyph, 0.07); border = bc.hoverBorder; }
    if (active)  { fill = UkLerp(fill, bc.accent, d ? 0.22 : 0.14); border = bc.accent; }

    HDC mem = CreateCompatibleDC(dis->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(dis->hDC, W, H);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    HBRUSH sb = CreateSolidBrush(sidebar);
    RECT all = { 0, 0, W, H };
    FillRect(mem, &all, sb);
    DeleteObject(sb);

    int bw = active ? (int)(W * 0.045) : (int)(W * 0.03);
    if (bw < 2) bw = 2;
    HPEN bp = CreatePen(PS_SOLID, bw, border);
    HBRUSH fb = CreateSolidBrush(fill);
    HGDIOBJ op = SelectObject(mem, bp);
    HGDIOBJ ob = SelectObject(mem, fb);
    int rad = (int)(W * 0.30);
    RoundRect(mem, bw / 2, bw / 2, W - bw / 2, H - bw / 2, rad, rad);
    SelectObject(mem, ob);
    SelectObject(mem, op);
    DeleteObject(bp);
    DeleteObject(fb);

    DrawTabGlyph(mem, W, kind, glyph, fill, g_fullscreen);

    SetStretchBltMode(dis->hDC, HALFTONE);
    SetBrushOrgEx(dis->hDC, 0, 0, NULL);
    StretchBlt(dis->hDC, rc.left, rc.top, w, h, mem, 0, 0, W, H, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

/* the star slider in Settings: 0 stars = no animation at all, so the timer is stopped */
static void ApplyStarCount(void)
{
    if (!g_hMainWnd) return;
    if (g_starCount > 0) SetTimer(g_hMainWnd, ANIM_TIMER_ID, ANIM_INTERVAL_MS, NULL);
    else KillTimer(g_hMainWnd, ANIM_TIMER_ID);
    RedrawWindow(g_hMainWnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE);
}
