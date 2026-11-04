/*
 * LayoutGuard - tiny Windows tray app that fixes words typed in the wrong keyboard layout.
 *
 * How it works
 *   - A low-level keyboard hook remembers the keys of the current word (in RAM only).
 *   - When a word ends (Space / Enter / Tab) it is checked with the Windows spell checker
 *     for each installed layout's language.
 *   - If the word is not valid in the active layout's language, but the same keys form a
 *     valid word in another installed layout's language, the word is erased, retyped
 *     correctly and the keyboard layout is switched.
 *   - Nothing is written to disk and nothing leaves the machine.
 *
 * Optional override: a UTF-8 word list at  <exe folder>\dict\<lang>.txt (one lowercase word
 * per line) is used instead of the Windows spell checker for that language.
 *
 * Build (MinGW): x86_64-w64-mingw32-gcc -O2 -s -static -mwindows -municode LayoutGuard.c -o LayoutGuard.exe -luser32 -lshell32 -lole32
 * Build (MSVC):  cl /O2 /MT /DUNICODE /D_UNICODE LayoutGuard.c /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib ole32.lib
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <spellcheck.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define DOWN(vk)     ((GetAsyncKeyState(vk) & 0x8000) != 0)
#define MAX_TOK      48          /* longest word we track; longer input resets the buffer */
#define MAX_LAYOUTS  16
#define MAX_LANGS    16
#define MAX_DICTS    16
#define WM_TRAY      (WM_APP + 1)
#define IDM_TOGGLE   1001
#define IDM_EXIT     1002
#define IDM_STATUS   1003

typedef struct {
    UINT  vk, sc;     /* virtual key and scan code of the physical key */
    HKL   hkl;        /* layout that was active when the key was pressed */
    BYTE  shift, caps, altgr;
} Tok;

typedef struct {
    wchar_t  lang[8];   /* primary language, e.g. "en" */
    wchar_t *text;      /* whole file, UTF-16 (words point into it) */
    wchar_t **words;    /* sorted, lowercase */
    size_t   n;
} Dict;

typedef struct {
    wchar_t        code[8];   /* primary language, e.g. "he" */
    Dict          *dict;      /* optional word-list override */
    ISpellChecker *sc;        /* Windows spell checker, used when no word list */
} Lang;

typedef struct {
    HKL hkl;
    int lang;                 /* index into g_langs */
} Layout;

static Tok    g_tok[MAX_TOK];
static int    g_ntok;
static Layout g_lay[MAX_LAYOUTS];
static int    g_nlay;
static Lang   g_langs[MAX_LANGS];
static int    g_nlang;
static Dict   g_dicts[MAX_DICTS];
static int    g_ndicts;
static ISpellCheckerFactory *g_fac;
static wchar_t g_dir[MAX_PATH];
static HHOOK  g_kh, g_mh;
static HWND   g_hwnd, g_lastfg;
static NOTIFYICONDATAW g_nid;
static BOOL   g_enabled = TRUE;
static BOOL   g_injecting;
/* counters only - no typed text is kept */
static int    g_stat_delims, g_stat_unmatched, g_stat_checks, g_stat_fixes;
static int    g_stat_keys;             /* key-down events the hook received */
static DWORD  g_last_key_tick;         /* GetTickCount of the last key-down */
static DWORD  g_slowest_ms;            /* slowest word decision, in ms */
static int    g_selftest[MAX_LANGS];   /* 0 = no sample word, 1 = passed, 2 = failed */
static BOOL   g_kh_ok, g_mh_ok;

/* ---------- word checking ---------- */

static int cmp_word(const void *a, const void *b)
{
    return wcscmp(*(wchar_t *const *)a, *(wchar_t *const *)b);
}

static BOOL in_dict(const Dict *d, const wchar_t *s, int len)
{
    wchar_t tmp[MAX_TOK + 1];
    const wchar_t *key = tmp;
    if (!d || !d->n || len < 2 || len > MAX_TOK) return FALSE;
    memcpy(tmp, s, len * sizeof(wchar_t));
    tmp[len] = 0;
    CharLowerBuffW(tmp, len);
    return bsearch(&key, d->words, d->n, sizeof *d->words, cmp_word) != NULL;
}

