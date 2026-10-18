/* ===================================================================
   settings.inc - the Settings tab.

   One round gear button at the bottom of the sidebar opens this page,
   which now holds everything the four old round buttons did:
     * Dark mode toggle
     * Music on/off + a volume slider
     * Open the game's modding folder
     * The info text (what the old (i) button's popup said)
   The first-run welcome popup is unchanged.
   Included from main.c after extras.inc, before SwitchTab().
   =================================================================== */

#define ID_TAB_SETTINGS 5007
#define ID_SET_DARK     8201
#define ID_SET_FOLDER   8202
#define ID_SET_README   8203
#define ID_SET_CREDITS  8204
#define ID_SET_BACK     8205
#define ID_SET_ABOUT    8206
#define ID_AB_GAME      8207
#define ID_AB_GB        8208
#define ID_SET_THEME0   8210      /* 8210..8214: the five colour swatches */
/* the round music button on this page reuses ID_MUSIC (extras.inc) */

/* g_hSettingsWnd, g_hSetInfoEdit, g_hTabSettingsBtn and g_musicVolume are declared near the top of main.c */
static HWND g_hSetTitle, g_hSetMusicLbl, g_hSetDarkBtn, g_hSetFolderBtn, g_hSetReadmeBtn;
static HWND g_hSetThemeLbl, g_hSetStarsLbl;
static HWND g_hSetThemeBtn[NUM_THEMES];
static RECT g_setHdr[4];
static const wchar_t* g_setHdrText[4] = { L"Appearance", L"Music", L"Folders", L"More" };
/* two custom-drawn sliders share the same code: 0 = music volume (0-100 %), 1 = number of background stars */
typedef struct { RECT area, pct; int* value; void (*apply)(void); const wchar_t* fmt; } SetSlider;
static SetSlider g_sliders[2];
static int g_slDrag = -1;         /* which slider is being dragged, or -1 */

/* ---- credits view (a second view inside the Settings page, opened by the Credits button) ---- */
/* Settings has three views: 0 = the normal settings, 1 = credits, 2 = About. Credits and About are
   painted pages (no edit controls, so nothing in them can be edited), each with a Back button. */
static int g_setView = 0;
static int g_setPrevView = 0;                 /* where Back goes after Credits (it can be opened from About) */
#define g_setCredits (g_setView == 1)
static HWND g_hSetCreditsBtn = NULL, g_hSetBackBtn = NULL, g_hSetAboutBtn = NULL, g_hAbGameBtn = NULL, g_hAbGbBtn = NULL;
static HFONT g_setLinkFont = NULL;          /* g_fontSmall, underlined */

typedef struct {
    const wchar_t* role;       /* small label on the right: what they did */
    const wchar_t* name;       /* who / what */
    const wchar_t* detail;     /* one or two sentences */
    const wchar_t* url;        /* clickable link, or NULL */
    RECT card, link;           /* filled in by SettingsLayoutCredits */
} CreditEntry;

#define NUM_CREDITS 5
static CreditEntry g_credits[NUM_CREDITS] = {
    { L"THE GAME", L"The Choicer Voicer \u2014 YeahMaybe",
      L"The Choicer Voicer is created by YeahMaybe. This launcher is a companion tool for the game.",
      L"https://yeahmaybe.itch.io/the-choicer-voicer" },
    { L"DUB RENDERER", L"Choicer Voicer Dub Renderer \u2014 TDTLuv",
      L"The Dub Renderer tab is a port of TDTLuv's Choicer Voicer Dub Renderer source code (the standard renderer; the co-op part isn't included).",
      L"https://gamebanana.com/tools/23587" },
    { L"BROWSE FILTERS", L"TCV Mod Manager \u2014 KevTheDev01",
      L"The Browse Mods type filters (Voice & Dub, Players, Studios, Judges, Menus, Hosts, Chatter) and the way a game's mods are listed follow how KevTheDev01's TCV Mod Manager does it.",
      L"https://github.com/KevTheDev01/tcv-mod-manager" },
    { L"MUSIC", L"Water Gloss (Frutiger Aero) \u2014 9jackjack8",
      L"The background music you can switch on in Settings.",
      NULL },
    { L"LAUNCHER", L"Claude Sonnet 5.5 \u2014 Anthropic",
      L"This launcher's code, design and the Dub Renderer port were written with Claude Sonnet 5.5, an AI model made by Anthropic.",
      L"https://www.anthropic.com/claude" },
};

/* ---- About page content ---- */
#define SET_WIDE2(x) L##x
#define SET_WIDE(x) SET_WIDE2(x)
static const wchar_t* ABOUT_BUILD = L"Built " SET_WIDE(__DATE__);
static const wchar_t* ABOUT_TAGLINE =
    L"One place for The Choicer Voicer: launch your game versions, browse and install mods, and render finished dubs.";
