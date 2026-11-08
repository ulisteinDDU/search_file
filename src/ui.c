/* ui.c - 界面构建与布局实现 */
#include "ui.h"
#include "util.h"
#include <windowsx.h>
#include <shobjidl.h>

static HINSTANCE g_hInst;
static double    g_mul = 1.0;
static HFONT     g_hFont;
static HWND      g_hTips;             /* 全局 Tooltip */

int S_(int v96) { return (int)(v96 * g_mul + 0.5); }
HFONT ui_font(void) { return g_hFont; }

static void add_tip(HWND panel, HWND ctrl, const wchar_t *txt)
{
    TOOLINFOW ti;
    if (!g_hTips || !ctrl) return;
    memset(&ti, 0, sizeof(ti));
    ti.cbSize   = sizeof(ti);
    ti.uFlags   = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd     = panel;
    ti.uId      = (UINT_PTR)ctrl;
    ti.lpszText = (LPWSTR)txt;
    SendMessageW(g_hTips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

/* ---------- 页面宿主窗口（仅负责 WM_SIZE 时重排子控件） ---------- */
static LRESULT CALLBACK PageProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_SIZE) {
        PageUI *pg = (PageUI *)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (pg && pg->feature >= 0)
            ui_layout_page(pg, LOWORD(l), HIWORD(l));
        return 0;
    }
    if (m == WM_COMMAND || m == WM_NOTIFY)     /* 转发给主窗口统一处理 */
        return SendMessageW(GetParent(h), m, w, l);
    return DefWindowProcW(h, m, w, l);
}

void ui_module_init(HINSTANCE hInst)
{
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = PageProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"QxPageWnd";
    RegisterClassW(&wc);
    g_hInst = hInst;

    g_hTips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                              WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                              NULL, NULL, hInst, NULL);
    if (g_hTips)
        SendMessageW(g_hTips, TTM_SETMAXTIPWIDTH, 0, S_(460));
}

void ui_init(HINSTANCE hInst)
{
    HDC dc = GetDC(NULL);
    int dpi;
    (void)hInst;
    dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    g_mul = dpi / 96.0;
    if (g_mul < 1.0) g_mul = 1.0;
    {
        NONCLIENTMETRICSW ncm;
        memset(&ncm, 0, sizeof(ncm));
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            g_hFont = CreateFontIndirectW(&ncm.lfMessageFont);
        }
    }
    if (!g_hFont)
        g_hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

/* ---------- 控件创建辅助（可见性由调用方在 style 中显式给出） ---------- */
static HWND mkwnd(const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex,
                  HWND parent, int id)
{
    return CreateWindowExW(ex, cls, text, WS_CHILD | style,
                           0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, g_hInst, NULL);
}
#define MKLBL(id, txt)  mkwnd(L"STATIC",  (txt), WS_VISIBLE | SS_LEFT, 0, pan, baseId + (id))
#define MKEDIT(id)      mkwnd(L"EDIT",    L"",     WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, pan, baseId + (id))
#define MKBTN(id, txt)  mkwnd(L"BUTTON",  (txt),   WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, pan, baseId + (id))
#define MKCHK(id, txt)  mkwnd(L"BUTTON",  (txt),   WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, pan, baseId + (id))

/* 后缀预设（文件级，供重建 [√] 标记列表复用） */
static const wchar_t *g_extPresets[] = {
    /* 文本类 */
    L"txt", L"log", L"ini", L"cfg", L"csv", L"md", L"html", L"htm",
    L"xml", L"json", L"rtf",
    /* 源代码 */
    L"c", L"cpp", L"h", L"hpp", L"cs", L"java", L"py", L"bat",
    L"ps1", L"sh", L"sql",
    /* Office / 文档 */
    L"doc", L"docx", L"xls", L"xlsx", L"ppt", L"pptx", L"pdf",
    /* 压缩 / 可执行等 */
    L"zip", L"rar", L"7z", L"exe", L"dll", NULL
};
static const wchar_t *g_custPresets[] = {
    L"txt", L"log", L"ini", L"cfg", L"csv", L"tsv", L"md", L"markdown",
    L"c", L"h", L"cpp", L"hpp", L"cc", L"cs", L"java", L"py", L"rb",
    L"go", L"rs", L"php", L"js", L"ts", L"css", L"html", L"htm",
    L"xml", L"json", L"yml", L"yaml", L"toml", L"sql", L"bat",
    L"ps1", L"sh", L"mak", L"rc", NULL
};

