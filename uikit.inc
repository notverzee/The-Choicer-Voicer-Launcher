/* ===================================================================
   uikit.inc - shared "soft" look: smooth rounded shapes and the custom
   title bar. Included from main.c just before PaintAnimatedBackdrop().

   GDI's RoundRect() has jagged, un-smoothed corners, so shapes here are
   rendered pixel-by-pixel from a signed-distance formula (distance from
   each pixel to the rounded rectangle's edge -> partial coverage ->
   anti-aliased blend). No extra library needed.
   =================================================================== */

#define TITLE_H_DEFAULT 36

static int  g_titleH = TITLE_H_DEFAULT;     /* 0 while in full screen (no title bar) */
static HWND g_hMainWnd = NULL;
static RECT g_contentRect;                  /* the tab-page area (outlined while a tab page is showing) */
static RECT g_cardRect[3];                  /* Normal, Compatibility, Get Mods card rects (main-window client coords) */
static HICON g_hTitleIcon = NULL;
static BOOL g_sizing = FALSE;               /* TRUE while the user is dragging the window edge */
/* the scaled, tinted backdrop is cached here (rebuilt only when the size or theme changes) */
static HDC g_hBgCacheDC = NULL;
static HBITMAP g_hBgCacheBmp = NULL;
static int g_bgCacheW = 0, g_bgCacheH = 0;
static BOOL g_bgCacheDark = FALSE;
static HFONT g_fontCaption = NULL;          /* Segoe MDL2 Assets: the minimize / maximize / close glyphs */

#define ID_CAP_MIN    5101
#define ID_CAP_MAX    5102
#define ID_CAP_CLOSE  5103
static HWND g_hCapMin = NULL, g_hCapMax = NULL, g_hCapClose = NULL;

static COLORREF UkLerp(COLORREF a, COLORREF b, double t)
{
    if (t <= 0) return a;
    if (t >= 1) return b;
    int r = (int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t + 0.5);
    int g = (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t + 0.5);
    int bl = (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t + 0.5);
    return RGB(r, g, bl);
}

/* colours shared by the round / square sidebar buttons, derived from the active theme */
typedef struct { COLORREF fill, border, glyph, hoverBorder, accent; } BtnColors;
static BtnColors ThemeBtnColors(void)
{
    const ThemePal* p = Pal();
    BtnColors c;
    c.fill = p->panel;
    c.border = p->border;
    c.glyph = g_darkMode ? p->text : UkLerp(p->text, RGB(0, 0, 0), 0.55);
    c.hoverBorder = UkLerp(c.border, c.glyph, 0.45);
    c.accent = p->accent;
    return c;
}

/* signed distance from point (px,py) to a rounded rect centred at (cx,cy) with half-sizes
   (hw,hh) and corner radius r; negative inside */
static double UkRoundRectDist(double px, double py, double cx, double cy, double hw, double hh, double r)
{
    double qx = fabs(px - cx) - (hw - r);
    double qy = fabs(py - cy) - (hh - r);
    double ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    double inside = (qx > qy ? qx : qy);
    if (inside > 0) inside = 0;
    return sqrt(ox * ox + oy * oy) + inside - r;
}

static double UkCoverage(double d)
{
    double c = 0.5 - d;
    return c < 0 ? 0 : (c > 1 ? 1 : c);
}

/* Renders the pixels of `region` (a sub-rectangle of the shape `shape`, both in the same
   coordinate space as hdc) with anti-aliasing. fillAlpha 255 = opaque fill with a border
   ring; fillAlpha 0 = outline only (nothing inside). Pixels are blended over whatever is
   already in the DC. */