static Dict *load_dict(const wchar_t *lang)
{
    wchar_t path[MAX_PATH];
    HANDLE f;
    DWORD size, got = 0;
    char *raw;
    wchar_t *w, *p, **words, *eol;
    int wlen, i;
    size_t cap = 1, n = 0, len;
    Dict *d;

    for (i = 0; i < g_ndicts; i++)
        if (!wcscmp(g_dicts[i].lang, lang))
            return g_dicts[i].n ? &g_dicts[i] : NULL;
    if (g_ndicts >= MAX_DICTS) return NULL;

    d = &g_dicts[g_ndicts++];
    memset(d, 0, sizeof *d);
    lstrcpynW(d->lang, lang, 8);

    lstrcpyW(path, g_dir);
    lstrcatW(path, L"dict\\");
    lstrcatW(path, lang);
    lstrcatW(path, L".txt");

    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(f, NULL);
    if (size == INVALID_FILE_SIZE || size > 64u * 1024 * 1024) { CloseHandle(f); return NULL; }
    raw = malloc(size + 1);
    if (!raw || !ReadFile(f, raw, size, &got, NULL)) { free(raw); CloseHandle(f); return NULL; }
    CloseHandle(f);

    wlen = MultiByteToWideChar(CP_UTF8, 0, raw, (int)got, NULL, 0);
    w = malloc((wlen + 1) * sizeof(wchar_t));
    if (!w) { free(raw); return NULL; }
    MultiByteToWideChar(CP_UTF8, 0, raw, (int)got, w, wlen);
    w[wlen] = 0;
    free(raw);

    for (i = 0; i < wlen; i++) if (w[i] == L'\n') cap++;
    words = malloc(cap * sizeof *words);
    if (!words) { free(w); return NULL; }

    for (p = w; *p; ) {
        eol = wcschr(p, L'\n');
        if (eol) *eol = 0;
        while (*p && (*p <= L' ' || *p == 0xFEFF)) p++;      /* trim, also UTF-8 BOM */
        len = wcslen(p);
        while (len && p[len - 1] <= L' ') p[--len] = 0;
        if (len >= 2 && len <= MAX_TOK) {
            CharLowerBuffW(p, (DWORD)len);
            words[n++] = p;
        }
        if (!eol) break;
        p = eol + 1;
    }
    qsort(words, n, sizeof *words, cmp_word);

    d->text = w;
    d->words = words;
    d->n = n;
    return n ? d : NULL;
}

/* Is s (len chars) a correctly spelled word in language L? */
static BOOL is_word(const Lang *L, const wchar_t *s, int len)
{
    wchar_t tmp[MAX_TOK + 1];
    IEnumSpellingError *en = NULL;
    ISpellingError *err = NULL;
    HRESULT hr;
    BOOL ok;

    if (len < 2 || len > MAX_TOK) return FALSE;
    if (L->dict) return in_dict(L->dict, s, len);
    if (!L->sc) return FALSE;

    memcpy(tmp, s, len * sizeof(wchar_t));
    tmp[len] = 0;
    hr = L->sc->lpVtbl->Check(L->sc, tmp, &en);
    if (FAILED(hr) || !en) return FALSE;
    /* Enumerator yields one error item per misspelling; S_FALSE on the first call = no errors. */
    hr = en->lpVtbl->Next(en, &err);
    ok = (hr == S_FALSE);
    if (err) err->lpVtbl->Release(err);
    en->lpVtbl->Release(en);
    return ok;
}

/* ---------- languages and layouts ---------- */

static ISpellChecker *make_checker(const wchar_t *tag, const wchar_t *code)
{
    const wchar_t *cands[2];
    int i;

    if (!g_fac) return NULL;
    cands[0] = tag;
    cands[1] = code;
    for (i = 0; i < 2; i++) {
        WINBOOL supported = FALSE;
        ISpellChecker *sc = NULL;
        if (SUCCEEDED(g_fac->lpVtbl->IsSupported(g_fac, cands[i], &supported)) && supported &&
            SUCCEEDED(g_fac->lpVtbl->CreateSpellChecker(g_fac, cands[i], &sc)) && sc)
            return sc;
    }
    return NULL;
}