/* 每次下拉打开时重建列表：已选中的格式前加 “[√] ” 标记 */
void ui_rebuild_ext_list(HWND hCombo, int listKind, const wchar_t *currentText)
{
    const wchar_t *const *presets = listKind == 0 ? g_extPresets : g_custPresets;
    int withAll = (listKind == 0);
    wchar_t **items = NULL;
    size_t n = 0, i;
    int all = (!currentText || !*currentText ||
               !wcscmp(currentText, L"全部") || !wcscmp(currentText, L"*"));
    wchar_t buf[96];

    ext_list_parse(currentText, &items, &n);
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
    if (withAll) {
        swprintf(buf, 96, all ? L"[√] 全部" : L"全部");
        ComboBox_AddString(hCombo, buf);
    }
    for (i = 0; presets[i]; i++) {
        size_t j;
        int hit = 0;
        if (!all)
            for (j = 0; j < n; j++)
                if (!wcscmp(items[j], presets[i])) { hit = 1; break; }
        swprintf(buf, 96, hit ? L"[√] %ls" : L"%ls", presets[i]);
        ComboBox_AddString(hCombo, buf);
    }
    ext_list_free(items, n);
    SendMessageW(hCombo, CB_SETCURSEL, (WPARAM)-1, 0);
    SetWindowTextW(hCombo, currentText ? currentText : L"");   /* CB_RESETCONTENT 会清空编辑框，需恢复 */
}

static void set_combos(PageUI *pg)
{
    if (pg->feature == 0) {
        ComboBox_AddString(pg->hMatch, L"全字匹配");
        ComboBox_AddString(pg->hMatch, L"包含匹配");
        ComboBox_SetCurSel(pg->hMatch, 1);      /* 默认包含匹配 */
        ComboBox_AddString(pg->hExt, L"全部");
        {
            int i;
            for (i = 0; g_extPresets[i]; i++)
                ComboBox_AddString(pg->hExt, g_extPresets[i]);
        }
        ComboBox_SetCurSel(pg->hExt, 0);
    } else {
        SendMessageW(pg->hCommon, BM_SETCHECK, BST_CHECKED, 0);
        {
            int i;
            for (i = 0; g_custPresets[i]; i++)
                ComboBox_AddString(pg->hCust, g_custPresets[i]);
        }
        SendMessageW(pg->hChkUtf8, BM_SETCHECK, BST_CHECKED, 0);   /* 默认 UTF-8 + GBK/GB18030 */
        SendMessageW(pg->hChkGbk,  BM_SETCHECK, BST_CHECKED, 0);
    }
    {
        static const int sizes[] = { 30, 50, 100, 200, 500, 1000 };
        wchar_t t[16];
        int i;
        for (i = 0; i < 6; i++) {
            swprintf(t, 16, L"%d", sizes[i]);
            ComboBox_AddString(pg->hPageSize, t);
        }
        ComboBox_SetCurSel(pg->hPageSize, 2);    /* 默认 100（30/50/100/200/500/1000 中的索引 2） */
    }
}

static void add_list_columns(HWND hList, int feature)
{
    LVCOLUMNW col;
    struct { const wchar_t *name; int w; int fmt; } cols1[] = {
        { L"序号",     62,  LVCFMT_RIGHT },
        { L"文件名",   180, LVCFMT_LEFT  },
        { L"扩展名",   72,  LVCFMT_LEFT  },
        { L"大小",     92,  LVCFMT_RIGHT },
        { L"修改时间", 140, LVCFMT_LEFT  },
        { L"所在目录", 420, LVCFMT_LEFT  },
    };
    struct { const wchar_t *name; int w; int fmt; } cols2[] = {
        { L"序号",         62,  LVCFMT_RIGHT },
        { L"文件绝对路径", 360, LVCFMT_LEFT  },
        { L"行号",         66,  LVCFMT_RIGHT },
        { L"匹配行内容",   560, LVCFMT_LEFT  },
    };
    int n = feature == 0 ? 6 : 4;
    int i;
    memset(&col, 0, sizeof(col));
    for (i = 0; i < n; i++) {
        const wchar_t *name = feature == 0 ? cols1[i].name : cols2[i].name;
        int w96           = feature == 0 ? cols1[i].w   : cols2[i].w;
        int fmt           = feature == 0 ? cols1[i].fmt : cols2[i].fmt;
        col.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT;
        col.fmt  = fmt;
        col.cx   = S_(w96);
        col.pszText = (LPWSTR)name;
        ListView_InsertColumn(hList, i, &col);
    }
}

