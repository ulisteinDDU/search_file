/* search.c - 目录遍历 + 匹配引擎 + 工作线程 */
#include "search.h"
#include "util.h"
#include "results.h"

/* ---------- 目录栈（迭代遍历，防深目录爆栈） ---------- */
typedef struct { wchar_t **v; size_t n, cap; } StrStack;

static void stack_init(StrStack *s) { s->v = NULL; s->n = s->cap = 0; }
static void stack_push(StrStack *s, wchar_t *p)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->v = (wchar_t **)xrealloc(s->v, s->cap * sizeof(wchar_t *));
    }
    s->v[s->n++] = p;
}
static void stack_free_all(StrStack *s)
{
    while (s->n) free(s->v[--s->n]);
    free(s->v);
    s->v = NULL; s->cap = 0;
}
static int job_cancelled(SearchJob *j)
{
    return InterlockedCompareExchange(&j->cancel, 0, 0) != 0;
}

/* ---------- 工作上下文 ---------- */
typedef struct {
    SearchJob *job;
    wchar_t  **exts;
    size_t     extN;
    int        includeNoExt;               /* 功能2：常用格式包含无扩展名文件 */
    wchar_t   *needleLower;                /* 大小写不敏感用 */
    char      *ndUtf8; size_t ndUtf8Len;   /* 区分大小写：UTF-8 字节串 */
    char      *ndGb;   size_t ndGbLen;     /* 区分大小写：GBK/GB18030(54936) 字节串 */
    char      *ndBig5; size_t ndBig5Len;   /* 区分大小写：Big5(950) 字节串 */
    wchar_t   *wideScratch;                /* 行转宽字符复用缓冲 */
    size_t     scratchCap;
} Ctx;

static void ctx_free(Ctx *c)
{
    ext_list_free(c->exts, c->extN);
    free(c->needleLower);
    free(c->ndUtf8);
    free(c->ndGb);
    free(c->ndBig5);
    free(c->wideScratch);
}

static void mb_needle(UINT cp, char **out, size_t *outLen, const wchar_t *nd)
{
    int k, k2;
    char *b;
    *out = NULL; *outLen = 0;
    if (!nd || !*nd) return;
    k = WideCharToMultiByte(cp, 0, nd, -1, NULL, 0, NULL, NULL);
    if (k <= 1) return;
    b = (char *)xmalloc((size_t)k);
    k2 = WideCharToMultiByte(cp, 0, nd, -1, b, k, NULL, NULL);
    if (k2 <= 1) { free(b); return; }
    *out = b;
    *outLen = (size_t)(k2 - 1);            /* 去掉结尾 NUL */
}

/* 行字节 -> 宽字符（复用 scratch） */
static wchar_t *to_wide(Ctx *c, UINT cp, const unsigned char *s, size_t n, size_t *outLen)
{
    int wl, w2;
    if (c->scratchCap == 0) {
        c->scratchCap = 256;
        c->wideScratch = (wchar_t *)xmalloc(c->scratchCap * sizeof(wchar_t));
    }
    if (!n) { c->wideScratch[0] = 0; *outLen = 0; return c->wideScratch; }
    wl = MultiByteToWideChar(cp, 0, (const char *)s, (int)n, NULL, 0);
    if (wl <= 0) { c->wideScratch[0] = 0; *outLen = 0; return c->wideScratch; }
    if ((size_t)wl + 1 > c->scratchCap) {
        while (c->scratchCap < (size_t)wl + 1) c->scratchCap *= 2;
        c->wideScratch = (wchar_t *)xrealloc(c->wideScratch, c->scratchCap * sizeof(wchar_t));
    }
    w2 = MultiByteToWideChar(cp, 0, (const char *)s, (int)n, c->wideScratch, wl);
    *outLen = w2 > 0 ? (size_t)w2 : 0;
    c->wideScratch[*outLen] = 0;
    return c->wideScratch;
}

static void post_progress(SearchJob *job, ProgressSnap *src)
{
    ProgressSnap *s;
    if (!job->hwnd) return;
    s = (ProgressSnap *)xmalloc(sizeof(*s));
    *s = *src;
    s->job = job;                            /* 供 UI 丢弃过期任务的消息 */
    if (!PostMessageW(job->hwnd, WM_APP_PROGRESS, (WPARAM)job->feature, (LPARAM)s))
        free(s);
}