static int find_or_add_lang(const wchar_t *tag, const wchar_t *code)
{
    int i;
    Lang *L;
    for (i = 0; i < g_nlang; i++)
        if (!wcscmp(g_langs[i].code, code)) return i;
    if (g_nlang >= MAX_LANGS) return -1;

    L = &g_langs[g_nlang];
    memset(L, 0, sizeof *L);
    lstrcpynW(L->code, code, 8);
    L->dict = load_dict(code);
    if (!L->dict) L->sc = make_checker(tag, code);
    return g_nlang++;
}

static void init_layouts(void)
{
    int i, n;
    HKL *list;

    CoCreateInstance(&CLSID_SpellCheckerFactory, NULL, CLSCTX_INPROC_SERVER,
                     &IID_ISpellCheckerFactory, (void **)&g_fac);

    n = GetKeyboardLayoutList(0, NULL);
    if (n <= 0) return;
    list = malloc(n * sizeof(HKL));
    if (!list) return;
    n = GetKeyboardLayoutList(n, list);

    for (i = 0; i < n && g_nlay < MAX_LAYOUTS; i++) {
        wchar_t name[LOCALE_NAME_MAX_LENGTH], code[8];
        int li, k = 0;
        LCID lcid = MAKELCID((LANGID)((UINT_PTR)list[i] & 0xFFFF), SORT_DEFAULT);
        if (!LCIDToLocaleName(lcid, name, LOCALE_NAME_MAX_LENGTH, 0)) continue;
        for (; name[k] && name[k] != L'-' && k < 7; k++) code[k] = name[k];
        code[k] = 0;
        CharLowerBuffW(code, k);

        li = find_or_add_lang(name, code);
        if (li < 0) continue;
        if (!g_langs[li].dict && !g_langs[li].sc) continue;   /* no checker for this language */

        g_lay[g_nlay].hkl = list[i];
        g_lay[g_nlay].lang = li;
        g_nlay++;
    }
    free(list);
}

static int lang_of_layout(HKL h)
{
    int i;
    for (i = 0; i < g_nlay; i++)
        if (g_lay[i].hkl == h) return g_lay[i].lang;
    /* fall back to the language id: the device half of the HKL can differ between APIs */
    for (i = 0; i < g_nlay; i++)
        if (((UINT_PTR)g_lay[i].hkl & 0xFFFF) == ((UINT_PTR)h & 0xFFFF)) return g_lay[i].lang;
    return -1;
}

static HKL fg_layout(void)
{
    return GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), NULL));
}

/* Character a tracked key produces under layout h, or 0 if it is not exactly one char. */
static int tok_char(const Tok *t, HKL h, wchar_t *out)
{
    BYTE ks[256];
    wchar_t buf[8];
    int r;

    memset(ks, 0, sizeof ks);
    if (t->shift) ks[VK_SHIFT] = 0x80;
    if (t->caps)  ks[VK_CAPITAL] = 0x01;
    if (t->altgr) { ks[VK_CONTROL] = 0x80; ks[VK_MENU] = 0x80; }
    /* flag 4 = do not change dead-key state */
    r = ToUnicodeEx(t->vk, t->sc, ks, buf, 8, 4, h);
    if (r != 1) return 0;
    *out = buf[0];
    return 1;
}

/* Text of the buffered word. force==NULL: each key under the layout it was typed in. */
static int build_text(HKL force, wchar_t *out)
{
    int i;
    for (i = 0; i < g_ntok; i++) {
        if (!tok_char(&g_tok[i], force ? force : g_tok[i].hkl, &out[i])) return 0;
    }
    out[g_ntok] = 0;
    return g_ntok;
}