void ui_create_page(PageUI *pg, HWND mainWnd, int feature, int baseId)
{
    HWND pan = mkwnd(L"QxPageWnd", NULL,
                     WS_CLIPCHILDREN | (feature == 0 ? WS_VISIBLE : 0), 0,
                     mainWnd, baseId + 500);
    memset(pg, 0, sizeof(*pg));
    pg->feature = feature;
    pg->panel   = pan;
    SetWindowLongPtrW(pan, GWLP_USERDATA, (LONG_PTR)pg);

    pg->hDirLbl    = MKLBL(IDW_DIR,     L"查找目录:");
    pg->hDir       = MKEDIT(IDW_DIR);
    pg->hBrowse    = MKBTN(IDW_BROWSE,  L"浏览…");

    pg->hNeedleLbl = MKLBL(IDW_NEEDLE,  feature == 0 ? L"文件名:" : L"查找内容:");
    pg->hNeedle    = MKEDIT(IDW_NEEDLE);
    pg->hCase      = MKCHK(IDW_CASE,    L"区分大小写");

    if (feature == 0) {
        pg->hMatchLbl = MKLBL(14, L"匹配模式:");
        pg->hMatch    = mkwnd(WC_COMBOBOXW, L"",
                              WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
                              WS_EX_CLIENTEDGE, pan, baseId + IDW_MATCH);
        pg->hExtLbl   = MKLBL(IDW_EXT, L"后缀:");
        pg->hExt      = mkwnd(WC_COMBOBOXW, L"",
                              WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL,
                              WS_EX_CLIENTEDGE, pan, baseId + IDW_EXT);
        pg->hExtHint  = MKLBL(IDW_EXT, L"下拉可多选，也可手工输入（如 txt;md）");
    } else {
        pg->hCommon   = MKCHK(IDW_COMMON, L"常用文本格式");
        pg->hCustLbl  = MKLBL(IDW_CUSTEXT, L"自定义后缀:");
        pg->hCust     = mkwnd(WC_COMBOBOXW, L"",
                              WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL,
                              WS_EX_CLIENTEDGE, pan, baseId + IDW_CUSTEXT);
        pg->hCharsetLbl = MKLBL(IDW_CHARSET, L"字符集:");
        pg->hChkUtf8  = MKCHK(IDW_CHARSET,  L"UTF-8");
        pg->hChkGbk   = MKCHK(IDW_CHKGBK,   L"GBK/GB18030");
        pg->hChkBig5  = MKCHK(IDW_CHKBIG5,  L"Big5");
        pg->hChkU16   = MKCHK(IDW_CHKU16,   L"UTF-16LE");
        pg->hCharsetHint   = MKLBL(IDW_CHARSET, L"无BOM按勾选逐一尝试，命中即止");
    }

    pg->hSearch    = mkwnd(L"BUTTON", L"开始查找",
                           WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, pan, baseId + IDW_SEARCH);
    pg->hStop      = MKBTN(IDW_STOP, L"停止");
    pg->hReset     = MKBTN(IDW_RESET, L"清空");
    EnableWindow(pg->hStop, FALSE);

    pg->hFilterLbl = MKLBL(IDW_FILTERE, L"过滤:");
    pg->hFilter    = MKEDIT(IDW_FILTERE);
    pg->hFilterBtn = MKBTN(IDW_FILTER,  L"过滤");
    pg->hClearBtn  = MKBTN(IDW_CLEAR,   L"清除");
    pg->hTotal     = MKLBL(IDW_TOTAL, L"共 0 条");

    pg->hPpLbl     = MKLBL(IDW_PAGESIZE, L"每页:");
    pg->hPageSize  = mkwnd(WC_COMBOBOXW, L"",
                           WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL,
                           WS_EX_CLIENTEDGE, pan, baseId + IDW_PAGESIZE);
    pg->hFirst     = MKBTN(IDW_FIRST, L"首页");
    pg->hPrev      = MKBTN(IDW_PREV,  L"上一页");
    pg->hNext      = MKBTN(IDW_NEXT,  L"下一页");
    pg->hLast      = MKBTN(IDW_LAST,  L"末页");
    pg->hPageInfo  = MKLBL(IDW_PAGEINFO, L"第 0 / 0 页");

    pg->hList = mkwnd(WC_LISTVIEWW, NULL,
                      WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS,
                      WS_EX_CLIENTEDGE, pan, baseId + IDW_LIST);
    ListView_SetExtendedListViewStyleEx(pg->hList, 0,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);

    add_list_columns(pg->hList, feature);
    set_combos(pg);

    /* 占位提示（cue banner）：获得焦点自动消失，为空时显示 */
    SendMessageW(pg->hDir,    EM_SETCUEBANNER, FALSE,
                 (LPARAM)(feature == 0 ? L"必填：输入或点“浏览…”选择要查找的目录（递归子目录）"
                                       : L"必填：输入或点“浏览…”选择要查找的目录（递归子目录）"));
    SendMessageW(pg->hNeedle, EM_SETCUEBANNER, FALSE,
                 (LPARAM)(feature == 0 ? L"必填：输入要查找的文件名（如 report）"
                                       : L"必填：输入要在文本中查找的字符串"));
    SendMessageW(pg->hFilter, EM_SETCUEBANNER, FALSE,
                 (LPARAM)L"输入关键字过滤结果（忽略大小写），回车应用");
    if (feature == 1)
        SendMessageW(pg->hCust, EM_SETCUEBANNER, FALSE,
                     (LPARAM)L"从下拉选择或输入文本后缀，可多选（如 log;tmp）");

    /* 控件悬浮说明 */
    if (feature == 0) {
        add_tip(pan, pg->hExt,
            L"可同时选择多种后缀：打开下拉后逐个点击，选中的格式以 [√] 标记并累积显示在输入框（如 txt;md），再点同一项即移除；\n"
            L"也可手工输入，用分号/逗号/空格分隔，如  txt;md;cpp\n"
            L"选择“全部”（或留空）表示不过滤后缀。");
        add_tip(pan, pg->hMatch,
            L"全字匹配：文件基本名（不含扩展名）或完整文件名与输入完全相同才算命中；\n"
            L"包含匹配：文件名中包含输入串即命中。");
    } else {
        add_tip(pan, pg->hCommon,
            L"勾选后扫描约 90 种常用文本/源码格式（txt、log、c、cpp、java、py、html、json、md、sql、bat 等，\n"
            L"并包含 Makefile 等无扩展名文本文件），与“自定义后缀”取并集；\n"
            L"全部留空则扫描所有文件。");
        add_tip(pan, pg->hCust,
            L"可同时选择多种文本后缀：打开下拉后逐个点击，选中项以 [√] 标记并累积在输入框（再点移除），也可手工输入（分号/逗号/空格分隔）。\n"
            L"注意：只应输入文本格式后缀；若输入 exe/jpg 等非文本后缀，开始查找时将会提示。");
        add_tip(pan, pg->hCharsetLbl,
            L"内容匹配使用的字符集（可多选）：\n"
            L"无 BOM 的文件按勾选的字符集依次尝试（UTF-8 → GBK/GB18030 → Big5），任一命中即算命中；\n"
            L"GBK/GB18030 向下兼容 GB2312；Big5 用于繁体中文；UTF-16LE 勾选后无 BOM 文件按 UTF-16 小端整文件处理；\n"
            L"带 BOM 的文件始终按 BOM 解码，不受勾选影响；UTF-16BE 暂不支持（自动跳过并计数）。\n"
            L"默认勾选 UTF-8 + GBK/GB18030，可同时命中 UTF-8 的 .md 与 GBK 的 .sql 等混合编码文件。");
        add_tip(pan, pg->hNeedle,
            L"必填。整串作为子串精确匹配（不支持通配符/正则），一行内多次出现只计 1 条。");
    }
    add_tip(pan, pg->hCase,   L"勾选后按大小写精确匹配；不勾选则忽略大小写（中文不受影响）。");
    add_tip(pan, pg->hDir,    L"必填。递归查找该目录及其全部子目录；无法访问的目录自动跳过并计数。");
    add_tip(pan, pg->hFilter, L"对已得到的查找结果二次过滤：功能一匹配完整路径，功能二匹配路径或行内容；\n回车或点“过滤”应用，“清除”恢复全部。");
    add_tip(pan, pg->hPageSize, L"每页显示条数，范围 30~1000（超出自动钳制）。");
    add_tip(pan, pg->hSearch, L"开始查找（快捷键：回车 / F5）。");
    add_tip(pan, pg->hStop,   L"中断当前查找，已找到的部分结果保留。");
    add_tip(pan, pg->hReset,  L"恢复本页全部默认选项：清空目录/内容/过滤，后缀=全部（页2：勾选常用格式、自定义后缀清空、字符集=自动检测），\n每页=100，并清除查找结果。");

    {   /* 统一设置字体 */
        HWND hs[] = {
            pg->hDirLbl, pg->hDir, pg->hBrowse, pg->hNeedleLbl, pg->hNeedle, pg->hCase,
            pg->hMatchLbl, pg->hMatch, pg->hExtLbl, pg->hExt, pg->hExtHint, pg->hCommon,
            pg->hCustLbl, pg->hCust, pg->hCharsetLbl, pg->hChkUtf8, pg->hChkGbk,
            pg->hChkBig5, pg->hChkU16, pg->hCharsetHint,
            pg->hSearch, pg->hStop, pg->hReset, pg->hFilterLbl, pg->hFilter, pg->hFilterBtn,
            pg->hClearBtn, pg->hTotal, pg->hPpLbl, pg->hPageSize, pg->hFirst, pg->hPrev,
            pg->hNext, pg->hLast, pg->hPageInfo, pg->hList
        };
        size_t i;
        for (i = 0; i < sizeof(hs) / sizeof(hs[0]); i++)
            if (hs[i]) SendMessageW(hs[i], WM_SETFONT, (WPARAM)g_hFont, TRUE);
    }
}

