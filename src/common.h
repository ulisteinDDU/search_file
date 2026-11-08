/* common.h - 公共包含、常量、消息与控件 ID */
#ifndef FS_COMMON_H
#define FS_COMMON_H

#define WINVER       0x0601
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define __USE_MINGW_ANSI_STDIO 1

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>
#include <process.h>

#define APP_TITLE  L"可视化文件查找工具"
#define APP_CLASS  L"QxFileSearchMainWnd"

/* ---------- 上限与默认值 ---------- */
#define MAX_NAME_HITS        1000000                  /* 功能1 结果条数上限 */
#define MAX_TEXT_HITS        1000000                  /* 功能2 匹配行数上限 */
#define MAX_TEXT_FILE_BYTES  (512ULL * 1024 * 1024)   /* 超过此大小的文本文件跳过 */
#define MAX_LINE_STORE       512                      /* 每行内容最多保留的字符数 */
#define PAGE_MIN             30
#define PAGE_MAX             1000
#define PAGE_DEFAULT         100

/* ---------- 工作线程 -> UI 消息 ---------- */
#define WM_APP_PROGRESS  (WM_APP + 1)
#define WM_APP_DONE      (WM_APP + 2)
#define WM_APP_EXTAPPLY  (WM_APP + 10)   /* 延迟写回后缀多选结果（避开系统覆盖时序） */

/* ---------- 全局命令（快捷键） ---------- */
#define CMD_SWITCH_TAB   30001
#define CMD_TAB1         30002
#define CMD_TAB2         30003
#define CMD_RESEARCH     30004

/* ---------- 控件 ID（页内子 ID，实际 ID = 页基数 1000/2000 + 子 ID） ---------- */
enum {
    IDW_DIR      = 1,   /* 目录输入框 */
    IDW_BROWSE   = 2,   /* 浏览按钮 */
    IDW_NEEDLE   = 3,   /* 文件名 / 查找内容输入框 */
    IDW_CASE     = 4,   /* 区分大小写复选框 */
    IDW_MATCH    = 5,   /* 匹配模式(仅页1) */
    IDW_EXT      = 6,   /* 后缀(仅页1) */
    IDW_COMMON   = 7,   /* 常用文本格式(仅页2) */
    IDW_CUSTEXT  = 8,   /* 自定义后缀(仅页2) */
    IDW_SEARCH   = 9,   /* 开始查找 */
    IDW_STOP     = 10,  /* 停止 */
    IDW_FILTERE  = 11,  /* 过滤输入框 */
    IDW_FILTER   = 12,  /* 过滤按钮 */
    IDW_CLEAR    = 13,  /* 清除过滤 */
    IDW_PAGESIZE = 15,  /* 每页条数 */
    IDW_FIRST    = 16,
    IDW_PREV     = 17,
    IDW_NEXT     = 18,
    IDW_LAST     = 19,
    IDW_LIST     = 21,  /* 结果列表 */
    IDW_TOTAL    = 22,  /* 总数标签 */
    IDW_PAGEINFO = 23,  /* 页码标签 */
    IDW_CHARSET  = 24,  /* 字符集标签(仅页2) */
    IDW_RESET    = 25,  /* 清空（恢复默认选项） */
    IDW_CHKGBK   = 26,  /* 字符集勾选：GBK/GB18030(仅页2) */
    IDW_CHKBIG5  = 27,  /* 字符集勾选：Big5(仅页2) */
    IDW_CHKU16   = 28,  /* 字符集勾选：UTF-16LE(仅页2) */
    IDC_TAB      = 900, /* TAB 控件 */
    IDC_STATUS   = 901, /* 状态栏 */
    IDM_HELP_FUNC   = 40001,   /* 帮助菜单 */
    IDM_HELP_RULES  = 40002,
    IDM_HELP_CHARSET= 40003,
    IDM_HELP_KEYS   = 40004,
    IDM_HELP_ABOUT  = 40005
};

/* 功能2 内容匹配字符集（位标志，可多选；带 BOM 的文件始终按 BOM 解码，
   勾选只决定无 BOM 文件按哪些字符集逐一尝试，命中即止） */
enum {
    CSF_UTF8     = 1,   /* UTF-8 */
    CSF_GBK18030 = 2,   /* GBK/GB18030（代码页 54936，向下兼容 GBK/GB2312） */
    CSF_BIG5     = 4,   /* Big5 繁体中文（950） */
    CSF_UTF16LE  = 8    /* UTF-16LE（作用于无 BOM 文件） */
};
#define CSF_DEFAULT  (CSF_UTF8 | CSF_GBK18030)   /* 默认勾选：UTF-8 + GBK/GB18030 */

typedef struct {
    unsigned long long files;
    unsigned long long dirs;
    unsigned long long hits;
    const void        *job;   /* 所属任务（UI 丢弃过期任务的残留进度消息） */
} ProgressSnap;

int run_selftest(void); /* selftest.c */

#endif
