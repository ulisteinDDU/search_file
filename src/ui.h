/* ui.h - 界面构建与布局 */
#ifndef FS_UI_H
#define FS_UI_H

#include "common.h"

typedef struct {
    int    feature;        /* 0=按文件名 1=按内容 */
    HWND   panel;
    HWND   hDirLbl, hDir, hBrowse;
    HWND   hNeedleLbl, hNeedle, hCase;
    HWND   hMatchLbl, hMatch;          /* 页1：匹配模式 */
    HWND   hExtLbl, hExt;              /* 页1：后缀（多选下拉） */
    HWND   hExtHint;                   /* 页1：后缀用法提示 */
    HWND   hCommon;                    /* 页2：常用文本格式 */
    HWND   hCustLbl, hCust;            /* 页2：自定义后缀（多选下拉） */
    HWND   hCharsetLbl, hChkUtf8, hChkGbk, hChkBig5, hChkU16, hCharsetHint;   /* 页2：字符集多选 */
    HWND   hSearch, hStop, hReset;
    HWND   hFilterLbl, hFilter, hFilterBtn, hClearBtn, hTotal;
    HWND   hPpLbl, hPageSize, hFirst, hPrev, hNext, hLast, hPageInfo;
    HWND   hList;
} PageUI;

void  ui_module_init(HINSTANCE hInst);            /* 注册页面窗口类 */
void  ui_init(HINSTANCE hInst);                   /* DPI 缩放 + 字体 */
HFONT ui_font(void);
int   S_(int v96);                                /* 96dpi 基准 -> 当前 dpi */

void  ui_create_page(PageUI *pg, HWND mainWnd, int feature, int baseId);
void  ui_layout_page(PageUI *pg, int cx, int cy);
/* 重建后缀多选列表（listKind: 0=功能一后缀(含“全部”) 1=功能二自定义后缀），
   currentText 中已含的格式在列表里加 “[√] ” 前缀 */
void  ui_rebuild_ext_list(HWND hCombo, int listKind, const wchar_t *currentText);
int   ui_browse_folder(HWND owner, wchar_t *buf, size_t cch);
void  ui_prefill_lastdir(wchar_t *buf, size_t cch);   /* 启动回填上次目录 */

#endif