/* ---------- 布局（96dpi 基准数值，S_() 缩放） ---------- */
static void mov(HWND h, int x, int y, int w, int hh)
{
    if (h) MoveWindow(h, x, y, w, hh, TRUE);
}

void ui_layout_page(PageUI *pg, int cx, int cy)
{
    int f  = pg->feature;
    int m  = S_(10), g = S_(8);
    int eh = S_(26), bh = S_(28);
    int x, y = m;

    /* 行1：目录(≤50%) + 浏览按钮紧跟其后 ……（右侧）开始查找 / 停止 */
    mov(pg->hDirLbl, m, y + S_(5), S_(66), eh - S_(4));
    {
        int bw = S_(92), sw = S_(112), tw = S_(80);
        int btnL = cx - m - tw - g - tw - g - sw;         /* 开始查找左边界（含清空） */
        int x1 = m + S_(72);
        int dirW = btnL - g - bw - g - x1;               /* 剩余空间 */
        if (btnL < x1 + dirW) btnL = x1 + dirW;          /* 极窄窗口保护 */
        if (dirW > cx / 2) dirW = cx / 2;                /* 目录输入框 ≤ 50% 行宽 */
        if (dirW < S_(100)) dirW = S_(100);
        mov(pg->hDir, x1, y, dirW, eh);
        mov(pg->hBrowse, x1 + dirW + g, y, bw, bh);
        mov(pg->hSearch, btnL, y, sw, bh);
        mov(pg->hStop, cx - m - tw - g - tw, y, tw, bh);
        mov(pg->hReset, cx - m - tw, y, tw, bh);
    }
    y += bh + g;

    /* 行2：查找条件（后缀组合框 ≤ 35% 行宽） */
    {
        int lbl2 = f == 0 ? S_(62) : S_(78);
        int nw   = f == 0 ? S_(230) : S_(250);
        int cw   = S_(104);
        mov(pg->hNeedleLbl, m, y + S_(5), lbl2, eh - S_(4));
        x = m + lbl2 + S_(6);
        mov(pg->hNeedle, x, y, nw, eh);
        x += nw + g;
        mov(pg->hCase, x, y + S_(2), cw, S_(22));
        x += cw + g;
        if (f == 0) {
            int mw = S_(104);
            int extW = cx - m - x;
            if (extW > cx * 35 / 100) extW = cx * 35 / 100;
            if (extW < S_(110)) extW = S_(110);
            mov(pg->hMatchLbl, x, y + S_(5), S_(70), eh - S_(4));
            x += S_(70) + S_(4);
            mov(pg->hMatch, x, y, mw, eh + S_(100));
            x += mw + g;
            mov(pg->hExtLbl, x, y + S_(5), S_(46), eh - S_(4));
            x += S_(46) + S_(4);
            mov(pg->hExt, x, y, extW, eh + S_(100));
        } else {
            int kw = S_(132);
            int cstW = cx - m - x;
            if (cstW > cx * 35 / 100) cstW = cx * 35 / 100;
            if (cstW < S_(110)) cstW = S_(110);
            mov(pg->hCommon, x, y + S_(2), kw, S_(22));
            x += kw + g;
            mov(pg->hCustLbl, x, y + S_(5), S_(90), eh - S_(4));
            x += S_(90) + S_(4);
            mov(pg->hCust, x, y, cstW, eh + S_(100));
        }
    }
    y += bh + g;

    /* 行3：页1=后缀用法提示；页2=字符集 */
    if (f == 0) {
        mov(pg->hExtHint, m + S_(8), y + S_(2), cx - 2 * m - S_(8), S_(18));
        y += S_(22);
    } else {
        int x2 = m + S_(72);
        mov(pg->hCharsetLbl, m, y + S_(3), S_(66), eh - S_(4));
        mov(pg->hChkUtf8, x2, y + S_(2), S_(62), S_(22));
        x2 += S_(62) + S_(2);
        mov(pg->hChkGbk, x2, y + S_(2), S_(110), S_(22));
        x2 += S_(110) + S_(2);
        mov(pg->hChkBig5, x2, y + S_(2), S_(58), S_(22));
        x2 += S_(58) + S_(2);
        mov(pg->hChkU16, x2, y + S_(2), S_(92), S_(22));
        x2 += S_(92) + S_(4);
        mov(pg->hCharsetHint, x2, y + S_(4), cx - m - x2 > S_(60) ? cx - m - x2 : S_(60), S_(18));
        y += S_(24);
    }

    /* 行4：过滤 +（右侧）总数 */
    mov(pg->hFilterLbl, m, y + S_(5), S_(46), eh - S_(4));
    x = m + S_(46) + S_(4);
    {
        int fw = S_(320), minw = S_(120);
        int avail = cx - m - x - (S_(74) + g) * 2;
        if (fw > avail) fw = avail;
        if (fw < minw) fw = minw;
        mov(pg->hFilter, x, y, fw, eh);
        x += fw + g;
        mov(pg->hFilterBtn, x, y, S_(74), bh);
        x += S_(74) + g;
        mov(pg->hClearBtn, x, y, S_(74), bh);
        if (pg->hTotal) mov(pg->hTotal, cx - m - S_(300), y + S_(5), S_(300), eh - S_(4));
    }
    y += bh + g;

    /* 行5：分页 */
    {
        int py = y + S_(2);
        int nb = S_(64), ng = S_(6);
        mov(pg->hPpLbl, m, py + S_(5), S_(46), eh - S_(4));
        x = m + S_(46) + S_(4);
        mov(pg->hPageSize, x, py, S_(76), eh + S_(100));
        x += S_(76) + g;
        mov(pg->hPageInfo, x, py + S_(5), S_(110), eh - S_(4));
        x = cx - m;
        x -= nb;       mov(pg->hLast,  x, py, nb, bh);
        x -= nb + ng;  mov(pg->hNext,  x, py, nb, bh);
        x -= nb + ng;  mov(pg->hPrev,  x, py, nb, bh);
        x -= nb + ng;  mov(pg->hFirst, x, py, nb, bh);
        y = py + bh + g;
    }

    /* 结果列表占满剩余 */
    mov(pg->hList, m, y, cx - 2 * m, cy - y - m);
}

