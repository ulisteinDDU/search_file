/* results.c - 结果容器实现
   容器内含临界区：worker 线程 push 加锁，UI 线程搜索期间经 *_results_lock
   短暂加锁读取（快照式拷贝字段/指针），实现查找过程中增量实时显示。 */
#include "results.h"
#include "util.h"

NameResults *name_results_new(void)
{
    NameResults *R = (NameResults *)xmalloc(sizeof(NameResults));
    memset(R, 0, sizeof(*R));
    InitializeCriticalSection(&R->cs);
    return R;
}

void name_results_free(NameResults *R)
{
    size_t i;
    if (!R) return;
    for (i = 0; i < R->n; i++) free(R->hits[i].path);
    free(R->hits);
    DeleteCriticalSection(&R->cs);
    free(R);
}

void name_results_lock(NameResults *R)   { if (R) EnterCriticalSection(&R->cs); }
void name_results_unlock(NameResults *R) { if (R) LeaveCriticalSection(&R->cs); }

void name_push(NameResults *R, const wchar_t *displayPath,
               unsigned long long size, const FILETIME *mtime)
{
    NameHit *h;
    EnterCriticalSection(&R->cs);
    if (R->n == R->cap) {
        R->cap = R->cap ? R->cap * 2 : 1024;
        R->hits = (NameHit *)xrealloc(R->hits, R->cap * sizeof(NameHit));
    }
    h = &R->hits[R->n++];
    h->path = wcs_dup(displayPath);
    h->size = size;
    h->mtime = *mtime;
    LeaveCriticalSection(&R->cs);
}

TextResults *text_results_new(void)
{
    TextResults *R = (TextResults *)xmalloc(sizeof(TextResults));
    memset(R, 0, sizeof(*R));
    InitializeCriticalSection(&R->cs);
    return R;
}

void text_results_free(TextResults *R)
{
    size_t i;
    if (!R) return;
    for (i = 0; i < R->nPath; i++) free(R->paths[i]);
    for (i = 0; i < R->n; i++) free(R->hits[i].text);
    free(R->paths);
    free(R->hits);
    DeleteCriticalSection(&R->cs);
    free(R);
}

void text_results_lock(TextResults *R)   { if (R) EnterCriticalSection(&R->cs); }
void text_results_unlock(TextResults *R) { if (R) LeaveCriticalSection(&R->cs); }

void text_push_path(TextResults *R, wchar_t *pathOwned)
{
    EnterCriticalSection(&R->cs);
    if (R->nPath == R->capPath) {
        R->capPath = R->capPath ? R->capPath * 2 : 256;
        R->paths = (wchar_t **)xrealloc(R->paths, R->capPath * sizeof(wchar_t *));
    }
    R->paths[R->nPath++] = pathOwned;
    LeaveCriticalSection(&R->cs);
}

void text_push_hit(TextResults *R, size_t fileIdx, unsigned line,
                   const wchar_t *text, size_t textLen)
{
    TextHit *h;
    int ell;
    size_t n;
    EnterCriticalSection(&R->cs);
    if (R->n == R->cap) {
        R->cap = R->cap ? R->cap * 2 : 1024;
        R->hits = (TextHit *)xrealloc(R->hits, R->cap * sizeof(TextHit));
    }
    h = &R->hits[R->n++];
    h->fileIdx = fileIdx;
    h->line = line;
    n = textLen > MAX_LINE_STORE ? MAX_LINE_STORE : textLen;
    ell = textLen > MAX_LINE_STORE;
    h->text = (wchar_t *)xmalloc((n + (size_t)ell + 1) * sizeof(wchar_t));
    if (n) memcpy(h->text, text, n * sizeof(wchar_t));
    if (ell) h->text[n++] = L'\u2026';                  /* … */
    h->text[n] = 0;
    LeaveCriticalSection(&R->cs);
}
