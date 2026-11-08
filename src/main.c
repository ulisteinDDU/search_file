/* main.c - 主窗口、TAB 切换、命令分发、过滤/分页/展示、消息循环 */
#include "common.h"
#include "util.h"
#include "results.h"
#include "search.h"
#include "ui.h"
#include <windowsx.h>
#include <shellapi.h>

typedef struct {
    int       cur;                       /* 当前页 0/1 */
    PageUI    pages[2];
    void     *results[2];                /* NameResults* / TextResults* */
    size_t   *view[2];                   /* 过滤后的下标视图 */
    size_t    viewCount[2];
    wchar_t  *filter[2];                 /* 已应用过滤（小写），NULL=无 */
    size_t    filterLen[2];
    int       pageCur[2];                /* 1 起 */
    int       pageSize[2];
    size_t    shown[2];                  /* 已渲染到视图的结果总数（增量显示用） */
    SearchJob *job;                      /* 当前活动任务 */
} App;

static App       g_app;
static HINSTANCE g_hInst;
static HWND      g_hTab, g_hStatus, g_hwndMain;
static HACCEL    g_hAccel;

/* ---------------- 小工具 ---------------- */
static void trim_ws(wchar_t *s)
{
    size_t n = wcslen(s), a = 0, b = n;
    while (a < b && iswspace((wint_t)s[a])) a++;
    while (b > a && iswspace((wint_t)s[b - 1])) b--;
    if (a || b != n) {
        memmove(s, s + a, (b - a) * sizeof(wchar_t));
        s[b - a] = 0;
    }
}

static void status_set(int part, const wchar_t *t)
{
    SendMessageW(g_hStatus, SB_SETTEXT, (WPARAM)part, (LPARAM)t);
}

static size_t total_of(int pi)
{
    size_t n;
    if (!g_app.results[pi]) return 0;
    if (pi == 0) {
        NameResults *R = (NameResults *)g_app.results[pi];
        name_results_lock(R);
        n = R->n;
        name_results_unlock(R);
    } else {
        TextResults *R = (TextResults *)g_app.results[pi];
        text_results_lock(R);
        n = R->n;
        text_results_unlock(R);
    }
    return n;
}

static void copy_to_clipboard(const wchar_t *s)
{
    size_t bytes;
    HGLOBAL gm;
    if (!s || !*s) return;
    bytes = (wcslen(s) + 1) * sizeof(wchar_t);
    gm = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!gm) return;
    memcpy(GlobalLock(gm), s, bytes);
    GlobalUnlock(gm);
    if (OpenClipboard(g_hwndMain)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, gm);
        CloseClipboard();
    } else {
        GlobalFree(gm);
    }
}

static void reveal_file(const wchar_t *path)
{
    wchar_t *args = (wchar_t *)xmalloc((wcslen(path) + 32) * sizeof(wchar_t));
    swprintf(args, wcslen(path) + 32, L"/select,\"%ls\"", path);
    ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
    free(args);
}

/* ---------------- 过滤 / 分页 / 展示 ---------------- */
static int read_page_size(PageUI *pg);       /* 前置：刷新前同步“每页”输入 */

static int page_count(int pi)
{
    size_t c = g_app.viewCount[pi];
    int ps = g_app.pageSize[pi];
    int pc = (int)((c + (size_t)ps - 1) / (size_t)ps);
    return pc < 1 ? 1 : pc;
}