/* Decide whether the buffered word was typed in the wrong layout. */
static BOOL decide(wchar_t out[MAX_TOK + 1], HKL *target)
{
    wchar_t actual[MAX_TOK + 1];
    int alen, cur, i;

    if (!g_ntok) return FALSE;
    alen = build_text(NULL, actual);
    if (alen <= 0) return FALSE;

    cur = lang_of_layout(g_tok[g_ntok - 1].hkl);
    if (cur < 0) { g_stat_unmatched++; return FALSE; }
    g_stat_checks++;
    if (is_word(&g_langs[cur], actual, alen)) return FALSE;   /* valid where typed */

    for (i = 0; i < g_nlay; i++) {
        const Layout *L = &g_lay[i];
        wchar_t cand[MAX_TOK + 1];
        int clen;
        if (L->lang == cur) continue;
        clen = build_text(L->hkl, cand);
        if (clen > 0 && is_word(&g_langs[L->lang], cand, clen)) {
            memcpy(out, cand, (clen + 1) * sizeof(wchar_t));
            *target = L->hkl;
            return TRUE;
        }
    }
    return FALSE;
}

/* ---------- injecting the correction ---------- */

static void add_key(INPUT *in, int *n, WORD vk, WORD scan, DWORD flags)
{
    INPUT *x = &in[(*n)++];
    memset(x, 0, sizeof *x);
    x->type = INPUT_KEYBOARD;
    x->ki.wVk = vk;
    x->ki.wScan = scan;
    x->ki.dwFlags = flags;
}

static void correct(int ntok, const wchar_t *text, UINT delim, HKL target)
{
    INPUT in[256];
    int n = 0, i, len = (int)wcslen(text);

    for (i = 0; i < ntok; i++) {                 /* erase the wrong word */
        add_key(in, &n, VK_BACK, 0, 0);
        add_key(in, &n, VK_BACK, 0, KEYEVENTF_KEYUP);
    }
    for (i = 0; i < len; i++) {                  /* retype it (layout-independent) */
        add_key(in, &n, 0, text[i], KEYEVENTF_UNICODE);
        add_key(in, &n, 0, text[i], KEYEVENTF_UNICODE | KEYEVENTF_KEYUP);
    }
    add_key(in, &n, (WORD)delim, 0, 0);          /* re-send the swallowed delimiter */
    add_key(in, &n, (WORD)delim, 0, KEYEVENTF_KEYUP);

    g_stat_fixes++;
    g_injecting = TRUE;
    SendInput(n, in, sizeof in[0]);
    g_injecting = FALSE;

    /* switch the focused window to the layout the word belongs to */
    {
        HWND fg = GetForegroundWindow();
        if (fg) PostMessageW(fg, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)target);
    }
}

/* ---------- hooks ---------- */

