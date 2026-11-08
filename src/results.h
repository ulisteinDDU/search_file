/* results.h - 结果容器 */
#ifndef FS_RESULTS_H
#define FS_RESULTS_H

#include "common.h"

/* 功能1：按文件名命中的文件 */
typedef struct {
    wchar_t             *path;    /* 绝对路径（显示用，无 \\?\ 前缀） */
    unsigned long long   size;
    FILETIME             mtime;
} NameHit;

typedef struct {
    NameHit             *hits;
    size_t               n, cap;
    unsigned long long   files, dirs, skippedDirs;   /* 统计 */
    int                  truncated;                   /* 达到上限提前停止 */
    CRITICAL_SECTION     cs;                          /* worker 写 / UI 读互斥 */
} NameResults;

/* 功能2：按内容命中的行（路径入池去重） */
typedef struct {
    size_t     fileIdx;      /* TextResults.paths 下标 */
    unsigned   line;         /* 1 起物理行号 */
    wchar_t   *text;         /* 行内容（截断至 MAX_LINE_STORE 字符） */
} TextHit;

typedef struct {
    wchar_t           **paths;
    size_t               nPath, capPath;
    TextHit            *hits;
    size_t               n, cap;
    unsigned long long   files, dirs, skippedDirs, skippedFiles, skippedLarge;
    int                  truncated;
    CRITICAL_SECTION     cs;                          /* worker 写 / UI 读互斥 */
} TextResults;

NameResults *name_results_new(void);
void         name_results_free(NameResults *R);
void         name_results_lock(NameResults *R);     /* UI 读侧短暂加锁用 */
void         name_results_unlock(NameResults *R);
void         name_push(NameResults *R, const wchar_t *displayPath,
                       unsigned long long size, const FILETIME *mtime);

TextResults *text_results_new(void);
void         text_results_free(TextResults *R);
void         text_results_lock(TextResults *R);     /* UI 读侧短暂加锁用 */
void         text_results_unlock(TextResults *R);
void         text_push_path(TextResults *R, wchar_t *pathOwned);      /* 接管所有权 */
void         text_push_hit(TextResults *R, size_t fileIdx, unsigned line,
                           const wchar_t *text, size_t textLen);

#endif