static void UkBlendShape(HDC hdc, RECT shape, RECT region, double radius, COLORREF fill, int fillAlpha,
                         COLORREF border, int borderW)
{
    int rw = region.right - region.left, rh = region.bottom - region.top;
    if (rw <= 0 || rh <= 0) return;
    int sw = shape.right - shape.left, sh = shape.bottom - shape.top;
    double r = radius;
    if (r > sw / 2.0) r = sw / 2.0;
    if (r > sh / 2.0) r = sh / 2.0;

    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = rw;
    bi.bmiHeader.biHeight = -rh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    unsigned char* bits = NULL;
    HBITMAP dib = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    if (!dib || !bits) { if (dib) DeleteObject(dib); return; }
    HDC mem = CreateCompatibleDC(hdc);
    HGDIOBJ old = SelectObject(mem, dib);
    BitBlt(mem, 0, 0, rw, rh, hdc, region.left, region.top, SRCCOPY);

    double cx = sw / 2.0, cy = sh / 2.0;
    double hw = sw / 2.0, hh = sh / 2.0;
    double ir = r - borderW; if (ir < 0) ir = 0;
    double ihw = hw - borderW, ihh = hh - borderW;

    for (int y = 0; y < rh; y++) {
        for (int x = 0; x < rw; x++) {
            double px = (region.left - shape.left) + x + 0.5;
            double py = (region.top - shape.top) + y + 0.5;
            double co = UkCoverage(UkRoundRectDist(px, py, cx, cy, hw, hh, r));
            if (co <= 0) continue;
            double ci = borderW > 0 ? UkCoverage(UkRoundRectDist(px, py, cx, cy, ihw, ihh, ir)) : co;
            unsigned char* p = bits + ((size_t)y * rw + x) * 4;
            COLORREF bg = RGB(p[2], p[1], p[0]);
            COLORREF out = bg;
            if (fillAlpha > 0) {
                if (borderW > 0) out = UkLerp(out, border, co);
                out = UkLerp(out, fill, ci * fillAlpha / 255.0);
            } else if (borderW > 0) {
                double ring = co - ci;
                out = UkLerp(out, border, ring < 0 ? 0 : ring);
            }
            p[0] = GetBValue(out); p[1] = GetGValue(out); p[2] = GetRValue(out);
        }
    }
    BitBlt(hdc, region.left, region.top, rw, rh, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(dib);
}

/* a whole small shape (buttons, hover pills): every pixel is evaluated */
static void DrawRoundedAA(HDC hdc, RECT rc, int radius, COLORREF fill, int fillAlpha, COLORREF border, int borderW)
{
    UkBlendShape(hdc, rc, rc, radius, fill, fillAlpha, border, borderW);
}

/* a big opaque card: the straight edges and the interior are plain GDI fills (cheap), and
   only the four corner squares are evaluated per pixel - this runs every animation frame */
static void DrawCardAA(HDC hdc, RECT rc, int radius, COLORREF fill, COLORREF border)
{
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w < 4 || h < 4) return;
    int R = radius + 2;
    if (R * 2 > w) R = w / 2;
    if (R * 2 > h) R = h / 2;
    const int bw = 1;

    HBRUSH bb = CreateSolidBrush(border), fb = CreateSolidBrush(fill);
    RECT t;
    /* middle band: left border | fill | right border */
    SetRect(&t, rc.left, rc.top + R, rc.left + bw, rc.bottom - R);              FillRect(hdc, &t, bb);
    SetRect(&t, rc.left + bw, rc.top + R, rc.right - bw, rc.bottom - R);        FillRect(hdc, &t, fb);
    SetRect(&t, rc.right - bw, rc.top + R, rc.right, rc.bottom - R);            FillRect(hdc, &t, bb);
    /* top band */
    SetRect(&t, rc.left + R, rc.top, rc.right - R, rc.top + bw);                FillRect(hdc, &t, bb);
    SetRect(&t, rc.left + R, rc.top + bw, rc.right - R, rc.top + R);            FillRect(hdc, &t, fb);
    /* bottom band */
    SetRect(&t, rc.left + R, rc.bottom - R, rc.right - R, rc.bottom - bw);      FillRect(hdc, &t, fb);
    SetRect(&t, rc.left + R, rc.bottom - bw, rc.right - R, rc.bottom);          FillRect(hdc, &t, bb);
    DeleteObject(bb);
    DeleteObject(fb);

    RECT corner;
    SetRect(&corner, rc.left, rc.top, rc.left + R, rc.top + R);
    UkBlendShape(hdc, rc, corner, radius, fill, 255, border, bw);
    SetRect(&corner, rc.right - R, rc.top, rc.right, rc.top + R);
    UkBlendShape(hdc, rc, corner, radius, fill, 255, border, bw);
    SetRect(&corner, rc.left, rc.bottom - R, rc.left + R, rc.bottom);
    UkBlendShape(hdc, rc, corner, radius, fill, 255, border, bw);
    SetRect(&corner, rc.right - R, rc.bottom - R, rc.right, rc.bottom);
    UkBlendShape(hdc, rc, corner, radius, fill, 255, border, bw);
}

/* Owner-drawn buttons have rounded corners now, so the pixels *behind* the corners have to
   be put back first. Buttons directly on the main window (the Launcher page, the title bar
   buttons) copy that spot out of the backdrop buffer; buttons inside the tab pages and
   popups sit on the plain theme background. */
static void PaintButtonSurround(HWND btn, HDC hdc, RECT rc)
{
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HWND parent = GetParent(btn);
    if (parent && parent == g_hMainWnd && g_hMemDC) {
        POINT p = { rc.left, rc.top };
        MapWindowPoints(btn, parent, &p, 1);
        BitBlt(hdc, rc.left, rc.top, w, h, g_hMemDC, p.x, p.y, SRCCOPY);
    } else {
        HBRUSH b = CreateSolidBrush(g_darkMode ? COLOR_DARK_BG : COLOR_LIGHT_BG);
        FillRect(hdc, &rc, b);
        DeleteObject(b);
    }
}