/* ---------- 功能1：文件名匹配 ---------- */
static int eq_cs(const wchar_t *a, size_t al, const wchar_t *b)
{
    size_t bl = wcslen(b);
    return al == bl && wcsncmp(a, b, al) == 0;
}
static int eq_ci(const wchar_t *a, size_t al, const wchar_t *bLower)
{
    size_t i, bl = wcslen(bLower);
    if (al != bl) return 0;
    for (i = 0; i < al; i++)
        if ((wchar_t)towlower(a[i]) != bLower[i]) return 0;
    return 1;
}
static int has_cs(const wchar_t *a, size_t al, const wchar_t *b)
{
    size_t bl = wcslen(b);
    size_t i;
    if (!bl) return 1;
    for (i = 0; i + bl <= al; i++)
        if (wcsncmp(a + i, b, bl) == 0) return 1;
    return 0;
}
static int has_ci(const wchar_t *a, size_t al, const wchar_t *bLower)
{
    size_t i, j, bl = wcslen(bLower);
    if (!bl) return 1;
    for (i = 0; i + bl <= al; i++) {
        for (j = 0; j < bl && (wchar_t)towlower(a[i + j]) == bLower[j]; j++) {}
        if (j == bl) return 1;
    }
    return 0;
}

/* 与“基本名”或“完整文件名”比较（全字=相等，包含=子串） */
static int name_file_hit(Ctx *c, const wchar_t *fileName)
{
    const wchar_t *base, *ext, *nd = c->job->needle;
    size_t bl, el, fl = wcslen(fileName);
    /* 后缀过滤（空列表 = 不过滤，由 ext_list_match 保证） */
    if (!ext_list_match(c->exts, c->extN, fileName))
        return 0;
    split_filename(fileName, &base, &bl, &ext, &el);
    if (c->job->caseSensitive) {
        if (c->job->matchContains)
            return has_cs(base, bl, nd) || has_cs(fileName, fl, nd);
        return eq_cs(base, bl, nd) || eq_cs(fileName, fl, nd);
    } else {
        if (c->job->matchContains)
            return has_ci(base, bl, c->needleLower) || has_ci(fileName, fl, c->needleLower);
        return eq_ci(base, bl, c->needleLower) || eq_ci(fileName, fl, c->needleLower);
    }
}