static const wchar_t* ABOUT_STEPS[3] = {
    L"Read the README first. It explains exactly where your game / version folders need to go so the launcher can find them.",
    L"On the Launcher tab, pick a version in the Normal or Compatibility dropdown and press Launch. Use + Add Version to add another one.",
    L"Browse Mods finds and installs mods, Installed Mods manages them, and Dub Renderer turns an exported dub into a finished MP4.",
};
static RECT g_abHero, g_abSteps, g_abIcon, g_abTitleR, g_abBuildR, g_abTagR, g_abStepsHdr;
static RECT g_abBadge[3], g_abStepText[3];

/* keeps the toggle captions in step with the real state */
static void SettingsRefresh(void)
{
    if (!g_hSettingsWnd) return;
    SetWindowTextW(g_hSetDarkBtn, g_darkMode ? L"Dark mode: On" : L"Dark mode: Off");
    SetWindowTextW(g_hSetMusicLbl, g_musicOn ? L"Music is on" : L"Music is off");
    InvalidateRect(g_hSetDarkBtn, NULL, TRUE);
    if (g_hSetMusicBtn) InvalidateRect(g_hSetMusicBtn, NULL, TRUE);
    for (int i = 0; i < 2; i++) {
        InvalidateRect(g_hSettingsWnd, &g_sliders[i].area, FALSE);
        InvalidateRect(g_hSettingsWnd, &g_sliders[i].pct, FALSE);
    }
    for (int i = 0; i < NUM_THEMES; i++) if (g_hSetThemeBtn[i]) InvalidateRect(g_hSetThemeBtn[i], NULL, TRUE);
}

static void SettingsSetTheme(int idx)
{
    if (idx < 0 || idx >= NUM_THEMES) return;
    g_themeIdx = idx;
    SaveSettings();
    ApplyThemeColors(g_hMainWnd);       /* new brushes + a full repaint (backdrop is rebuilt too) */
    SettingsRefresh();
}

static void SettingsToggleDark(void)
{
    g_darkMode = !g_darkMode;
    SaveSettings();
    ApplyThemeColors(g_hMainWnd);
    SettingsRefresh();
}

/* stacks the four credit cards; heights come from the real (wrapped) text so nothing clips */
/* the credits list is taller than the page at the minimum window size, so it scrolls (mouse wheel) */
static int g_credScroll = 0, g_credTop = 0, g_credBottom = 0, g_credTotal = 0, g_credW = 0, g_credH = 0;

/* stacks the credit cards under `top`, scrolled by g_credScroll, and records the visible band
   [top, bottom); heights come from the real (wrapped) text so nothing clips */
