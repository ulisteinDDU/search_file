/* search.h - 后台查找任务 */
#ifndef FS_SEARCH_H
#define FS_SEARCH_H

#include "common.h"

typedef struct {
    HWND         hwnd;            /* 通知窗口（自测时可为 NULL） */
    int          feature;         /* 1=按文件名 2=按内容 */
    wchar_t     *root;            /* 用户输入根目录 */
    wchar_t     *needle;          /* 文件名 / 查找串 */
    int          caseSensitive;
    int          matchContains;   /* 功能1：包含匹配（默认全字） */
    int          useCommonText;   /* 功能2：包含常用文本格式 */
    wchar_t     *extSpec;         /* 功能1：后缀；功能2：自定义后缀 */
    int          charset;         /* 功能2：字符集（CS_*） */
    volatile LONG cancel;
    HANDLE       thread;
    void        *results;         /* 完成后有效：NameResults 或 TextResults 指针 */
} SearchJob;

SearchJob *search_job_new(int feature, HWND hwnd, const wchar_t *root, const wchar_t *needle,
                          int caseSensitive, int matchContains, int useCommonText,
                          const wchar_t *extSpec, int charset);
int  search_start(SearchJob *job);          /* 线程启动成功返回 1 */
void search_cancel(SearchJob *job);
DWORD search_wait(SearchJob *job, DWORD ms);   /* 返回 WaitForSingleObject 原值 */
void search_job_free(SearchJob *job);       /* 释放 job 与其字符串（不含 results） */
void results_free(int feature, void *r);

#endif