static LRESULT CALLBACK kb_proc(int code, WPARAM wp, LPARAM lp)
{
    const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
    HWND fg;
    UINT vk;
    BOOL altgr, ctrl, alt, win;
    Tok t;
    wchar_t c;

    if (code != HC_ACTION || (wp != WM_KEYDOWN && wp != WM_SYSKEYDOWN) ||
        g_injecting || (k->flags & LLKHF_INJECTED))
        return CallNextHookEx(NULL, code, wp, lp);

    g_stat_keys++;
    g_last_key_tick = GetTickCount();
    fg = GetForegroundWindow();
    if (fg != g_lastfg) { g_lastfg = fg; g_ntok = 0; }   /* focus moved: forget the word */

    vk = k->vkCode;
    switch (vk) {                                /* pure modifier presses keep the word */
    case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL:
    case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_CAPITAL:
        return CallNextHookEx(NULL, code, wp, lp);
    }

    if (!g_enabled) { g_ntok = 0; return CallNextHookEx(NULL, code, wp, lp); }

    altgr = DOWN(VK_RMENU);
    ctrl  = DOWN(VK_LCONTROL) || DOWN(VK_RCONTROL);
    alt   = DOWN(VK_LMENU);
    win   = DOWN(VK_LWIN) || DOWN(VK_RWIN);
    if (win || alt || (ctrl && !altgr)) {       /* shortcut, not text */
        g_ntok = 0;
        return CallNextHookEx(NULL, code, wp, lp);
    }

    switch (vk) {
    case VK_BACK:
        if (g_ntok) g_ntok--;
        return CallNextHookEx(NULL, code, wp, lp);

    case VK_SPACE:
    case VK_RETURN:
    case VK_TAB: {
        wchar_t fixed[MAX_TOK + 1];
        HKL target = NULL;
        int ntok = g_ntok;
        g_stat_delims++;
        BOOL fix;
        {
            DWORD t0 = GetTickCount();
            fix = decide(fixed, &target);
            if (GetTickCount() - t0 > g_slowest_ms) g_slowest_ms = GetTickCount() - t0;
        }
        g_ntok = 0;
        if (fix) {
            correct(ntok, fixed, vk, target);
            return 1;                            /* swallow: correct() re-sends it */
        }
        return CallNextHookEx(NULL, code, wp, lp);
    }

    case VK_ESCAPE: case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT: case VK_DELETE: case VK_INSERT:
        g_ntok = 0;                              /* caret moved: we no longer know the text */
        return CallNextHookEx(NULL, code, wp, lp);
    }

    memset(&t, 0, sizeof t);
    t.vk = vk;
    t.sc = k->scanCode;
    t.hkl = fg_layout();
    t.shift = DOWN(VK_LSHIFT) || DOWN(VK_RSHIFT);
    t.caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
    t.altgr = altgr;

    if (!tok_char(&t, t.hkl, &c)) {              /* not a character key (F-keys, dead keys...) */
        g_ntok = 0;
        return CallNextHookEx(NULL, code, wp, lp);
    }
    if (g_ntok >= MAX_TOK) g_ntok = 0;
    g_tok[g_ntok++] = t;
    return CallNextHookEx(NULL, code, wp, lp);
}

static LRESULT CALLBACK ms_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION &&
        (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN || wp == WM_XBUTTONDOWN))
        g_ntok = 0;                              /* click moves the caret */
    return CallNextHookEx(NULL, code, wp, lp);
}

/* ---------- self-test and status ---------- */

static const struct { const wchar_t *code, *word; } g_samples[] = {
    {L"en", L"hello"}, {L"es", L"hola"}, {L"fr", L"bonjour"}, {L"de", L"hallo"},
    {L"it", L"ciao"}, {L"pt", L"obrigado"},
    {L"ru", L"\u043F\u0440\u0438\u0432\u0435\u0442"},          /* привет */
    {L"he", L"\u05E9\u05DC\u05D5\u05DD"},                        /* שלום */
    {L"el", L"\u03B3\u03B5\u03B9\u03B1"},                        /* γεια */
    {L"ar", L"\u0645\u0631\u062D\u0628\u0627"},                 /* مرحبا */
};

static void self_test_all(void)
{
    int i, k, n = (int)(sizeof g_samples / sizeof g_samples[0]);
    for (i = 0; i < g_nlang; i++) {
        g_selftest[i] = 0;
        for (k = 0; k < n; k++) {
            if (!wcscmp(g_samples[k].code, g_langs[i].code)) {
                const wchar_t *w = g_samples[k].word;
                g_selftest[i] = is_word(&g_langs[i], w, (int)wcslen(w)) ? 1 : 2;
                break;
            }
        }
    }
}