/* ---------- 目录选择 ---------- */
/* exe 同目录 ini 路径（记忆上次目录） */
static void app_ini_path(wchar_t *p, size_t cch)
{
    wchar_t *sl;
    GetModuleFileNameW(NULL, p, (DWORD)cch);
    sl = wcsrchr(p, L'\\');
    if (sl) sl[1] = 0;
    wcscat(p, L"FileSearchTool.ini");
}

/* 启动时回填上次使用的目录（无效则留空） */
void ui_prefill_lastdir(wchar_t *buf, size_t cch)
{
    wchar_t ini[MAX_PATH], last[MAX_PATH];
    app_ini_path(ini, MAX_PATH);
    last[0] = 0;
    GetPrivateProfileStringW(L"UI", L"LastDir", L"", last, MAX_PATH, ini);
    if (*last && GetFileAttributesW(last) != INVALID_FILE_ATTRIBUTES) {
        wcsncpy(buf, last, cch - 1);
        buf[cch - 1] = 0;
    } else {
        buf[0] = 0;
    }
}

/* 本地 GUID 常量（避免静态链入 libuuid 带来的体积膨胀） */
static const CLSID k_CLSID_FileOpenDialog =
    { 0xdc1c5a9c, 0xe88a, 0x4dde, { 0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7 } };