static void refresh_view_ui(int pi)
{
    PageUI *pg = &g_app.pages[pi];
    size_t c = g_app.viewCount[pi];
    int ps, pc;
    size_t start, rows;
    wchar_t b1[40], b2[40], t[128];

    /* 每次刷新都从下拉框同步“每页”值（手工输入/程序设置也能立即生效） */
    g_app.pageSize[pi] = read_page_size(pg);
    ps = g_app.pageSize[pi];
    pc = page_count(pi);

    if (g_app.pageCur[pi] > pc) g_app.pageCur[pi] = pc;
    if (g_app.pageCur[pi] < 1)  g_app.pageCur[pi] = 1;

    fmt_u64(c, b1, 40);
    if (g_app.filter[pi]) {
        fmt_u64(total_of(pi), b2, 40);
        swprintf(t, 128, L"共 %ls 条（已过滤，全部 %ls 条）", b1, b2);
    } else {
        swprintf(t, 128, L"共 %ls 条", b1);
    }
    SetWindowTextW(pg->hTotal, t);

    if (c) swprintf(t, 128, L"第 %d / %d 页", g_app.pageCur[pi], pc);
    else   wcscpy(t, L"第 0 / 0 页");
    SetWindowTextW(pg->hPageInfo, t);

    start = (size_t)(g_app.pageCur[pi] - 1) * (size_t)ps;
    rows  = start < c ? (c - start) : 0;
    if (rows > (size_t)ps) rows = (size_t)ps;
    ListView_SetItemCountEx(pg->hList, (int)rows,
                            LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    InvalidateRect(pg->hList, NULL, TRUE);
}

/* ---------------- 功能一：按“修改时间”表头排序 ---------------- */
static int          g_mtimeSort = 0;   /* 0=自然序 1=升序 2=降序 */
static NameResults *g_sortR;           /* qsort 比较器上下文 */

static int cmp_view_mtime(const void *a, const void *b)
{
    const NameHit *x = &g_sortR->hits[*(const size_t *)a];
    const NameHit *y = &g_sortR->hits[*(const size_t *)b];
    unsigned long long xt = ((unsigned long long)x->mtime.dwHighDateTime << 32)
                            | x->mtime.dwLowDateTime;
    unsigned long long yt = ((unsigned long long)y->mtime.dwHighDateTime << 32)
                            | y->mtime.dwLowDateTime;
    int r;
    if (xt < yt) r = -1;
    else if (xt > yt) r = 1;
    else r = _wcsicmp(x->path, y->path);          /* 时间相同按路径名决胜 */
    return g_mtimeSort == 2 ? -r : r;
}

static void set_mtime_arrow(HWND hList, int dir)  /* dir: 0=清除 1=升 2=降 */
{
    HWND hd = ListView_GetHeader(hList);
    HDITEMW hi;
    if (!hd) return;
    memset(&hi, 0, sizeof(hi));
    hi.mask = HDI_FORMAT;
    if (!Header_GetItem(hd, 4, &hi)) return;      /* 第 5 列：修改时间 */
    hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
    if (dir == 1) hi.fmt |= HDF_SORTUP;
    if (dir == 2) hi.fmt |= HDF_SORTDOWN;
    Header_SetItem(hd, 4, &hi);
}

static void reset_mtime_sort(void)
{
    if (g_mtimeSort) {
        g_mtimeSort = 0;
        set_mtime_arrow(g_app.pages[0].hList, 0);
    }
}

static void sort_view_by_mtime(void)
{
    if (!g_app.view[0] || g_app.viewCount[0] < 2 || !g_app.results[0]) {
        refresh_view_ui(0);
        return;
    }
    g_sortR = (NameResults *)g_app.results[0];
    qsort(g_app.view[0], g_app.viewCount[0], sizeof(size_t), cmp_view_mtime);
    g_app.pageCur[0] = 1;
    refresh_view_ui(0);
}

static void rebuild_view(int pi)
{
    size_t n = total_of(pi), i, cnt = 0;
    const wchar_t *fl = g_app.filter[pi];
    size_t flLen = g_app.filterLen[pi];
    size_t *v = NULL;

    if (pi == 0) reset_mtime_sort();      /* 结果重建（新搜索/过滤）恢复自然序 */
    free(g_app.view[pi]);
    g_app.view[pi] = NULL;
    g_app.viewCount[pi] = 0;
    if (!n) { refresh_view_ui(pi); return; }

    v = (size_t *)xmalloc(n * sizeof(size_t));
    if (pi == 0) {
        NameResults *R = (NameResults *)g_app.results[pi];
        name_results_lock(R);
        for (i = 0; i < R->n; i++)
            if (!fl || wcs_ifind(R->hits[i].path, wcslen(R->hits[i].path), fl, flLen))
                v[cnt++] = i;
        name_results_unlock(R);
    } else {
        TextResults *R = (TextResults *)g_app.results[pi];
        text_results_lock(R);
        for (i = 0; i < R->n; i++) {
            const wchar_t *p = R->paths[R->hits[i].fileIdx];
            if (!fl ||
                wcs_ifind(p, wcslen(p), fl, flLen) ||
                wcs_ifind(R->hits[i].text, wcslen(R->hits[i].text), fl, flLen))
                v[cnt++] = i;
        }
        text_results_unlock(R);
    }
    g_app.view[pi] = v;
    g_app.viewCount[pi] = cnt;
    g_app.pageCur[pi] = 1;
    refresh_view_ui(pi);
}

/* 查找进行中：结果增量到达后刷新视图（WM_APP_PROGRESS 调用，UI 线程）。
   无过滤时只把新下标追加进 view[]（O(新增量)，页码不动）；有过滤则全量重建。 */
static void live_view_update(int pi)
{
    size_t n = total_of(pi), i, old;
    size_t *v;
    if (!g_app.results[pi] || n == g_app.shown[pi]) return;
    if (g_app.filter[pi]) {
        rebuild_view(pi);
    } else {
        old = g_app.viewCount[pi];
        v = (size_t *)xrealloc(g_app.view[pi], n * sizeof(size_t));
        g_app.view[pi] = v;
        for (i = old; i < n; i++) v[i] = i;
        g_app.viewCount[pi] = n;
        refresh_view_ui(pi);
    }
    g_app.shown[pi] = n;
}

static int read_page_size(PageUI *pg)
{
    wchar_t b[32];
    int v;
    GetWindowTextW(pg->hPageSize, b, 32);
    v = _wtoi(b);
    if (v < PAGE_MIN) v = PAGE_MIN;
    if (v > PAGE_MAX) v = PAGE_MAX;
    return v;
}

/* 应用每页条数（下拉选择 / 失焦 / 回车 共用）：钳制后规范化显示 */
static void apply_page_size(int pi)
{
    PageUI *pg = &g_app.pages[pi];
    wchar_t b[32];
    int ps = read_page_size(pg);
    swprintf(b, 32, L"%d", ps);
    if (wcscmp(b, L"")) SetWindowTextW(pg->hPageSize, b);
    if (ps != g_app.pageSize[pi]) {
        g_app.pageSize[pi] = ps;
        g_app.pageCur[pi] = 1;
        refresh_view_ui(pi);
    }
}

/* 列表当前选中项 -> 原始结果下标；无选中返回 (size_t)-1 */
static size_t selected_entry(int pi)
{
    PageUI *pg = &g_app.pages[pi];
    int i = ListView_GetNextItem(pg->hList, -1, LVNI_SELECTED);
    size_t start;
    if (i < 0 || !g_app.view[pi]) return (size_t)-1;
    start = (size_t)(g_app.pageCur[pi] - 1) * (size_t)g_app.pageSize[pi];
    if ((size_t)i >= g_app.viewCount[pi] - start) {
        /* 越界保护 */
        if (start + (size_t)i >= g_app.viewCount[pi]) return (size_t)-1;
    }
    return g_app.view[pi][start + (size_t)i];
}

/* ---------------- 布局 / TAB ---------------- */
static void layout_all(void)
{
    RECT rc, st;
    int cx, cy, stH, tabH;
    GetClientRect(g_hwndMain, &rc);
    cx = rc.right; cy = rc.bottom;
    if (cx <= 0 || cy <= 0) return;
    SendMessageW(g_hStatus, WM_SIZE, 0, 0);
    GetWindowRect(g_hStatus, &st);
    stH = st.bottom - st.top;
    tabH = S_(34);
    MoveWindow(g_hTab, 0, 0, cx, tabH, TRUE);
    MoveWindow(g_app.pages[g_app.cur].panel, 0, tabH, cx, cy - tabH - stH, TRUE);
    {
        int w1 = cx - S_(560);
        int parts[2];
        if (w1 < cx / 2) w1 = cx / 2;
        parts[0] = w1; parts[1] = -1;
        SendMessageW(g_hStatus, SB_SETPARTS, 2, (LPARAM)parts);
    }
}

static void switch_page(int i)
{
    if (i == g_app.cur || i < 0 || i > 1) return;
    ShowWindow(g_app.pages[g_app.cur].panel, SW_HIDE);
    g_app.cur = i;
    TabCtrl_SetCurSel(g_hTab, i);          /* 同步 TAB 标题选中态（快捷键切换时） */
    ShowWindow(g_app.pages[i].panel, SW_SHOW);
    layout_all();
    SetFocus(g_app.pages[i].hNeedle);
}

/* ---------------- 查找任务控制 ---------------- */
static void set_running(int running)
{
    int i;
    for (i = 0; i < 2; i++) {
        EnableWindow(g_app.pages[i].hSearch, !running);
        EnableWindow(g_app.pages[i].hStop, running);
    }
}

/* ---------------- 后缀多选下拉 / 非文本后缀提示 ---------------- */
static wchar_t g_comboSaved[2][512];      /* 下拉打开时的文本（点击选择前） */
static wchar_t g_comboPending[2][64];     /* SELENDOK 记录、延迟写回时应用 */
static HWND    g_applyCombo[2];           /* CLOSEUP 时的组合框，供延迟写回定位 */

/* item：不带 [√] 前缀的裸后缀；base：下拉打开时的文本 */
static void combo_multi_toggle(HWND hCombo, wchar_t *base, const wchar_t *item)
{
    wchar_t out[512];
    wchar_t **items;
    size_t n, i, w = 0;
    int found = 0;

    if (!wcscmp(item, L"全部")) {                    /* 选“全部”= 恢复不过滤 */
        SetWindowTextW(hCombo, L"全部");
        return;
    }
    if (!wcscmp(base, L"全部") || !wcscmp(base, L"*")) base[0] = 0;
    ext_list_parse(base, &items, &n);
    for (i = 0; i < n; i++)                          /* 已存在则移除（再点取消） */
        if (!wcscmp(items[i], item)) {
            memmove(items + i, items + i + 1, (n - i - 1) * sizeof(wchar_t *));
            n--;
            found = 1;
            break;
        }
    if (!found && n < 24) {
        items = (wchar_t **)xrealloc(items, (n + 1) * sizeof(wchar_t *));
        items[n++] = wcs_dup(item);
    }
    for (i = 0; i < n; i++) {
        if (w) out[w++] = L';';
        wcscpy(out + w, items[i]);
        w += wcslen(items[i]);
    }
    out[w] = 0;
    SetWindowTextW(hCombo, out);
    ext_list_free(items, n);
}

/* 已知非文本后缀（用于功能2提示） */
static int is_non_text_ext(const wchar_t *ext)
{
    static const wchar_t *bin[] = {
        L"exe", L"dll", L"sys", L"bin", L"obj", L"lib", L"o", L"so", L"class", L"jar",
        L"zip", L"rar", L"7z", L"tar", L"gz", L"bz2", L"xz", L"cab", L"iso", L"wim",
        L"msi",
        L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"ico", L"cur", L"ani", L"tif",
        L"tiff", L"webp", L"svgz", L"psd", L"ai",
        L"mp3", L"wma", L"flac", L"aac", L"wav", L"mid",
        L"mp4", L"avi", L"mkv", L"mov", L"wmv", L"flv", L"m4v", L"webm",
        L"pdf", L"doc", L"docx", L"xls", L"xlsx", L"ppt", L"pptx", L"vsd", L"vsdx",
        L"mdb", L"accdb", L"one", L"onetoc2",
        L"ttf", L"otf", L"fon", L"db", L"dat", L"pak", NULL
    };
    int i;
    for (i = 0; bin[i]; i++)
        if (!_wcsicmp(ext, bin[i])) return 1;
    return 0;
}

/* 返回 1=无问题/继续，2=用户取消 */
static int warn_non_text_ext(const wchar_t *spec)
{
    wchar_t **items;
    size_t n, i, w;
    wchar_t bad[256];
    int cnt = 0;
    ext_list_parse(spec, &items, &n);
    bad[0] = 0;
    w = 0;
    for (i = 0; i < n; i++)
        if (is_non_text_ext(items[i]) && w + wcslen(items[i]) + 2 < 256) {
            if (w) { wcscpy(bad + w, L"; "); w += 2; }
            wcscpy(bad + w, items[i]);
            w += wcslen(items[i]);
            cnt++;
        }
    ext_list_free(items, n);
    if (!cnt) return 1;
    {
        wchar_t msg[600];
        swprintf(msg, 600,
                 L"自定义后缀中包含通常不是文本格式的类型：\n\n    %ls\n\n"
                 L"这些文件按文本处理通常无法得到有效匹配结果（乱码或无命中）。\n"
                 L"是否仍按此设置继续查找？",
                 bad);
        return MessageBoxW(g_hwndMain, msg, L"非文本后缀提示",
                           MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK ? 1 : 2;
    }
}

/* 恢复某页全部默认选项并清除结果 */
static void reset_page(int pi)
{
    PageUI *pg = &g_app.pages[pi];
    if (g_app.job) {
        search_cancel(g_app.job);
        search_wait(g_app.job, 10000);
    }
    SetWindowTextW(pg->hDir, L"");
    SetWindowTextW(pg->hNeedle, L"");
    SetWindowTextW(pg->hFilter, L"");
    SendMessageW(pg->hCase, BM_SETCHECK, BST_UNCHECKED, 0);
    if (pi == 0) {
        SendMessageW(pg->hMatch, CB_SETCURSEL, 1, 0);
        SetWindowTextW(pg->hExt, L"全部");
    } else {
        SendMessageW(pg->hCommon, BM_SETCHECK, BST_CHECKED, 0);
        SetWindowTextW(pg->hCust, L"");
        SendMessageW(pg->hChkUtf8, BM_SETCHECK, BST_CHECKED, 0);   /* 默认 UTF-8 + GBK/GB18030 */
        SendMessageW(pg->hChkGbk,  BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(pg->hChkBig5, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(pg->hChkU16,  BM_SETCHECK, BST_UNCHECKED, 0);
    }
    SendMessageW(pg->hPageSize, CB_SETCURSEL, 2, 0);   /* 每页恢复默认 100 */
    g_app.pageSize[pi] = PAGE_DEFAULT;
    if (pi == 0) reset_mtime_sort();
    if (g_app.results[pi]) { results_free(pi + 1, g_app.results[pi]); g_app.results[pi] = NULL; }
    free(g_app.view[pi]); g_app.view[pi] = NULL; g_app.viewCount[pi] = 0;
    free(g_app.filter[pi]); g_app.filter[pi] = NULL; g_app.filterLen[pi] = 0;
    g_app.pageCur[pi] = 1;
    refresh_view_ui(pi);
    status_set(0, L"已恢复默认选项。");
    status_set(1, L"");
    SetFocus(pg->hDir);
}

static void start_search(int pi)
{
    PageUI *pg = &g_app.pages[pi];
    wchar_t dir[1024], needle[1024], ext[512];
    int cs, contains = 0, useCommon = 0, charset = CSF_DEFAULT;
    const wchar_t *extSpec = NULL;
    DWORD oldWait = WAIT_OBJECT_0;
    SearchJob *job;

    if (g_app.job) {                        /* 有任务在跑：先停止并等待 */
        search_cancel(g_app.job);
        oldWait = search_wait(g_app.job, 10000);
    }

    GetWindowTextW(pg->hDir, dir, 1024);       trim_ws(dir);
    GetWindowTextW(pg->hNeedle, needle, 1024); trim_ws(needle);
    if (!*dir) {
        MessageBoxW(g_hwndMain, L"请先选择或输入查找目录。", APP_TITLE, MB_ICONWARNING);
        SetFocus(pg->hDir);
        return;
    }
    if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_hwndMain, L"目录不存在，请检查后重试。", APP_TITLE, MB_ICONWARNING);
        SetFocus(pg->hDir);
        return;
    }
    if (!*needle) {
        MessageBoxW(g_hwndMain,
                    pi == 0 ? L"文件名为必填项，请输入要查找的文件名。"
                            : L"请输入要查找的字符串。",
                    APP_TITLE, MB_ICONWARNING);
        SetFocus(pg->hNeedle);
        return;
    }
    cs = (int)SendMessageW(pg->hCase, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (pi == 0) {
        contains = (int)SendMessageW(pg->hMatch, CB_GETCURSEL, 0, 0) == 1;
        GetWindowTextW(pg->hExt, ext, 512);
        trim_ws(ext);
        if (*ext && wcscmp(ext, L"全部") && wcscmp(ext, L"*"))
            extSpec = ext;
    } else {
        useCommon = (int)SendMessageW(pg->hCommon, BM_GETCHECK, 0, 0) == BST_CHECKED;
        GetWindowTextW(pg->hCust, ext, 512);
        trim_ws(ext);
        extSpec = ext;                       /* 可为空 */
        charset = ((int)SendMessageW(pg->hChkUtf8, BM_GETCHECK, 0, 0) == BST_CHECKED ? CSF_UTF8 : 0)
                | ((int)SendMessageW(pg->hChkGbk,  BM_GETCHECK, 0, 0) == BST_CHECKED ? CSF_GBK18030 : 0)
                | ((int)SendMessageW(pg->hChkBig5, BM_GETCHECK, 0, 0) == BST_CHECKED ? CSF_BIG5 : 0)
                | ((int)SendMessageW(pg->hChkU16,  BM_GETCHECK, 0, 0) == BST_CHECKED ? CSF_UTF16LE : 0);
        if (!charset) {
            MessageBoxW(g_hwndMain, L"请至少勾选一种字符集。", APP_TITLE, MB_ICONWARNING);
            SetFocus(pg->hChkUtf8);
            return;
        }
        if (*ext && warn_non_text_ext(ext) == 2) {   /* 用户取消 */
            SetFocus(pg->hCust);
            return;
        }
    }

    job = search_job_new(pi + 1, g_hwndMain, dir, needle, cs, contains, useCommon,
                         extSpec, charset);

    /* 替换旧结果。旧 worker 若 10 秒仍未退出（如卡死的网络目录），不释放其结果
       （避免与仍在 push 的 worker 竞争），结果随孤儿 job 保留、由其 DONE 或进程退出回收 */
    if (pi == 0) reset_mtime_sort();
    if (g_app.results[pi]) {
        if (oldWait == WAIT_OBJECT_0)
            results_free(pi + 1, g_app.results[pi]);
        g_app.results[pi] = NULL;
    }
    free(g_app.view[pi]); g_app.view[pi] = NULL; g_app.viewCount[pi] = 0;
    free(g_app.filter[pi]); g_app.filter[pi] = NULL; g_app.filterLen[pi] = 0;
    g_app.pageCur[pi] = 1;
    g_app.shown[pi] = 0;
    refresh_view_ui(pi);

    if (!search_start(job)) {
        results_free(pi + 1, job->results);
        job->results = NULL;
        search_job_free(job);
        MessageBoxW(g_hwndMain, L"创建查找线程失败。", APP_TITLE, MB_ICONERROR);
        return;
    }
    g_app.results[pi] = job->results;       /* 搜索期间即接管，命中增量实时可读 */
    g_app.shown[pi] = 0;
    g_app.job = job;
    set_running(1);
    status_set(0, L"正在查找…");
    status_set(1, L"");
}

static void apply_filter(int pi)
{
    PageUI *pg = &g_app.pages[pi];
    wchar_t buf[512];
    free(g_app.filter[pi]);
    g_app.filter[pi] = NULL;
    g_app.filterLen[pi] = 0;
    GetWindowTextW(pg->hFilter, buf, 512);
    trim_ws(buf);
    if (*buf) {
        g_app.filter[pi] = wcs_lower_dup(buf);
        g_app.filterLen[pi] = wcslen(g_app.filter[pi]);
    }
    rebuild_view(pi);
}

/* ---------------- 结果展示（虚拟列表） ---------------- */
static void fill_disp_info(int pi, NMLVDISPINFOW *di)
{
    wchar_t tmp[1024];
    size_t start = (size_t)(g_app.pageCur[pi] - 1) * (size_t)g_app.pageSize[pi];
    size_t vi, ri;
    int row = di->item.iItem, col = di->item.iSubItem;

    if (!(di->item.mask & LVIF_TEXT) || row < 0) return;
    vi = start + (size_t)row;
    if (vi >= g_app.viewCount[pi] || !g_app.view[pi]) {
        di->item.pszText[0] = 0;
        return;
    }
    ri = g_app.view[pi][vi];
    tmp[0] = 0;

    if (pi == 0) {
        NameResults *R = (NameResults *)g_app.results[pi];
        NameHit hCopy;
        const wchar_t *sl, *name;
        if (!R || ri >= R->n) { di->item.pszText[0] = 0; return; }
        name_results_lock(R);               /* 搜索期间 worker 可能 realloc hits */
        hCopy = R->hits[ri];
        name_results_unlock(R);
        sl = wcsrchr(hCopy.path, L'\\');
        name = sl ? sl + 1 : hCopy.path;
        switch (col) {
        case 0: fmt_u64(vi + 1, tmp, 1024); break;
        case 1: lstrcpynW(tmp, name, 1024); break;
        case 2: {
            const wchar_t *b, *e; size_t bl, el;
            split_filename(name, &b, &bl, &e, &el);
            lstrcpynW(tmp, e, 1024);
            break;
        }
        case 3: fmt_size(hCopy.size, tmp, 1024); break;
        case 4: fmt_filetime(&hCopy.mtime, tmp, 1024); break;
        case 5:
            swprintf(tmp, 1024, L"%.*ls",
                     sl ? (int)(sl - hCopy.path) : (int)wcslen(hCopy.path), hCopy.path);
            break;
        default: break;
        }
    } else {
        TextResults *R = (TextResults *)g_app.results[pi];
        const wchar_t *path = NULL, *text = NULL;
        unsigned line = 0;
        if (!R || ri >= R->n) { di->item.pszText[0] = 0; return; }
        text_results_lock(R);
        path = R->paths[R->hits[ri].fileIdx];
        line = R->hits[ri].line;
        text = R->hits[ri].text;
        text_results_unlock(R);
        switch (col) {
        case 0: fmt_u64(vi + 1, tmp, 1024); break;
        case 1: lstrcpynW(tmp, path, 1024); break;
        case 2: fmt_u64(line, tmp, 1024); break;
        case 3: lstrcpynW(tmp, text ? text : L"", 1024); break;
        default: break;
        }
    }
    lstrcpynW(di->item.pszText, tmp, di->item.cchTextMax);
}

/* ---------------- 帮助菜单 ---------------- */
static void show_help(HWND owner, int topic)
{
    static const wchar_t *titles[] = {
        L"功能说明", L"查找与匹配规则", L"字符集说明", L"常用快捷键", L"关于本工具"
    };
    const wchar_t *txt;
    switch (topic) {
    case 0:
        txt =
            L"功能一【按文件名查找】\n"
            L"　选择任意目录并输入文件名（必填），递归查找其下所有子目录；\n"
            L"　可选区分大小写、匹配模式（全字/包含）、后缀过滤（下拉可多选，也可手工输入，含 doc/docx/xls/xlsx/ppt/pptx 等）。\n\n"
            L"功能二【按内容查找（文本）】\n"
            L"　选择任意目录并输入字符串（必填），在常用文本格式（约 90 种，含 Makefile 等无扩展名文件）\n"
            L"　和/或自定义后缀的文本文件中递归查找该字符串；可选区分大小写、选择字符集；\n"
            L"　结果显示序号、文件绝对路径、行号、匹配行内容。\n\n"
            L"通用能力\n"
            L"　· 两个功能通过顶部 TAB 切换；\n"
            L"　· 查找结果可用“过滤”框二次过滤；\n"
            L"　· 分页显示：每页 30~1000 条，支持首页/上一页/下一页/末页；\n"
            L"　· 显示结果总数（过滤时同时显示全部数）；\n"
            L"　· 双击结果在资源管理器中定位；右键可复制路径/行内容；\n"
            L"　· 查找过程后台执行，可随时“停止”并保留部分结果。";
        break;
    case 1:
        txt =
            L"【文件名匹配（功能一）】\n"
            L"　全字匹配：输入串与文件基本名（不含扩展名）或完整文件名完全相同才命中；\n"
            L"　包含匹配：文件名中包含输入串即命中；\n"
            L"　“区分大小写”同时作用于文件名匹配与功能二的内容匹配。\n\n"
            L"【内容匹配（功能二）】\n"
            L"　输入串整串作为子串精确匹配（不支持通配符/正则）；\n"
            L"　按物理行（\\n 分隔）逐行查找，一行内多次出现只计 1 条；\n"
            L"　行内容显示最多 512 字符（超长截断加“…”）；\n"
            L"　单文件 >512MB 自动跳过；结果上限 100 万条。\n\n"
            L"【后缀】\n"
            L"　后缀取文件名最后一个“.”之后的部分，忽略大小写；\n"
            L"　多个后缀用分号/逗号/空格分隔，写入时可带前导点（自动去除）；\n"
            L"　功能一选“全部”表示不过滤；功能二勾选“常用文本格式”时与自定义后缀取并集。";
        break;
    case 2:
        txt =
            L"【功能二内容匹配支持的字符集（可多选勾选）】\n"
            L"　· 带文件头 BOM 的文件始终按 BOM 解码，不受勾选影响：\n"
            L"　　 EF BB BF = UTF-8；FF FE = UTF-16LE。\n"
            L"　· 无 BOM 的文件按勾选的字符集依次尝试（UTF-8 → GBK/GB18030 → Big5），\n"
            L"　　 任一字符集下命中即算命中，同一行只计 1 条。\n"
            L"　· UTF-8：匹配无 BOM 的 UTF-8 文件（如各类 .md、Git 导出文件）。\n"
            L"　· GBK/GB18030：代码页 54936，向下兼容 GBK/GB2312（默认勾选）。\n"
            L"　· Big5：繁体中文（950）。\n"
            L"　· UTF-16LE：勾选后无 BOM 文件按 UTF-16 小端整文件解码。\n\n"
            L"说明：默认勾选 UTF-8 + GBK/GB18030，UTF-8 的 .md 与 GBK 的 .sql 可一次同时命中；\n"
            L"　　UTF-16BE 暂不支持（自动跳过并计数）；\n"
            L"　　全部取消勾选将无法开始查找；\n"
            L"　　大小写折叠仅覆盖 ASCII 字母（中文无大小写，不受影响）。";
        break;
    case 3:
        txt =
            L"【通用】\n"
            L"　Enter（回车）　开始查找（输入框内）/ 应用过滤（过滤框内）\n"
            L"　F5　　　　　　 重新执行当前页查找\n"
            L"　F1　　　　　　 打开帮助-功能说明\n\n"
            L"【TAB 切换】\n"
            L"　Ctrl+Tab　　　 在“按文件名查找”与“按内容查找”间切换\n"
            L"　Ctrl+1 / Ctrl+2　切换到功能一 / 功能二\n\n"
            L"【结果操作】\n"
            L"　双击　　　　　 在资源管理器中定位并选中该文件\n"
            L"　右键　　　　　 打开所在位置 / 复制完整路径 / 复制行内容（功能二）";
        break;
    default:
        txt =
            L"可视化文件查找工具 FileSearchTool v1.2.6\n\n"
            L"纯 C（Win32 API）实现，无第三方依赖；支持 Windows 7 / 10 / 11（x64）。\n"
            L"双功能：按文件名查找 / 按文本内容查找，TAB 切换。\n\n"
            L"兼容提示：Windows 10/11 开箱即用；纯净 Windows 7 需安装\n"
            L"微软 VC++ 2015-2019 运行库（x64）或 KB2999226。";
        break;
    }
    MessageBoxW(owner, txt, titles[topic], MB_OK | MB_ICONINFORMATION);
}

/* ---------------- 主窗口过程 ---------------- */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {

    case WM_CREATE: {
        TCITEMW ti;
        HMENU bar, help;
        g_hwndMain = hwnd;

        /* 帮助菜单 */
        bar  = CreateMenu();
        help = CreatePopupMenu();
        AppendMenuW(help, MF_STRING, IDM_HELP_FUNC,    L"功能说明\tF1");
        AppendMenuW(help, MF_STRING, IDM_HELP_RULES,   L"查找与匹配规则");
        AppendMenuW(help, MF_STRING, IDM_HELP_CHARSET, L"字符集说明");
        AppendMenuW(help, MF_STRING, IDM_HELP_KEYS,    L"常用快捷键");
        AppendMenuW(help, MF_SEPARATOR, 0, NULL);
        AppendMenuW(help, MF_STRING, IDM_HELP_ABOUT,   L"关于本工具");
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"帮助(&H)");
        SetMenu(hwnd, bar);

        g_hTab = CreateWindowExW(0, WC_TABCONTROLW, NULL,
                                 WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | TCS_TABS,
                                 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_TAB, g_hInst, NULL);
        memset(&ti, 0, sizeof(ti));
        ti.mask = TCIF_TEXT;
        ti.pszText = (LPWSTR)L"按文件名查找";
        TabCtrl_InsertItem(g_hTab, 0, &ti);
        ti.pszText = (LPWSTR)L"按内容查找（文本）";
        TabCtrl_InsertItem(g_hTab, 1, &ti);
        SendMessageW(g_hTab, WM_SETFONT, (WPARAM)ui_font(), TRUE);

        ui_create_page(&g_app.pages[0], hwnd, 0, 1000);
        ui_create_page(&g_app.pages[1], hwnd, 1, 2000);
        g_app.cur = 0;
        g_app.pageSize[0] = g_app.pageSize[1] = PAGE_DEFAULT;
        g_app.pageCur[0] = g_app.pageCur[1] = 1;

        g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, NULL,
                                    WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SBARS_SIZEGRIP,
                                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_STATUS, g_hInst, NULL);
        SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)ui_font(), TRUE);
        status_set(0, L"就绪。请选择目录并输入查找条件。");
        status_set(1, L"");

        layout_all();
        {   /* 回填上次使用的目录（两页） */
            wchar_t ld[MAX_PATH];
            ui_prefill_lastdir(ld, MAX_PATH);
            if (*ld) {
                SetWindowTextW(g_app.pages[0].hDir, ld);
                SetWindowTextW(g_app.pages[1].hDir, ld);
            }
        }
        SetFocus(g_app.pages[0].hDir);
        return 0;
    }

    case WM_SIZE:
        if (w == SIZE_MINIMIZED) break;
        layout_all();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)l;
        mmi->ptMinTrackSize.x = S_(980);
        mmi->ptMinTrackSize.y = S_(660);
        return 0;
    }

    case DM_GETDEFID:   /* IsDialogMessage 回车 -> 当前页“开始查找” */
        return MAKELRESULT((DWORD)((g_app.cur + 1) * 1000 + IDW_SEARCH), DC_HASDEFID);

    case WM_NOTIFY: {
        NMHDR *h = (NMHDR *)l;
        if (h->hwndFrom == g_hTab && h->code == TCN_SELCHANGE) {
            switch_page(TabCtrl_GetCurSel(g_hTab));
            return 0;
        }
        if (h->hwndFrom == g_app.pages[0].hList || h->hwndFrom == g_app.pages[1].hList) {
            int pi = h->hwndFrom == g_app.pages[1].hList ? 1 : 0;
            if (h->code == LVN_GETDISPINFO) {
                fill_disp_info(pi, (NMLVDISPINFOW *)l);
                return 0;
            }
            if (h->code == LVN_COLUMNCLICK) {        /* 功能一“修改时间”表头排序 */
                NMLISTVIEW *pn = (NMLISTVIEW *)l;
                if (pi == 0 && pn->iSubItem == 4 && g_app.viewCount[0] >= 2) {
                    g_mtimeSort = (g_mtimeSort == 1) ? 2 : 1;
                    set_mtime_arrow(g_app.pages[0].hList, g_mtimeSort);
                    sort_view_by_mtime();
                }
                return 0;
            }
            if (h->code == NM_DBLCLK) {
                size_t ri = selected_entry(pi);
                if (ri != (size_t)-1 && g_app.results[pi]) {
                    const wchar_t *p = NULL;
                    if (pi == 0) {
                        NameResults *R = (NameResults *)g_app.results[pi];
                        name_results_lock(R);
                        if (ri < R->n) p = R->hits[ri].path;
                        name_results_unlock(R);
                    } else {
                        TextResults *R = (TextResults *)g_app.results[pi];
                        text_results_lock(R);
                        if (ri < R->n) p = R->paths[R->hits[ri].fileIdx];
                        text_results_unlock(R);
                    }
                    if (p) reveal_file(p);
                }
                return 0;
            }
        }
        break;
    }

    case WM_CONTEXTMENU: {
        int pi;
        for (pi = 0; pi < 2; pi++) {
            if ((HWND)w == g_app.pages[pi].hList) break;
        }
        if (pi < 2) {
            size_t ri = selected_entry(pi);
            if (ri != (size_t)-1 && g_app.results[pi]) {
                const wchar_t *path = NULL;
                const wchar_t *line = NULL;
                if (pi == 0) {
                    NameResults *R = (NameResults *)g_app.results[pi];
                    name_results_lock(R);
                    if (ri < R->n) path = R->hits[ri].path;
                    name_results_unlock(R);
                } else {
                    TextResults *R = (TextResults *)g_app.results[pi];
                    text_results_lock(R);
                    if (ri < R->n) {
                        path = R->paths[R->hits[ri].fileIdx];
                        line = R->hits[ri].text;
                    }
                    text_results_unlock(R);
                }
                if (path) {
                    HMENU m = CreatePopupMenu();
                    POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
                    int cmd;
                    AppendMenuW(m, MF_STRING, 1, L"打开所在位置");
                    AppendMenuW(m, MF_STRING, 2, L"复制完整路径");
                    if (line) AppendMenuW(m, MF_STRING, 3, L"复制行内容");
                    if (pt.x == -1 && pt.y == -1) {   /* 键盘触发 */
                        RECT rc;
                        GetWindowRect((HWND)w, &rc);
                        pt.x = (rc.left + rc.right) / 2;
                        pt.y = (rc.top + rc.bottom) / 2;
                    }
                    cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                         pt.x, pt.y, 0, hwnd, NULL);
                    DestroyMenu(m);
                    if (cmd == 1)      reveal_file(path);
                    else if (cmd == 2) copy_to_clipboard(path);
                    else if (cmd == 3 && line) copy_to_clipboard(line);
                }
            }
            return 0;
        }
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(w);
        int code = HIWORD(w);
        int pi = id / 1000 - 1;
        int sub;
        if (id == CMD_SWITCH_TAB) { switch_page(1 - g_app.cur); return 0; }
        if (id == CMD_TAB1)       { switch_page(0); return 0; }
        if (id == CMD_TAB2)       { switch_page(1); return 0; }
        if (id == CMD_RESEARCH)   { start_search(g_app.cur); return 0; }
        switch (id) {                                   /* 帮助菜单 */
        case IDM_HELP_FUNC:    show_help(hwnd, 0); return 0;
        case IDM_HELP_RULES:   show_help(hwnd, 1); return 0;
        case IDM_HELP_CHARSET: show_help(hwnd, 2); return 0;
        case IDM_HELP_KEYS:    show_help(hwnd, 3); return 0;
        case IDM_HELP_ABOUT:   show_help(hwnd, 4); return 0;
        default: break;
        }
        if (pi < 0 || pi > 1) break;
        sub = id % 1000;
        switch (sub) {
        case IDW_BROWSE: {
            wchar_t buf[MAX_PATH];
            if (ui_browse_folder(hwnd, buf, MAX_PATH))
                SetWindowTextW(g_app.pages[pi].hDir, buf);
            break;
        }
        case IDW_SEARCH: start_search(pi); break;
        case IDW_STOP:
            if (g_app.job) {
                search_cancel(g_app.job);
                status_set(0, L"正在停止查找…");
            }
            break;
        case IDW_RESET: reset_page(pi); break;
        case IDW_EXT:
        case IDW_CUSTEXT:
            /* 多选状态机（写回一律延迟到 WM_APP_EXTAPPLY，确保在组合框关闭流程
               全部结束、系统对编辑框的任何覆盖完成之后再写入，免疫时序差异）：
               CBN_DROPDOWN     记录当前文本并刷新列表 [√] 标记（重建会清空编辑框，立即恢复）
               CBN_SELENDOK     仅记录所选项（此刻系统稍后会把编辑框覆盖为该项）
               CBN_CLOSEUP      投递延迟消息：有选择=写回累积文本；无选择=恢复原文本 */
            if (code == CBN_DROPDOWN) {
                wchar_t cur[512];
                GetWindowTextW((HWND)l, cur, 512);
                ui_rebuild_ext_list((HWND)l, pi == 0 ? 0 : 1, cur);
                wcscpy(g_comboSaved[pi], cur);
                g_comboPending[pi][0] = 0;
            } else if (code == CBN_SELENDOK) {
                int sel = (int)SendMessageW((HWND)l, CB_GETCURSEL, 0, 0);
                g_comboPending[pi][0] = 0;
                if (sel >= 0 && SendMessageW((HWND)l, CB_GETLBTEXTLEN, sel, 0) < 60) {
                    SendMessageW((HWND)l, CB_GETLBTEXT, sel, (LPARAM)g_comboPending[pi]);
                    if (!wcsncmp(g_comboPending[pi], L"[√] ", 4)) {   /* 去掉标记前缀 */
                        size_t pl = wcslen(g_comboPending[pi]);
                        memmove(g_comboPending[pi], g_comboPending[pi] + 4,
                                (pl - 4 + 1) * sizeof(wchar_t));
                    }
                }
            } else if (code == CBN_SELENDCANCEL) {
                g_comboPending[pi][0] = 0;
            } else if (code == CBN_CLOSEUP) {
                g_applyCombo[pi] = (HWND)l;
                PostMessageW(hwnd, WM_APP_EXTAPPLY,
                             (WPARAM)pi, (LPARAM)(g_comboPending[pi][0] ? 1 : 0));
            }
            break;
        case IDW_FILTER: apply_filter(pi); break;
        case IDW_CLEAR:
            SetWindowTextW(g_app.pages[pi].hFilter, L"");
            apply_filter(pi);
            break;
        case IDW_PAGESIZE:
            if (code == CBN_SELCHANGE || code == CBN_CLOSEUP || code == CBN_KILLFOCUS)
                apply_page_size(pi);
            break;
        case IDW_FIRST: g_app.pageCur[pi] = 1;                refresh_view_ui(pi); break;
        case IDW_PREV:  if (g_app.pageCur[pi] > 1) g_app.pageCur[pi]--; refresh_view_ui(pi); break;
        case IDW_NEXT:  if (g_app.pageCur[pi] < page_count(pi)) g_app.pageCur[pi]++; refresh_view_ui(pi); break;
        case IDW_LAST:  g_app.pageCur[pi] = page_count(pi);   refresh_view_ui(pi); break;
        default: break;
        }
        return 0;
    }

    case WM_APP_EXTAPPLY: {   /* 延迟写回：组合框关闭流程（含系统覆盖编辑框）已全部结束 */
        int api = (int)w;
        HWND hc;
        if (api < 0 || api > 1) break;
        hc = g_applyCombo[api];
        if (!hc || !IsWindow(hc)) break;
        if (l && g_comboPending[api][0]) {
            combo_multi_toggle(hc, g_comboSaved[api], g_comboPending[api]);
            g_comboPending[api][0] = 0;
        } else {
            /* 未选择即关闭（Esc/点击空白）：恢复原文本（重建列表会清空编辑框） */
            SetWindowTextW(hc, g_comboSaved[api]);
        }
        return 0;
    }

    case WM_APP_PROGRESS: {
        ProgressSnap *s = (ProgressSnap *)l;
        if (!s) return 0;
        if (s->job == g_app.job && g_app.job) {       /* 过期任务的残留消息直接丢弃 */
            int pi = g_app.job->feature - 1;
            wchar_t b1[40], b2[40], b3[40], t[192];
            fmt_u64(s->files, b1, 40);
            fmt_u64(s->dirs,  b2, 40);
            fmt_u64(s->hits,  b3, 40);
            swprintf(t, 192, L"正在查找… 已扫描文件 %ls 个、目录 %ls 个、命中 %ls 条", b1, b2, b3);
            status_set(0, t);
            live_view_update(pi);                     /* 命中增量实时显示 */
        }
        free(s);
        return 0;
    }

    case WM_APP_DONE: {
        SearchJob *job = (SearchJob *)l;
        int pi = (int)w - 1;
        int wasCancelled;
        void *res;
        if (pi < 0 || pi > 1) {
            search_wait(job, INFINITE);
            search_job_free(job);
            return 0;
        }
        if (job != g_app.job) {                       /* 过期消息：结果归 g_app 所有，不在此释放 */
            search_wait(job, INFINITE);
            search_job_free(job);
            return 0;
        }
        wasCancelled = InterlockedCompareExchange(&job->cancel, 0, 0) != 0;
        res = job->results;                           /* 仅作统计读取（g_app.results[pi] 已指向它） */
        search_wait(job, INFINITE);
        search_job_free(job);
        g_app.job = NULL;
        set_running(0);

        /* 结果在 start_search 时即接管进 g_app.results[pi]，此处无需再释放/赋值 */
        rebuild_view(pi);
        g_app.shown[pi] = total_of(pi);

        {   /* 完成统计 */
            wchar_t b1[40], b2[40], b3[40], b4[40], t[256];
            int trunc = 0, skipD = 0;
            unsigned long long files, dirs, skippedDirs;
            if (pi == 0) {
                NameResults *R = (NameResults *)res;
                files = R->files; dirs = R->dirs; skippedDirs = R->skippedDirs;
                trunc = R->truncated;
            } else {
                TextResults *R = (TextResults *)res;
                files = R->files; dirs = R->dirs; skippedDirs = R->skippedDirs;
                trunc = R->truncated;
            }
            fmt_u64(files, b1, 40);
            fmt_u64(dirs, b2, 40);
            fmt_u64(total_of(pi), b3, 40);
            fmt_u64(skippedDirs, b4, 40);
            swprintf(t, 256, L"%ls扫描文件 %ls 个、目录 %ls 个，命中结果 %ls 条",
                     wasCancelled ? L"已停止（返回部分结果）：完成时" : L"完成：",
                     b1, b2, b3);
            if (skipD + skippedDirs > 0) {
                wchar_t t2[128];
                swprintf(t2, 128, L"；跳过无法访问目录 %ls 个", b4);
                wcscat(t, t2);
            }
            if (trunc) {
                wchar_t t2[96];
                swprintf(t2, 96, L"；已达结果上限（%d 条），提前停止",
                         pi == 0 ? MAX_NAME_HITS : MAX_TEXT_HITS);
                wcscat(t, t2);
            }
            status_set(0, t);
            status_set(1, wasCancelled ? L"已停止" : L"完成");
        }
        return 0;
    }

    case WM_DESTROY:
        if (g_app.job) {
            search_cancel(g_app.job);
            search_wait(g_app.job, 3000);
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

/* ---------------- 入口 ---------------- */
static int gui_main(void)
{
    WNDCLASSW wc;
    MSG msg;
    INITCOMMONCONTROLSEX icc;
    ACCEL acc[5];
    RECT wa, wr;
    int width, height;

    memset(&icc, 0, sizeof(icc));
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    ui_module_init(g_hInst);
    ui_init(g_hInst);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = g_hInst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon         = LoadIconW(g_hInst, MAKEINTRESOURCEW(1));   /* res/app.ico */
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = APP_CLASS;
    RegisterClassW(&wc);

    acc[0].fVirt = FCONTROL | FVIRTKEY; acc[0].key = VK_TAB; acc[0].cmd = CMD_SWITCH_TAB;
    acc[1].fVirt = FCONTROL | FVIRTKEY; acc[1].key = '1';    acc[1].cmd = CMD_TAB1;
    acc[2].fVirt = FCONTROL | FVIRTKEY; acc[2].key = '2';    acc[2].cmd = CMD_TAB2;
    acc[3].fVirt = FVIRTKEY;            acc[3].key = VK_F5;  acc[3].cmd = CMD_RESEARCH;
    acc[4].fVirt = FVIRTKEY;            acc[4].key = VK_F1;  acc[4].cmd = IDM_HELP_FUNC;
    g_hAccel = CreateAcceleratorTableW(acc, 5);

    width = S_(1100); height = S_(720);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_hwndMain = CreateWindowExW(0, APP_CLASS, APP_TITLE, WS_OVERLAPPEDWINDOW,
                                 wa.left + ((wa.right - wa.left) - width) / 2,
                                 wa.top + ((wa.bottom - wa.top) - height) / 2,
                                 width, height, NULL, NULL, g_hInst, NULL);
    if (!g_hwndMain) return 1;
    GetWindowRect(g_hwndMain, &wr);
    (void)wr;
    ShowWindow(g_hwndMain, SW_SHOW);
    UpdateWindow(g_hwndMain);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        int handled = 0;
        if (TranslateAcceleratorW(g_hwndMain, g_hAccel, &msg)) continue;
        /* 过滤框内回车 = 应用过滤；每页框内回车 = 应用每页条数（而非触发默认按钮） */
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            HWND focus = GetFocus();
            if (focus && (focus == g_app.pages[0].hFilter || focus == g_app.pages[1].hFilter)) {
                int pi = focus == g_app.pages[1].hFilter ? 1 : 0;
                SendMessageW(g_hwndMain, WM_COMMAND,
                             MAKEWPARAM((pi + 1) * 1000 + IDW_FILTER, BN_CLICKED),
                             (LPARAM)g_app.pages[pi].hFilterBtn);
                handled = 1;
            } else if (focus && (focus == g_app.pages[0].hPageSize ||
                                 focus == g_app.pages[1].hPageSize)) {
                int pi = focus == g_app.pages[1].hPageSize ? 1 : 0;
                apply_page_size(pi);
                handled = 1;
            }
        }
        if (!handled && IsDialogMessageW(g_hwndMain, &msg)) handled = 1;
        if (!handled) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmd, int nShow)
{
    (void)hPrev; (void)nShow;
    g_hInst = hInst;
    if (cmd && wcscmp(cmd, L"--selftest") == 0)
        return run_selftest();
    return gui_main();
}