/* the strip across the top of the window: app icon, title, and (as real buttons placed by
   LayoutMainWindow) minimize / maximize / close */
static void PaintTitleBar(HDC hdc, int w)
{
    if (g_titleH <= 0) return;
    BOOL d = g_darkMode;
    RECT bar = { 0, 0, w, g_titleH };
    HBRUSH b = CreateSolidBrush(d ? UkLerp(Pal()->bg, RGB(0, 0, 0), 0.30) : UkLerp(Pal()->bg, Pal()->border, 0.25));
    FillRect(hdc, &bar, b);
    DeleteObject(b);
    RECT line = { 0, g_titleH - 1, w, g_titleH };
    HBRUSH lb = CreateSolidBrush(d ? COLOR_DARK_BORDER : COLOR_LIGHT_BORDER);
    FillRect(hdc, &line, lb);
    DeleteObject(lb);

    if (!g_hTitleIcon)
        g_hTitleIcon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    if (g_hTitleIcon) DrawIconEx(hdc, 14, (g_titleH - 16) / 2, g_hTitleIcon, 16, 16, 0, NULL, DI_NORMAL);

    HGDIOBJ of = SelectObject(hdc, g_fontRegular);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, d ? Pal()->text : UkLerp(Pal()->text, RGB(0, 0, 0), 0.35));
    RECT tr = { 38, 0, w - 160, g_titleH };
    DrawTextW(hdc, L"The Choicer Voicer Launcher", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(hdc, of);
}

/* minimize / maximize(restore) / close, drawn as a soft rounded pill on hover */
static void DrawCaptionButton(LPDRAWITEMSTRUCT dis, BOOL maximized)
{
    BOOL hovered = GetPropW(dis->hwndItem, L"CVHover") != NULL;
    BOOL pressed = (dis->itemState & ODS_SELECTED) != 0;
    BOOL d = g_darkMode;
    PaintButtonSurround(dis->hwndItem, dis->hDC, dis->rcItem);

    COLORREF glyph = d ? Pal()->text : UkLerp(Pal()->text, RGB(0, 0, 0), 0.45);
    if (hovered || pressed) {
        COLORREF fill;
        if (dis->CtlID == ID_CAP_CLOSE) { fill = pressed ? RGB(160, 36, 24) : RGB(205, 50, 36); glyph = RGB(255, 255, 255); }
        else fill = UkLerp(Pal()->bg, Pal()->text, pressed ? 0.26 : 0.14);
        DrawRoundedAA(dis->hDC, dis->rcItem, 8, fill, 255, fill, 0);
    }

    const wchar_t* ch = L"\xE921";                                       /* minimize */
    if (dis->CtlID == ID_CAP_MAX)   ch = maximized ? L"\xE923" : L"\xE922";
    if (dis->CtlID == ID_CAP_CLOSE) ch = L"\xE8BB";
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, glyph);
    HGDIOBJ of = SelectObject(dis->hDC, g_fontCaption ? g_fontCaption : g_fontSymbol);
    RECT r = dis->rcItem;
    DrawTextW(dis->hDC, ch, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dis->hDC, of);
}

/* a thin smooth rounded outline (no fill): straight edges are 1px GDI lines, corners are per-pixel */
static void DrawOutlineAA(HDC hdc, RECT rc, int radius, COLORREF border)
{
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w < 4 || h < 4) return;
    int R = radius + 2;
    if (R * 2 > w) R = w / 2;
    if (R * 2 > h) R = h / 2;
    HBRUSH bb = CreateSolidBrush(border);
    RECT t;
    SetRect(&t, rc.left + R, rc.top, rc.right - R, rc.top + 1);          FillRect(hdc, &t, bb);
    SetRect(&t, rc.left + R, rc.bottom - 1, rc.right - R, rc.bottom);    FillRect(hdc, &t, bb);
    SetRect(&t, rc.left, rc.top + R, rc.left + 1, rc.bottom - R);        FillRect(hdc, &t, bb);
    SetRect(&t, rc.right - 1, rc.top + R, rc.right, rc.bottom - R);      FillRect(hdc, &t, bb);
    DeleteObject(bb);
    RECT c;
    SetRect(&c, rc.left, rc.top, rc.left + R, rc.top + R);               UkBlendShape(hdc, rc, c, radius, 0, 0, border, 1);
    SetRect(&c, rc.right - R, rc.top, rc.right, rc.top + R);             UkBlendShape(hdc, rc, c, radius, 0, 0, border, 1);
    SetRect(&c, rc.left, rc.bottom - R, rc.left + R, rc.bottom);         UkBlendShape(hdc, rc, c, radius, 0, 0, border, 1);
    SetRect(&c, rc.right - R, rc.bottom - R, rc.right, rc.bottom);       UkBlendShape(hdc, rc, c, radius, 0, 0, border, 1);
}