static const IID k_IID_IFileOpenDialog =
    { 0xd57c7288, 0xd4ad, 0x4768, { 0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60 } };
static const IID k_IID_IShellItem =
    { 0x43826d1e, 0xe718, 0x42ee, { 0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe } };

/* 现代文件夹选择器（Vista+ IFileOpenDialog）：
   地址栏可复制/粘贴路径，打开时定位到上次选择目录，确定后写入 ini 记忆 */
int ui_browse_folder(HWND owner, wchar_t *buf, size_t cch)
{
    IFileOpenDialog *dlg = NULL;
    IShellItem *si = NULL, *res = NULL;
    PWSTR path = NULL;
    wchar_t ini[MAX_PATH], last[MAX_PATH];
    DWORD opts = 0;
    int ok = 0;

    buf[0] = 0;
    if (FAILED(CoCreateInstance(&k_CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                &k_IID_IFileOpenDialog, (void **)&dlg)) || !dlg)
        return 0;

    dlg->lpVtbl->GetOptions(dlg, &opts);
    dlg->lpVtbl->SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    dlg->lpVtbl->SetTitle(dlg, L"请选择要查找的目录（地址栏可复制路径）");

    app_ini_path(ini, MAX_PATH);
    last[0] = 0;
    GetPrivateProfileStringW(L"UI", L"LastDir", L"", last, MAX_PATH, ini);
    if (*last && GetFileAttributesW(last) != INVALID_FILE_ATTRIBUTES &&
        SUCCEEDED(SHCreateItemFromParsingName(last, NULL, &k_IID_IShellItem, (void **)&si)) && si) {
        dlg->lpVtbl->SetFolder(dlg, si);
        si->lpVtbl->Release(si);
    }

    if (SUCCEEDED(dlg->lpVtbl->Show(dlg, owner)) &&
        SUCCEEDED(dlg->lpVtbl->GetResult(dlg, &res)) && res) {
        if (SUCCEEDED(res->lpVtbl->GetDisplayName(res, SIGDN_FILESYSPATH, &path)) && path) {
            wcsncpy(buf, path, cch - 1);
            buf[cch - 1] = 0;
            ok = buf[0] != 0;
            CoTaskMemFree(path);
            if (ok)
                WritePrivateProfileStringW(L"UI", L"LastDir", buf, ini);
        }
        res->lpVtbl->Release(res);
    }
    dlg->lpVtbl->Release(dlg);
    return ok;
}