static void SettingsLayoutCredits(HWND hwnd, int W, int H, int top, int bottom)
{
    const int pad = 14, cardPad = 14, gap = 10;
    int cardW = W - 2 * pad - 10;                 /* a little room on the right for the scroll bar */
    int hHdr = DubFontH(g_fontGroupHeader) + 2;
    int hSmall = DubFontH(g_fontSmall) + 2;
    HDC dc = GetDC(hwnd);
    HGDIOBJ of = SelectObject(dc, g_fontSmall);

    int heights[NUM_CREDITS], detailHs[NUM_CREDITS], total = 0;
    for (int i = 0; i < NUM_CREDITS; i++) {
        RECT m = { 0, 0, cardW - 2 * cardPad, 0 };
        DrawTextW(dc, g_credits[i].detail, -1, &m, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        detailHs[i] = m.bottom - m.top;
        heights[i] = 10 + hHdr + 2 + detailHs[i] + (g_credits[i].url ? 4 + hSmall : 0) + 12;
        total += heights[i] + (i ? gap : 0);
    }
    int maxScroll = total - (bottom - top);
    if (maxScroll < 0) maxScroll = 0;
    if (g_credScroll > maxScroll) g_credScroll = maxScroll;
    if (g_credScroll < 0) g_credScroll = 0;

    int y = top - g_credScroll;
    for (int i = 0; i < NUM_CREDITS; i++) {
        SetRect(&g_credits[i].card, pad, y, pad + cardW, y + heights[i]);
        if (g_credits[i].url) {
            int ly = y + 10 + hHdr + 2 + detailHs[i] + 4;
            SIZE sz; GetTextExtentPoint32W(dc, g_credits[i].url, (int)wcslen(g_credits[i].url), &sz);
            SetRect(&g_credits[i].link, pad + cardPad, ly, pad + cardPad + sz.cx, ly + hSmall);
        } else {
            SetRectEmpty(&g_credits[i].link);
        }
        y += heights[i] + gap;
    }
    g_credTop = top; g_credBottom = bottom; g_credTotal = total; g_credW = W; g_credH = H;
    SelectObject(dc, of);
    ReleaseDC(hwnd, dc);
}

/* the About page: a hero card (icon, name, build date, one-line description), a "Getting started"
   card with three numbered steps, and a row of link buttons. Heights come from the wrapped text. */
static void SettingsLayoutAbout(HWND hwnd, int W, int top, int bh)
{
    const int pad = 14, cp = 16, gap = 10;
    int cardW = W - 2 * pad;
    int hTitle = DubFontH(g_fontTitle) + 2;
    int hSmall = DubFontH(g_fontSmall) + 2;
    int hHdr = DubFontH(g_fontGroupHeader) + 2;
    HDC dc = GetDC(hwnd);
    HGDIOBJ of = SelectObject(dc, g_fontSmall);

    int textX = pad + cp + 48 + 16;
    int textW = pad + cardW - cp - textX;
    RECT m = { 0, 0, textW, 0 };
    DrawTextW(dc, ABOUT_TAGLINE, -1, &m, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    int tagH = m.bottom - m.top;
    int heroH = cp + hTitle + 2 + hSmall + 6 + tagH + cp;
    if (heroH < 2 * cp + 48) heroH = 2 * cp + 48;
    SetRect(&g_abHero, pad, top, pad + cardW, top + heroH);
    SetRect(&g_abIcon, pad + cp, top + cp, pad + cp + 48, top + cp + 48);
    int ty = top + cp;
    SetRect(&g_abTitleR, textX, ty, textX + textW, ty + hTitle); ty += hTitle + 2;
    SetRect(&g_abBuildR, textX, ty, textX + textW, ty + hSmall); ty += hSmall + 6;
    SetRect(&g_abTagR, textX, ty, textX + textW, ty + tagH);

    int y = top + heroH + gap;
    int stepsW = cardW - 2 * cp - 26 - 12;
    int sy = y + cp;
    SetRect(&g_abStepsHdr, pad + cp, sy, pad + cardW - cp, sy + hHdr);
    sy += hHdr + 8;
    for (int i = 0; i < 3; i++) {
        RECT mm = { 0, 0, stepsW, 0 };
        DrawTextW(dc, ABOUT_STEPS[i], -1, &mm, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        int th = mm.bottom - mm.top;
        int rowH = th < 26 ? 26 : th;
        SetRect(&g_abBadge[i], pad + cp, sy, pad + cp + 26, sy + 26);
        SetRect(&g_abStepText[i], pad + cp + 26 + 12, sy, pad + cardW - cp, sy + th);
        sy += rowH + 10;
    }
    sy += cp - 10;
    SetRect(&g_abSteps, pad, y, pad + cardW, sy);
    SelectObject(dc, of);
    ReleaseDC(hwnd, dc);

    if (g_setView == 2) {       /* the link row (README and Credits are shared with the main view) */
        int ry = sy + 14, x = pad;
        MoveWindow(g_hSetReadmeBtn, x, ry, 150, bh, TRUE);  x += 160;
        MoveWindow(g_hAbGameBtn,    x, ry, 150, bh, TRUE);  x += 160;
        MoveWindow(g_hAbGbBtn,      x, ry, 170, bh, TRUE);  x += 180;
        MoveWindow(g_hSetCreditsBtn, x, ry, 120, bh, TRUE);
    }
}

static void SettingsLayout(HWND hwnd)
{
    if (!g_hSetDarkBtn) return;
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    const int pad = 14, gap = 8;
    int iw = W - 2 * pad;

    int hTitle = DubFontH(g_fontTitle) + 2;
    int hSmall = DubFontH(g_fontSmall) + 2;
    int hHdr   = DubFontH(g_fontGroupHeader) + 2;
    int hEdit  = DubFontH(g_fontRegular) + 8;
    if (hEdit < 30) hEdit = 30;
    int bh = hEdit + 4;
    (void)hSmall; (void)gap;

    int y = 4;
    MoveWindow(g_hSetTitle, pad, y, iw, hTitle, TRUE);
    y += hTitle + 8;

    /* shared geometry: the two sliders line up under each other */
    const int mb = 44;
    int lblW = 130;
    int sx = pad + mb + 12 + lblW + 10;
    int pctW = 56;
    int sw = W - pad - pctW - sx;
    if (sw > 420) sw = 420;
    if (sw < 80) sw = 80;

    SetRect(&g_setHdr[0], pad, y, W - pad, y + hHdr); y += hHdr + 8;
    MoveWindow(g_hSetDarkBtn, pad, y, 190, bh, TRUE);
    {   /* colour swatches */
        int lx = pad + 190 + 28;
        MoveWindow(g_hSetThemeLbl, lx, y + (bh - hEdit) / 2, 70, hEdit, TRUE);
        const int sd = 30, sg = 10;
        int x0 = lx + 78;
        for (int i = 0; i < NUM_THEMES; i++)
            MoveWindow(g_hSetThemeBtn[i], x0 + i * (sd + sg), y + (bh - sd) / 2, sd, sd, TRUE);
    }
    y += bh + 12;

    /* background stars: label + slider + count */
    MoveWindow(g_hSetStarsLbl, pad, y + (36 - hEdit) / 2, sx - pad - 10, hEdit, TRUE);
    SetRect(&g_sliders[1].area, sx, y + 4, sx + sw, y + 32);
    SetRect(&g_sliders[1].pct, sx + sw + 10, y + 18 - hEdit / 2, sx + sw + 10 + pctW, y + 18 + hEdit / 2);
    y += 36 + 16;

    SetRect(&g_setHdr[1], pad, y, W - pad, y + hHdr); y += hHdr + 8;
    MoveWindow(g_hSetMusicBtn, pad, y, mb, mb, TRUE);
    MoveWindow(g_hSetMusicLbl, pad + mb + 12, y + (mb - hEdit) / 2 + 2, lblW, hEdit, TRUE);
    SetRect(&g_sliders[0].area, sx, y + mb / 2 - 14, sx + sw, y + mb / 2 + 14);
    SetRect(&g_sliders[0].pct, sx + sw + 10, y + mb / 2 - hEdit / 2, sx + sw + 10 + pctW, y + mb / 2 + hEdit / 2);
    y += mb + 16;

    SetRect(&g_setHdr[2], pad, y, W - pad, y + hHdr); y += hHdr + 8;
    MoveWindow(g_hSetFolderBtn, pad, y, 260, bh, TRUE);
    y += bh + 16;

    SetRect(&g_setHdr[3], pad, y, W - pad, y + hHdr); y += hHdr + 8;
    if (g_setView != 2) {       /* README and Credits move into the link row while About is showing */
        MoveWindow(g_hSetAboutBtn, pad, y, 190, bh, TRUE);
        MoveWindow(g_hSetCreditsBtn, pad + 200, y, 130, bh, TRUE);
        MoveWindow(g_hSetReadmeBtn, pad + 340, y, 150, bh, TRUE);
    }

    /* the Credits and About pages sit under the title; Back goes along the bottom */
    SettingsLayoutCredits(hwnd, W, H, 4 + hTitle + 8, H - bh - 12 - 10);
    SettingsLayoutAbout(hwnd, W, 4 + hTitle + 8, bh);
    MoveWindow(g_hSetBackBtn, pad, H - bh - 12, 120, bh, TRUE);
}

/* switches between the normal settings (0), the credits page (1) and the About page (2) */
static void SettingsShowView(int view)
{
    if (view == 1) { g_setPrevView = g_setView; g_credScroll = 0; }   /* Back returns to wherever Credits was opened from */
    g_setView = view;
    BOOL v0 = (view == 0), v1 = (view == 1), v2 = (view == 2);
    int sMain = v0 ? SW_SHOW : SW_HIDE;
    ShowWindow(g_hSetDarkBtn, sMain);
    ShowWindow(g_hSetThemeLbl, sMain);
    ShowWindow(g_hSetStarsLbl, sMain);
    for (int i = 0; i < NUM_THEMES; i++) ShowWindow(g_hSetThemeBtn[i], sMain);
    ShowWindow(g_hSetMusicBtn, sMain);
    ShowWindow(g_hSetMusicLbl, sMain);
    ShowWindow(g_hSetFolderBtn, sMain);
    ShowWindow(g_hSetAboutBtn, sMain);
    ShowWindow(g_hSetReadmeBtn, (v0 || v2) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hSetCreditsBtn, (v0 || v2) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hAbGameBtn, v2 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hAbGbBtn, v2 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hSetBackBtn, (v1 || v2) ? SW_SHOW : SW_HIDE);
    SetWindowTextW(g_hSetTitle, v1 ? L"Credits" : (v2 ? L"About" : L"Settings"));
    SettingsLayout(g_hSettingsWnd);
    InvalidateRect(g_hSettingsWnd, NULL, FALSE);
    UpdateWindow(g_hSettingsWnd);
}

/* the About page */
static void SettingsPaintAbout(HDC hdc)
{
    COLORREF panel = g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL;
    COLORREF border = g_darkMode ? COLOR_DARK_BORDER : COLOR_LIGHT_BORDER;
    COLORREF text = g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT;
    COLORREF accent = DubKindColor(DUBC_ACCENT);
    SetBkMode(hdc, TRANSPARENT);

    /* hero card */
    DrawCardAA(hdc, g_abHero, 14, panel, border);
    static HICON appIcon = NULL;
    if (!appIcon)
        appIcon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 48, 48, LR_DEFAULTCOLOR);
    if (appIcon) DrawIconEx(hdc, g_abIcon.left, g_abIcon.top, appIcon, 48, 48, 0, NULL, DI_NORMAL);
    HGDIOBJ of = SelectObject(hdc, g_fontTitle);
    SetTextColor(hdc, text);
    DrawTextW(hdc, L"The Choicer Voicer Launcher", -1, &g_abTitleR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(hdc, g_fontSmall);
    SetTextColor(hdc, DubKindColor(DUBC_MUTED));
    DrawTextW(hdc, ABOUT_BUILD, -1, &g_abBuildR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(hdc, text);
    DrawTextW(hdc, ABOUT_TAGLINE, -1, &g_abTagR, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

    /* getting started */
    DrawCardAA(hdc, g_abSteps, 14, panel, border);
    SelectObject(hdc, g_fontGroupHeader);
    SetTextColor(hdc, accent);
    DrawTextW(hdc, L"Getting started", -1, &g_abStepsHdr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    for (int i = 0; i < 3; i++) {
        DrawRoundedAA(hdc, g_abBadge[i], 13, accent, 255, accent, 0);
        wchar_t num[2] = { (wchar_t)(L'1' + i), 0 };
        SelectObject(hdc, g_fontSmall);
        SetTextColor(hdc, g_darkMode ? RGB(18, 18, 22) : RGB(255, 255, 255));
        RECT br = g_abBadge[i];
        DrawTextW(hdc, num, -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SetTextColor(hdc, text);
        DrawTextW(hdc, ABOUT_STEPS[i], -1, &g_abStepText[i], DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    }
    SelectObject(hdc, of);
}

static void SettingsPaintCredits(HDC hdc)
{
    COLORREF panel = g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL;
    COLORREF border = g_darkMode ? COLOR_DARK_BORDER : COLOR_LIGHT_BORDER;
    COLORREF text = g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT;
    if (!g_setLinkFont) {
        LOGFONTW lf;
        if (GetObjectW(g_fontSmall, sizeof(lf), &lf)) { lf.lfUnderline = TRUE; g_setLinkFont = CreateFontIndirectW(&lf); }
    }
    const int cardPad = 14;
    int hHdr = DubFontH(g_fontGroupHeader) + 2;
    SetBkMode(hdc, TRANSPARENT);
    int savedDc = SaveDC(hdc);
    IntersectClipRect(hdc, 0, g_credTop, g_credW, g_credBottom);      /* cards never draw over the title or Back button */
    for (int i = 0; i < NUM_CREDITS; i++) {
        CreditEntry* c = &g_credits[i];
        DrawCardAA(hdc, c->card, 12, panel, border);

        /* role label, right-aligned and muted */
        HGDIOBJ of = SelectObject(hdc, g_fontSmall);
        SetTextColor(hdc, DubKindColor(DUBC_MUTED));
        RECT rr = { c->card.left + cardPad, c->card.top + 10, c->card.right - cardPad, c->card.top + 10 + hHdr };
        DrawTextW(hdc, c->role, -1, &rr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        /* name */
        SelectObject(hdc, g_fontGroupHeader);
        SetTextColor(hdc, DubKindColor(DUBC_ACCENT));
        RECT nr = { c->card.left + cardPad, c->card.top + 10, c->card.right - cardPad - 130, c->card.top + 10 + hHdr };
        DrawTextW(hdc, c->name, -1, &nr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

        /* detail */
        SelectObject(hdc, g_fontSmall);
        SetTextColor(hdc, text);
        RECT dr = { c->card.left + cardPad, c->card.top + 10 + hHdr + 2, c->card.right - cardPad, c->card.bottom - 6 };
        DrawTextW(hdc, c->detail, -1, &dr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

        /* link */
        if (c->url) {
            SelectObject(hdc, g_setLinkFont ? g_setLinkFont : g_fontSmall);
            SetTextColor(hdc, DubKindColor(DUBC_ACCENT));
            DrawTextW(hdc, c->url, -1, &c->link, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(hdc, of);
    }
    RestoreDC(hdc, savedDc);

    /* thin scroll bar when the list is taller than the visible band */
    int view = g_credBottom - g_credTop;
    if (g_credTotal > view && view > 0) {
        RECT track = { g_credW - 12, g_credTop, g_credW - 8, g_credBottom };
        DrawRoundedAA(hdc, track, 2, UkLerp(Pal()->bg, Pal()->border, 0.6), 255, 0, 0);
        int thumbH = (int)((double)view * view / g_credTotal);
        if (thumbH < 36) thumbH = 36;
        int maxScroll = g_credTotal - view;
        int thumbY = g_credTop + (int)((double)(view - thumbH) * g_credScroll / (maxScroll ? maxScroll : 1));
        RECT thumb = { g_credW - 12, thumbY, g_credW - 8, thumbY + thumbH };
        DrawRoundedAA(hdc, thumb, 2, DubKindColor(DUBC_ACCENT), 255, DubKindColor(DUBC_ACCENT), 0);
    }
}

static const wchar_t* SettingsLinkAt(POINT pt)
{
    if (!g_setCredits) return NULL;
    for (int i = 0; i < NUM_CREDITS; i++)
        if (g_credits[i].url && PtInRect(&g_credits[i].link, pt) && pt.y >= g_credTop && pt.y < g_credBottom) return g_credits[i].url;
    return NULL;
}

/* ---- volume slider ---- */

static int SettingsSliderTL(int i) { return g_sliders[i].area.left + 9; }
static int SettingsSliderTR(int i) { return g_sliders[i].area.right - 9; }

static void SettingsSetSliderFromX(HWND hwnd, int i, int x)
{
    int tl = SettingsSliderTL(i), tr = SettingsSliderTR(i);
    if (tr <= tl) return;
    int v = (int)(((long long)(x - tl) * 100 + (tr - tl) / 2) / (tr - tl));
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (v == *g_sliders[i].value) return;
    *g_sliders[i].value = v;
    if (g_sliders[i].apply) g_sliders[i].apply();      /* live: you hear / see it as you drag */
    InvalidateRect(hwnd, &g_sliders[i].area, FALSE);
    InvalidateRect(hwnd, &g_sliders[i].pct, FALSE);
}

static void SettingsPaintSlider(HDC hdc, int i)
{
    BOOL d = g_darkMode;
    int tl = SettingsSliderTL(i), tr = SettingsSliderTR(i);
    int cy = (g_sliders[i].area.top + g_sliders[i].area.bottom) / 2;
    int thumbX = tl + (tr - tl) * (*g_sliders[i].value) / 100;
    COLORREF accent = DubKindColor(DUBC_ACCENT);

    HPEN none = (HPEN)GetStockObject(NULL_PEN);
    HGDIOBJ op = SelectObject(hdc, none);

    HBRUSH track = CreateSolidBrush(UkLerp(Pal()->panel, Pal()->text, 0.22));
    SelectObject(hdc, track);
    RoundRect(hdc, tl, cy - 3, tr + 1, cy + 4, 7, 7);
    DeleteObject(track);

    if (thumbX > tl) {
        HBRUSH fill = CreateSolidBrush(accent);
        SelectObject(hdc, fill);
        RoundRect(hdc, tl, cy - 3, thumbX + 1, cy + 4, 7, 7);
        DeleteObject(fill);
    }

    HPEN ring = CreatePen(PS_SOLID, 2, accent);
    HBRUSH knob = CreateSolidBrush(d ? RGB(235, 235, 235) : RGB(255, 255, 255));
    SelectObject(hdc, ring);
    HGDIOBJ ob = SelectObject(hdc, knob);
    Ellipse(hdc, thumbX - 9, cy - 9, thumbX + 10, cy + 10);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(ring);
    DeleteObject(knob);
}

/* which slider (if any) is under the point */
static int SettingsSliderAt(POINT pt)
{
    for (int i = 0; i < 2; i++) {
        RECT r = g_sliders[i].area;
        InflateRect(&r, 4, 6);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

/* a round colour swatch: filled with that theme's accent, ringed when it is the chosen one */
static void DrawThemeSwatch(LPDRAWITEMSTRUCT dis, int idx)
{
    BOOL selected = (idx == g_themeIdx);
    BOOL hovered = GetPropW(dis->hwndItem, L"CVHover") != NULL;
    BOOL d = g_darkMode;
    PaintButtonSurround(dis->hwndItem, dis->hDC, dis->rcItem);
    COLORREF col = (idx == 0) ? RGB(150, 153, 162) : (d ? g_darkPals[idx].accent : g_lightPals[idx].accent);
    COLORREF contrast = d ? RGB(240, 240, 245) : RGB(40, 40, 50);
    int w = dis->rcItem.right - dis->rcItem.left;
    if (selected) {
        RECT in = dis->rcItem;
        DrawRoundedAA(dis->hDC, in, w / 2, d ? Pal()->bg : Pal()->bg, 255, contrast, 2);
        InflateRect(&in, -5, -5);
        DrawRoundedAA(dis->hDC, in, (w - 10) / 2, col, 255, col, 0);
    } else {
        RECT in = dis->rcItem;
        if (hovered) InflateRect(&in, 0, 0); else InflateRect(&in, -2, -2);
        DrawRoundedAA(dis->hDC, in, (in.right - in.left) / 2, col, 255, hovered ? contrast : col, hovered ? 1 : 0);
    }
}

static HWND DubLabel(HWND parent, HINSTANCE hInst, const wchar_t* text, HFONT font, DWORD extraStyle);
static HWND DubEdit(HWND parent, HINSTANCE hInst, DWORD style, HFONT font);

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
        g_hSetTitle = DubLabel(hwnd, hInst, L"Settings", g_fontTitle, 0);
        g_hSetDarkBtn = MakeButton(hwnd, hInst, L"Dark mode", 0, 0, 10, 10, ID_SET_DARK, g_fontRegular);
        g_hSetThemeLbl = DubLabel(hwnd, hInst, L"Color", g_fontRegular, 0);
        for (int i = 0; i < NUM_THEMES; i++)
            g_hSetThemeBtn[i] = MakeButton(hwnd, hInst, L"", 0, 0, 10, 10, ID_SET_THEME0 + i, g_fontRegular);
        g_hSetStarsLbl = DubLabel(hwnd, hInst, L"Background stars", g_fontRegular, 0);
        g_sliders[0].value = &g_musicVolume; g_sliders[0].apply = MusicApplyVolume; g_sliders[0].fmt = L"%d%%";
        g_sliders[1].value = &g_starCount;   g_sliders[1].apply = ApplyStarCount;   g_sliders[1].fmt = L"%d";
        g_hSetMusicBtn = MakeButton(hwnd, hInst, L"", 0, 0, 10, 10, ID_MUSIC, g_fontIcon);
        g_hSetMusicLbl = DubLabel(hwnd, hInst, L"Music", g_fontRegular, 0);
        g_hSetFolderBtn = MakeButton(hwnd, hInst, L"Open Modding Folder", 0, 0, 10, 10, ID_SET_FOLDER, g_fontRegular);
        g_hSetReadmeBtn = MakeButton(hwnd, hInst, L"Open README", 0, 0, 10, 10, ID_SET_README, g_fontRegular);
        g_hSetCreditsBtn = MakeButton(hwnd, hInst, L"Credits", 0, 0, 10, 10, ID_SET_CREDITS, g_fontRegular);
        g_hSetBackBtn = MakeButton(hwnd, hInst, L"\u25C0 Back", 0, 0, 10, 10, ID_SET_BACK, g_fontRegular);
        g_hSetAboutBtn = MakeButton(hwnd, hInst, L"About this launcher", 0, 0, 10, 10, ID_SET_ABOUT, g_fontRegular);
        g_hAbGameBtn = MakeButton(hwnd, hInst, L"Game page", 0, 0, 10, 10, ID_AB_GAME, g_fontRegular);
        g_hAbGbBtn = MakeButton(hwnd, hInst, L"GameBanana page", 0, 0, 10, 10, ID_AB_GB, g_fontRegular);
        ShowWindow(g_hSetBackBtn, SW_HIDE);
        ShowWindow(g_hAbGameBtn, SW_HIDE);
        ShowWindow(g_hAbGbBtn, SW_HIDE);
        SettingsLayout(hwnd);
        SettingsRefresh();
        return 0;
    }

    case WM_SIZE:
        SettingsLayout(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
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
        if (g_setView == 1) {
            SettingsPaintCredits(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        if (g_setView == 2) {
            SettingsPaintAbout(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        for (int i = 0; i < 4; i++) DubPaintHeader(hdc, &g_setHdr[i], g_setHdrText[i]);
        for (int i = 0; i < 2; i++) {
            SettingsPaintSlider(hdc, i);
            wchar_t pct[16];
            _snwprintf(pct, 15, g_sliders[i].fmt, *g_sliders[i].value);
            pct[15] = 0;
            SelectObject(hdc, g_fontRegular);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, (i == 0 && !g_musicOn) ? DubKindColor(DUBC_MUTED) : (g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT));
            DrawTextW(hdc, pct, -1, &g_sliders[i].pct, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
        if (dis->CtlType != ODT_BUTTON) return FALSE;
        if (dis->CtlID == ID_MUSIC) {
            DrawMusicIcon(dis->hDC, dis->rcItem, g_darkMode ? COLOR_DARK_BG : COLOR_LIGHT_BG,
                GetPropW(dis->hwndItem, L"CVHover") != NULL, (dis->itemState & ODS_SELECTED) != 0, g_musicOn);
            return TRUE;
        }
        if (dis->CtlID >= ID_SET_THEME0 && dis->CtlID < ID_SET_THEME0 + NUM_THEMES) {
            DrawThemeSwatch(dis, (int)(dis->CtlID - ID_SET_THEME0));
            return TRUE;
        }
        DrawThemedOwnerButton(dis);
        return TRUE;
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND ctl = (HWND)lParam;
        if (ctl == g_hSetInfoEdit) {
            SetBkColor(hdc, g_darkMode ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL);
            SetTextColor(hdc, g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT);
            return (LRESULT)(g_hBrushPanel ? g_hBrushPanel : GetSysColorBrush(COLOR_WINDOW));
        }
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, (ctl == g_hSetMusicLbl && !g_musicOn) ? DubKindColor(DUBC_MUTED)
                                                                 : (g_darkMode ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT));
        return (LRESULT)(g_hBrushBg ? g_hBrushBg : GetSysColorBrush(COLOR_BTNFACE));
    }

    case WM_LBUTTONDOWN: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        const wchar_t* link = SettingsLinkAt(pt);
        if (link) { ShellExecuteW(g_hMainWnd, L"open", link, NULL, NULL, SW_SHOWNORMAL); return 0; }
        int si = (g_setView != 0) ? -1 : SettingsSliderAt(pt);
        if (si >= 0) {
            g_slDrag = si;
            SetCapture(hwnd);
            SettingsSetSliderFromX(hwnd, si, pt.x);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_slDrag >= 0) SettingsSetSliderFromX(hwnd, g_slDrag, (short)LOWORD(lParam));
        return 0;
    case WM_LBUTTONUP:
        if (g_slDrag >= 0) {
            g_slDrag = -1;
            ReleaseCapture();
            SaveSettings();
        }
        return 0;
    case WM_CAPTURECHANGED:
        g_slDrag = -1;
        return 0;
    case WM_MOUSEWHEEL: {
        if (g_setView == 1) {                       /* credits page: scroll the list */
            g_credScroll -= (GET_WHEEL_DELTA_WPARAM(wParam) / 120) * 56;
            SettingsLayoutCredits(hwnd, g_credW, g_credH, g_credTop, g_credBottom);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        POINT pt; GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        int si = (g_setView != 0) ? -1 : SettingsSliderAt(pt);
        if (si >= 0) {
            int v = *g_sliders[si].value + (GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 5 : -5);
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            *g_sliders[si].value = v;
            if (g_sliders[si].apply) g_sliders[si].apply();
            SaveSettings();
            InvalidateRect(hwnd, &g_sliders[si].area, FALSE);
            InvalidateRect(hwnd, &g_sliders[si].pct, FALSE);
            return 0;
        }
        break;
    }
    case WM_SETCURSOR: {
        POINT pt; GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (g_slDrag >= 0 || SettingsLinkAt(pt) || (g_setView == 0 && SettingsSliderAt(pt) >= 0)) {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;
        }
        break;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) >= ID_SET_THEME0 && LOWORD(wParam) < ID_SET_THEME0 + NUM_THEMES) {
            SettingsSetTheme((int)(LOWORD(wParam) - ID_SET_THEME0));
            return 0;
        }
        switch (LOWORD(wParam)) {
        case ID_SET_DARK:   SettingsToggleDark(); return 0;
        case ID_MUSIC:      MusicSetOn(g_hMainWnd, !g_musicOn); return 0;
        case ID_SET_FOLDER: OpenModdingFolder(g_hMainWnd); return 0;
        case ID_SET_README: ShellExecuteW(g_hMainWnd, L"open", README_URL, NULL, NULL, SW_SHOWNORMAL); return 0;
        case ID_SET_CREDITS: SettingsShowView(1); return 0;
        case ID_SET_ABOUT:   SettingsShowView(2); return 0;
        case ID_SET_BACK:    SettingsShowView(g_setView == 1 ? g_setPrevView : 0); return 0;
        case ID_AB_GAME:     ShellExecuteW(g_hMainWnd, L"open", L"https://yeahmaybe.itch.io/the-choicer-voicer", NULL, NULL, SW_SHOWNORMAL); return 0;
        case ID_AB_GB:       ShellExecuteW(g_hMainWnd, L"open", L"https://gamebanana.com/games/20674", NULL, NULL, SW_SHOWNORMAL); return 0;
        }
        break;

    case WM_DESTROY:
        g_hSettingsWnd = NULL;
        g_hSetInfoEdit = NULL;
        g_hSetMusicBtn = NULL;
        g_hSetDarkBtn = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static HWND CreateSettingsWnd(HWND parent, HINSTANCE hInst, int x, int y, int w, int h)
{
    static BOOL classRegistered = FALSE;
    const wchar_t CLASS_NAME[] = L"CVLauncherSettingsWnd";
    if (!classRegistered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc   = SettingsWndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = CLASS_NAME;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        RegisterClassW(&wc);
        classRegistered = TRUE;
    }
    HWND hwnd = CreateWindowExW(0, CLASS_NAME, NULL,
        WS_CHILD | WS_CLIPCHILDREN, x, y, w, h, parent, NULL, hInst, NULL);
    g_hSettingsWnd = hwnd;
    return hwnd;
}