static unsigned __stdcall name_worker(void *arg)
{
    SearchJob *job = (SearchJob *)arg;
    NameResults *R = (NameResults *)job->results;   /* search_job_new 预创建 */
    Ctx c;
    wchar_t *rootD = NULL;
    wchar_t *rootP = make_prefixed_root(job->root, &rootD);

    memset(&c, 0, sizeof(c));
    c.job = job;
    if (rootP) {
        StrStack sp, sd;
        ProgressSnap prog;
        unsigned tick = 0;
        unsigned long long postedHits = (unsigned long long)-1;
        DWORD lastPost = GetTickCount();
        int stop = 0;
        if (job->extSpec && *job->extSpec)
            ext_list_parse(job->extSpec, &c.exts, &c.extN);
        c.needleLower = wcs_lower_dup(job->needle);

        stack_init(&sp);
        stack_init(&sd);
        stack_push(&sp, wcs_dup(rootP));
        stack_push(&sd, wcs_dup(rootD));
        memset(&prog, 0, sizeof(prog));

        while (sp.n && !stop) {
            wchar_t *dirP, *dirD, *pat;
            WIN32_FIND_DATAW fd;
            HANDLE h;
            if (job_cancelled(job)) break;
            dirP = sp.v[--sp.n];
            dirD = sd.v[--sd.n];
            pat = path_join(dirP, L"*");
            h = FindFirstFileW(pat, &fd);
            free(pat);
            if (h == INVALID_HANDLE_VALUE) {
                R->skippedDirs++;
                free(dirP); free(dirD);
                continue;
            }
            do {
                if (fd.cFileName[0] == L'.' &&
                    (fd.cFileName[1] == 0 ||
                     (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
                    continue;
                if ((++tick & 0xFF) == 0 ||
                    (prog.hits != postedHits && GetTickCount() - lastPost >= 100)) {
                    if (job_cancelled(job)) break;
                    post_progress(job, &prog);      /* 命中即通知，UI 增量显示 */
                    postedHits = prog.hits;
                    lastPost = GetTickCount();
                }
                {
                    wchar_t *fullP = path_join(dirP, fd.cFileName);
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                            free(fullP);               /* 跳过符号链接/联接点 */
                        } else {
                            stack_push(&sp, fullP);   /* 栈接管 fullP */
                            stack_push(&sd, path_join(dirD, fd.cFileName));
                            prog.dirs++;
                        }
                    } else {
                        prog.files++;
                        if (name_file_hit(&c, fd.cFileName)) {
                            wchar_t *fullD = path_join(dirD, fd.cFileName);
                            name_push(R, fullD,
                                      ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow,
                                      &fd.ftLastWriteTime);
                            free(fullD);
                            prog.hits = R->n;
                            if (R->n >= MAX_NAME_HITS) { R->truncated = 1; stop = 1; }
                        }
                        free(fullP);
                    }
                }
            } while (!stop && FindNextFileW(h, &fd));
            FindClose(h);
            free(dirP);
            free(dirD);
        }
        stack_free_all(&sp);
        stack_free_all(&sd);
        R->files = prog.files;
        R->dirs = prog.dirs;
    }
    free(rootP);
    free(rootD);
    ctx_free(&c);
    PostMessageW(job->hwnd, WM_APP_DONE, (WPARAM)1, (LPARAM)job);
    return 0;
}

/* ---------- 功能2：文本内容匹配 ---------- */
static int text_file_ext_ok(Ctx *c, const wchar_t *fileName)
{
    const wchar_t *dot = wcsrchr(fileName, L'.');
    if (!dot || dot == fileName) return c->includeNoExt; /* Makefile 等无扩展名文本 */
    return ext_list_match(c->exts, c->extN, fileName);
}

static void scan_text_file(Ctx *c, const wchar_t *dirD, const wchar_t *fileName,
                           const wchar_t *fullP, unsigned long long fsize,
                           TextResults *R, int *stopAll)
{
    HANDLE h;
    unsigned char *buf;
    size_t sz, got = 0;
    const wchar_t *nd = c->job->needle;
    size_t ndLen = wcslen(nd);
    size_t fileIdx = (size_t)-1;

    if (fsize == 0) return;
    if (fsize > MAX_TEXT_FILE_BYTES) { R->skippedLarge++; return; }
    h = CreateFileW(fullP, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { R->skippedFiles++; return; }
    sz = (size_t)fsize;
    buf = (unsigned char *)xmalloc(sz + 2);
    for (;;) {
        DWORD r = 0;
        if (!ReadFile(h, buf + got, (DWORD)(sz - got), &r, NULL) || r == 0) break;
        got += r;
        if (got >= sz) break;
    }
    CloseHandle(h);
    if (!got) { free(buf); return; }

#define LAZY_PATH() do { \
        if (fileIdx == (size_t)-1) { \
            text_push_path(R, path_join(dirD, fileName)); \
            fileIdx = R->nPath - 1; \
        } \
    } while (0)

    /* ---- 编码判定：BOM 优先（带 BOM 恒按 BOM 解码）；无 BOM 文件按勾选字符集逐一尝试 ---- */
    {
        int utf16   = (got >= 2 && buf[0] == 0xFF && buf[1] == 0xFE);
        int be16    = (got >= 2 && buf[0] == 0xFE && buf[1] == 0xFF);
        int utf8bom = (got >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF);
        UINT cpList[3];                        /* 候选字符集（有序：UTF-8 -> GB18030 -> Big5） */
        const char *ndList[3];
        size_t ndLList[3];
        int nCand = 0;

        if (be16) {                                    /* UTF-16BE 暂不支持 */
            R->skippedFiles++;
            free(buf);
            return;
        }
        if (!utf16 && (c->job->charset & CSF_UTF16LE)) /* 无 BOM 也按 UTF-16LE */
            utf16 = 1;

        if (utf8bom) {                                 /* UTF-8 BOM：恒按 UTF-8 */
            cpList[0] = CP_UTF8;
            ndList[0] = c->ndUtf8;   ndLList[0] = c->ndUtf8Len;   nCand = 1;
        } else if (!utf16) {
            if (c->job->charset & CSF_UTF8) {
                cpList[nCand] = CP_UTF8;
                ndList[nCand] = c->ndUtf8;   ndLList[nCand] = c->ndUtf8Len;   nCand++;
            }
            if (c->job->charset & CSF_GBK18030) {
                cpList[nCand] = 54936;
                ndList[nCand] = c->ndGb;     ndLList[nCand] = c->ndGbLen;     nCand++;
            }
            if (c->job->charset & CSF_BIG5) {
                cpList[nCand] = 950;
                ndList[nCand] = c->ndBig5;   ndLList[nCand] = c->ndBig5Len;   nCand++;
            }
        }

        if (utf16) {                                              /* UTF-16LE */
            const wchar_t *w = (const wchar_t *)buf;
            size_t nw = got / 2, ls = 0, i;
            unsigned lineNo = 0;
            for (i = 0; i <= nw; i++) {
                if (i == nw || w[i] == L'\n') {
                    size_t le = i;
                    lineNo++;
                    if (le > ls && w[le - 1] == L'\r') le--;
                    {
                        const wchar_t *m = c->job->caseSensitive
                            ? wcs_find(w + ls, le - ls, nd, ndLen)
                            : wcs_ifind(w + ls, le - ls, c->needleLower, ndLen);
                        if (m) {
                            LAZY_PATH();
                            text_push_hit(R, fileIdx, lineNo, w + ls, le - ls);
                        }
                    }
                    ls = i + 1;
                    if (R->n >= MAX_TEXT_HITS) { R->truncated = 1; *stopAll = 1; break; }
                }
            }
        } else if (nCand > 0) {                      /* 字节型：按候选逐一尝试，命中即止 */
            const unsigned char *s = buf;
            size_t len = got, ls = 0, i;
            unsigned lineNo = 0;
            if (utf8bom) { s += 3; len -= 3; }
            for (i = 0; i <= len; i++) {
            if (i == len || s[i] == '\n') {
                size_t le = i;
                int hit = 0, k;
                size_t wl = 0;
                wchar_t *wb = NULL;
                lineNo++;
                if (le > ls && s[le - 1] == '\r') le--;
                for (k = 0; k < nCand && !hit; k++) {
                    if (c->job->caseSensitive) {
                        if (ndLList[k] && le - ls >= ndLList[k] &&
                            mem_find(s + ls, le - ls,
                                     (const unsigned char *)ndList[k], ndLList[k])) {
                            hit = 1;                 /* 命中的候选解码用于行内容显示 */
                            wb = to_wide(c, cpList[k], s + ls, le - ls, &wl);
                        }
                    } else {
                        wb = to_wide(c, cpList[k], s + ls, le - ls, &wl);
                        if (wcs_ifind(wb, wl, c->needleLower, ndLen))
                            hit = 1;
                    }
                }
                if (hit) {
                    LAZY_PATH();
                    text_push_hit(R, fileIdx, lineNo, wb ? wb : L"", wl);
                }
                ls = i + 1;
                if (R->n >= MAX_TEXT_HITS) { R->truncated = 1; *stopAll = 1; break; }
            }
            }
        }
    }   /* 编码判定块结束 */
#undef LAZY_PATH
    free(buf);
}

static unsigned __stdcall text_worker(void *arg)
{
    SearchJob *job = (SearchJob *)arg;
    TextResults *R = (TextResults *)job->results;   /* search_job_new 预创建 */
    Ctx c;
    wchar_t *rootD = NULL;
    wchar_t *rootP = make_prefixed_root(job->root, &rootD);

    memset(&c, 0, sizeof(c));
    c.job = job;
    if (rootP) {
        StrStack sp, sd;
        ProgressSnap prog;
        unsigned tick = 0;
        unsigned long long postedHits = (unsigned long long)-1;
        DWORD lastPost = GetTickCount();
        int stop = 0;

        /* 合并常用文本格式与自定义后缀（并集） */
        if (job->useCommonText && job->extSpec && *job->extSpec) {
            const wchar_t *common = common_text_ext_spec();
            size_t n = wcslen(common) + 1 + wcslen(job->extSpec);
            wchar_t *all = (wchar_t *)xmalloc((n + 1) * sizeof(wchar_t));
            swprintf(all, n + 1, L"%ls;%ls", common, job->extSpec);
            ext_list_parse(all, &c.exts, &c.extN);
            free(all);
        } else if (job->useCommonText) {
            ext_list_parse(common_text_ext_spec(), &c.exts, &c.extN);
        } else if (job->extSpec && *job->extSpec) {
            ext_list_parse(job->extSpec, &c.exts, &c.extN);
        }
        c.includeNoExt = job->useCommonText;
        c.needleLower = wcs_lower_dup(job->needle);
        mb_needle(CP_UTF8, &c.ndUtf8,  &c.ndUtf8Len, job->needle);
        mb_needle(54936,   &c.ndGb,    &c.ndGbLen,   job->needle);
        mb_needle(950,     &c.ndBig5,  &c.ndBig5Len, job->needle);

        stack_init(&sp);
        stack_init(&sd);
        stack_push(&sp, wcs_dup(rootP));
        stack_push(&sd, wcs_dup(rootD));
        memset(&prog, 0, sizeof(prog));

        while (sp.n && !stop) {
            wchar_t *dirP, *dirD, *pat;
            WIN32_FIND_DATAW fd;
            HANDLE h;
            if (job_cancelled(job)) break;
            dirP = sp.v[--sp.n];
            dirD = sd.v[--sd.n];
            pat = path_join(dirP, L"*");
            h = FindFirstFileW(pat, &fd);
            free(pat);
            if (h == INVALID_HANDLE_VALUE) {
                R->skippedDirs++;
                free(dirP); free(dirD);
                continue;
            }
            do {
                if (fd.cFileName[0] == L'.' &&
                    (fd.cFileName[1] == 0 ||
                     (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
                    continue;
                if ((++tick & 0xFF) == 0 ||
                    (prog.hits != postedHits && GetTickCount() - lastPost >= 100)) {
                    if (job_cancelled(job)) break;
                    post_progress(job, &prog);      /* 命中即通知，UI 增量显示 */
                    postedHits = prog.hits;
                    lastPost = GetTickCount();
                }
                {
                    wchar_t *fullP = path_join(dirP, fd.cFileName);
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                            free(fullP);
                        } else {
                            stack_push(&sp, fullP);
                            stack_push(&sd, path_join(dirD, fd.cFileName));
                            prog.dirs++;
                        }
                    } else {
                        prog.files++;
                        if (text_file_ext_ok(&c, fd.cFileName)) {
                            int stopAll = 0;
                            scan_text_file(&c, dirD, fd.cFileName, fullP,
                                           ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow,
                                           R, &stopAll);
                            prog.hits = R->n;
                            if (stopAll) stop = 1;
                        }
                        free(fullP);
                    }
                }
            } while (!stop && FindNextFileW(h, &fd));
            FindClose(h);
            free(dirP);
            free(dirD);
        }
        stack_free_all(&sp);
        stack_free_all(&sd);
        R->files = prog.files;
        R->dirs = prog.dirs;
    }
    free(rootP);
    free(rootD);
    ctx_free(&c);
    PostMessageW(job->hwnd, WM_APP_DONE, (WPARAM)2, (LPARAM)job);
    return 0;
}

/* ---------- 任务管理 ---------- */
SearchJob *search_job_new(int feature, HWND hwnd, const wchar_t *root, const wchar_t *needle,
                          int caseSensitive, int matchContains, int useCommonText,
                          const wchar_t *extSpec, int charset)
{
    SearchJob *j = (SearchJob *)xmalloc(sizeof(SearchJob));
    memset(j, 0, sizeof(*j));
    j->feature = feature;
    j->hwnd = hwnd;
    j->root = wcs_dup(root);
    j->needle = wcs_dup(needle);
    j->caseSensitive = caseSensitive;
    j->matchContains = matchContains;
    j->useCommonText = useCommonText;
    j->extSpec = wcs_dup(extSpec);
    j->charset = charset;
    /* 结果容器随 job 创建：UI 在 search_start 后立即接管指针（g_app.results），
       搜索期间即可安全读取实现增量显示；job->results 仅为 worker 侧别名，不做释放依据 */
    j->results = feature == 1 ? (void *)name_results_new()
                              : (void *)text_results_new();
    return j;
}

int search_start(SearchJob *job)
{
    uintptr_t t = _beginthreadex(NULL, 0,
                                 job->feature == 1 ? name_worker : text_worker,
                                 job, 0, NULL);
    if (!t) return 0;
    job->thread = (HANDLE)t;
    return 1;
}

void search_cancel(SearchJob *job)
{
    if (job) InterlockedExchange(&job->cancel, 1);
}

DWORD search_wait(SearchJob *job, DWORD ms)
{
    if (!job || !job->thread) return WAIT_OBJECT_0;
    return WaitForSingleObject(job->thread, ms);
}

void search_job_free(SearchJob *job)
{
    if (!job) return;
    if (job->thread) CloseHandle(job->thread);
    free(job->root);
    free(job->needle);
    free(job->extSpec);
    free(job);
}

void results_free(int feature, void *r)
{
    if (!r) return;
    if (feature == 1) name_results_free((NameResults *)r);
    else              text_results_free((TextResults *)r);
}