static void show_status(void)
{
    wchar_t msg[2048], line[256];
    int i;

    lstrcpyW(msg, L"Layout Guard status\n\n");
    wsprintfW(line, L"Keyboard hook: %s\nMouse hook: %s\n\n",
              g_kh_ok ? L"running" : L"FAILED", g_mh_ok ? L"running" : L"FAILED");
    lstrcatW(msg, line);

    for (i = 0; i < g_nlang; i++) {
        const wchar_t *how = g_langs[i].dict ? L"word list" :
                             (g_langs[i].sc ? L"Windows spell checker" : L"none");
        const wchar_t *res = g_selftest[i] == 1 ? L"self-test passed" :
                             g_selftest[i] == 2 ? L"self-test FAILED" : L"no self-test word";
        wsprintfW(line, L"%s: %s, %s\n", g_langs[i].code, how, res);
        lstrcatW(msg, line);
    }

    wsprintfW(line, L"\nLayouts in use: %d\nKey presses seen by hook: %d\n"
                    L"Seconds since last key press: %d\n"
                    L"Word endings seen (Space/Enter/Tab): %d\nWords checked: %d\n"
                    L"Keys on an unrecognized layout: %d\nCorrections made: %d\n"
                    L"Slowest word check: %d ms\n",
              g_nlay, g_stat_keys,
              g_last_key_tick ? (int)((GetTickCount() - g_last_key_tick) / 1000) : -1,
              g_stat_delims, g_stat_checks, g_stat_unmatched, g_stat_fixes, (int)g_slowest_ms);
    lstrcatW(msg, line);

    MessageBoxW(NULL, msg, L"Layout Guard", MB_OK | MB_ICONINFORMATION);
}

/* ---------- tray UI ---------- */

static void tray_add(void)                       /* called once at startup */
{
    wchar_t tip[128];
    wsprintfW(tip, L"Layout Guard: %s, %d layout(s) active",
              g_enabled ? L"on" : L"paused", g_nlay);
    memset(&g_nid, 0, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    lstrcpynW(g_nid.szTip, tip, ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void set_enabled(BOOL on)
{
    wchar_t tip[128];
    g_enabled = on;
    g_ntok = 0;
    wsprintfW(tip, L"Layout Guard: %s, %d layout(s) active",
              g_enabled ? L"on" : L"paused", g_nlay);
    g_nid.uFlags = NIF_TIP;
    lstrcpynW(g_nid.szTip, tip, ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void show_menu(HWND h)
{
    POINT p;
    HMENU m = CreatePopupMenu();
    GetCursorPos(&p);
    AppendMenuW(m, MF_STRING | (g_enabled ? MF_CHECKED : 0), IDM_TOGGLE, L"Enabled");
    AppendMenuW(m, MF_STRING, IDM_STATUS, L"Status...");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
    SetForegroundWindow(h);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, h, NULL);
    DestroyMenu(m);
    PostMessageW(h, WM_NULL, 0, 0);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_TRAY:
        if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) show_menu(h);
        else if (LOWORD(l) == WM_LBUTTONUP) set_enabled(!g_enabled);
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) == IDM_TOGGLE) set_enabled(!g_enabled);
        else if (LOWORD(w) == IDM_STATUS) show_status();
        else if (LOWORD(w) == IDM_EXIT) DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---------- entry ---------- */

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmd, int show)
{
    WNDCLASSW wc;
    MSG msg;
    HANDLE mx;
    wchar_t *slash;

    (void)hp; (void)cmd; (void)show;

    mx = CreateMutexW(NULL, TRUE, L"Local\\LayoutGuard.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    GetModuleFileNameW(NULL, g_dir, MAX_PATH);
    slash = wcsrchr(g_dir, L'\\');
    if (slash) slash[1] = 0;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    init_layouts();
    self_test_all();

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hi;
    wc.lpszClassName = L"LayoutGuardWnd";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowW(L"LayoutGuardWnd", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hi, NULL);

    tray_add();
    g_kh = SetWindowsHookExW(WH_KEYBOARD_LL, kb_proc, GetModuleHandleW(NULL), 0);
    g_mh = SetWindowsHookExW(WH_MOUSE_LL, ms_proc, GetModuleHandleW(NULL), 0);
    g_kh_ok = g_kh != NULL;
    g_mh_ok = g_mh != NULL;
    if (!g_kh_ok || !g_mh_ok)
        MessageBoxW(NULL, L"Layout Guard could not install its keyboard/mouse hooks.",
                    L"Layout Guard", MB_OK | MB_ICONERROR);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_kh) UnhookWindowsHookEx(g_kh);
    if (g_mh) UnhookWindowsHookEx(g_mh);
    if (mx) CloseHandle(mx);
    return 0;
}
