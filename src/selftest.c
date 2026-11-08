/* selftest.c - 命令行自测：FileSearchTool.exe --selftest
   在临时目录构造用例，校验两套引擎的命中数；写 selftest_result.txt，退出码 0=PASS */
#include "common.h"
#include "util.h"
#include "results.h"
#include "search.h"
#include <locale.h>

static FILE *g_log;
static int g_pass = 1;

static void check(const wchar_t *name, int got, int expect)
{
    fwprintf(g_log, L"%-42ls expect=%d got=%d %ls\n", name, expect, got,
             got == expect ? L"[OK]" : L"[FAIL]");
    if (got != expect) g_pass = 0;
}

static void write_bytes(const wchar_t *path, const void *data, size_t n)
{
    FILE *f = _wfopen(path, L"wb");
    if (!f) return;
    if (n) fwrite(data, 1, n, f);
    fclose(f);
}

static void write_text(const wchar_t *path, const char *s)
{
    write_bytes(path, s, strlen(s));
}

static void write_utf8bom(const wchar_t *path, const char *s)
{
    FILE *f = _wfopen(path, L"wb");
    if (!f) return;
    fputc(0xEF, f); fputc(0xBB, f); fputc(0xBF, f);   /* BOM: UTF-8 */
    fwrite(s, 1, strlen(s), f);
    fclose(f);
}

static void write_utf16(const wchar_t *path, const wchar_t *s)
{
    FILE *f = _wfopen(path, L"wb");
    if (!f) return;
    fputc(0xFF, f); fputc(0xFE, f);                 /* BOM: UTF-16LE */
    fwrite(s, sizeof(wchar_t), wcslen(s), f);
    fclose(f);
}

static NameResults *run_name(const wchar_t *root, const wchar_t *needle,
                             int cs, int contains, const wchar_t *ext)
{
    SearchJob *j = search_job_new(1, NULL, root, needle, cs, contains, 0, ext, CSF_DEFAULT);
    NameResults *R;
    search_start(j);
    search_wait(j, INFINITE);
    R = (NameResults *)j->results;
    search_job_free(j);
    return R;
}

static TextResults *run_text(const wchar_t *root, const wchar_t *needle,
                             int cs, int useCommon, const wchar_t *ext, int charset)
{
    SearchJob *j = search_job_new(2, NULL, root, needle, cs, 0, useCommon, ext, charset);
    TextResults *R;
    search_start(j);
    search_wait(j, INFINITE);
    R = (TextResults *)j->results;
    search_job_free(j);
    return R;
}

int run_selftest(void)
{
    wchar_t tmp[MAX_PATH], dir[MAX_PATH], sub[MAX_PATH], p[MAX_PATH];
    NameResults *NR;
    TextResults *TR;

    g_log = _wfopen(L"selftest_result.txt", L"w");
    if (!g_log) return 1;
    setlocale(LC_ALL, "");                   /* 日志中文按系统代码页输出 */

    GetTempPathW(MAX_PATH, tmp);
    swprintf(dir, MAX_PATH, L"%lsQxFsSelfTest", tmp);
    swprintf(sub, MAX_PATH, L"%ls\\sub", dir);
    CreateDirectoryW(dir, NULL);
    CreateDirectoryW(sub, NULL);

    /* 构造用例 */
    swprintf(p, MAX_PATH, L"%ls\\Hello.txt", dir);
    write_text(p, "Hello World\nsecond line has Apple\n");
    swprintf(p, MAX_PATH, L"%ls\\data.log", dir);
    write_text(p, "apple pie\n");
    swprintf(p, MAX_PATH, L"%ls\\Makefile", dir);
    write_text(p, "all: build\n\tclean Apple core\n");   /* 无扩展名文本 */
    swprintf(p, MAX_PATH, L"%ls\\note.md", sub);
    write_utf8bom(p, "\xe4\xbd\xa0\xe5\xa5\xbd world\n"); /* UTF-8+BOM: 你好 world */
    swprintf(p, MAX_PATH, L"%ls\\Report_2024.txt", sub);
    write_text(p, "a\n");
    swprintf(p, MAX_PATH, L"%ls\\uni.txt", sub);
    write_utf16(p, L"unicode Apple here\n");              /* UTF-16LE */
    swprintf(p, MAX_PATH, L"%ls\\binary.bin", sub);
    {   char bin[8] = { 0x00, 0x01, 0x02, 'A', 'p', 'p', 'l', 'e' };
        write_bytes(p, bin, 8); }                          /* 非文本后缀，应被过滤 */

    fwprintf(g_log, L"== name search (root=%ls) ==\n", dir);

    NR = run_name(dir, L"Report_2024", 1, 0, L"txt");
    check(L"T1_name_exact_CS_exttxt", NR ? (int)NR->n : -1, 1);
    name_results_free(NR);

    NR = run_name(dir, L"REPORT_2024", 0, 0, NULL);   /* 大写输入检验忽略大小写 */
    check(L"T2_name_exact_CI", NR ? (int)NR->n : -1, 1);
    name_results_free(NR);

    NR = run_name(dir, L"PORT_20", 0, 1, NULL);
    check(L"T3_name_contains_CI", NR ? (int)NR->n : -1, 1);
    name_results_free(NR);

    NR = run_name(dir, L"Report", 1, 1, NULL);
    check(L"T4_name_contains_CS", NR ? (int)NR->n : -1, 1);
    name_results_free(NR);

    NR = run_name(dir, L"Makefile", 0, 0, L"txt");    /* 后缀不符 -> 排除 */
    check(L"T13_name_ext_filter_reject", NR ? (int)NR->n : -1, 0);
    name_results_free(NR);

    NR = run_name(dir, L"data", 0, 1, L"log");        /* 后缀命中 */
    check(L"T14_name_ext_filter_accept", NR ? (int)NR->n : -1, 1);
    name_results_free(NR);

    NR = run_name(dir, L"Report", 0, 1, L"txt;log");  /* 多后缀 */
    check(L"T15_name_multi_ext", NR ? (int)NR->n : -1, 1);
    name_results_free(NR);

    fwprintf(g_log, L"== text search ==\n");

    TR = run_text(dir, L"Apple", 1, 1, NULL, CSF_DEFAULT);
    check(L"T5_text_CS", TR ? (int)TR->n : -1, 3);
    text_results_free(TR);

    TR = run_text(dir, L"Apple", 0, 1, NULL, CSF_DEFAULT);
    check(L"T6_text_CI", TR ? (int)TR->n : -1, 4);
    text_results_free(TR);

    TR = run_text(dir, L"\x4f60\x597d", 0, 1, NULL, CSF_DEFAULT);  /* 你好 */
    check(L"T7_text_chinese_utf8bom", TR ? (int)TR->n : -1, 1);
    text_results_free(TR);

    TR = run_text(dir, L"all", 0, 0, L"mak", CSF_DEFAULT);
    check(L"T8_text_custom_ext", TR ? (int)TR->n : -1, 0);
    text_results_free(TR);

    TR = run_text(dir, L"world", 1, 1, NULL, CSF_DEFAULT);
    check(L"T9_text_CS_world", TR ? (int)TR->n : -1, 1);
    text_results_free(TR);

    /* ---- 字符集用例 ---- */
    swprintf(p, MAX_PATH, L"%ls\\gbk.txt", dir);
    {   char gbk[] = { 'a', ' ', (char)0xC4, (char)0xE3, (char)0xBA, (char)0xC3, ' ', 'b', '\n' };
        write_bytes(p, gbk, sizeof(gbk)); }              /* GBK 无 BOM: a 你好 b */
    swprintf(p, MAX_PATH, L"%ls\\utf8nb.txt", dir);
    write_text(p, "x \xe4\xb8\x96\xe7\x95\x8c y\n");     /* UTF-8 无 BOM: x 世界 y */
    swprintf(p, MAX_PATH, L"%ls\\readme.md", dir);
    write_text(p, "\xe4\xbd\xa0\xe5\xa5\xbd markdown\n");/* UTF-8 无 BOM: 你好 markdown */
    swprintf(p, MAX_PATH, L"%ls\\sqlgbk.sql", dir);
    write_text(p, "SELECT * FROM t; -- \xc4\xe3\xba\xc3\n"); /* GBK 无 BOM: -- 你好 */
    swprintf(p, MAX_PATH, L"%ls\\big5.txt", dir);
    {   char b5[] = { (char)0xA7, (char)0x41, (char)0xA6, (char)0x6E, ' ', 'b', '\n' };
        write_bytes(p, b5, sizeof(b5)); }                /* Big5 无 BOM: 你好 b */

    TR = run_text(dir, L"\x4f60\x597d", 1, 0, L"txt", CSF_GBK18030);
    check(L"T10_charset_GBK18030_forced", TR ? (int)TR->n : -1, 1);
    text_results_free(TR);

    TR = run_text(dir, L"\x4e16\x754c", 1, 0, L"txt", CSF_UTF8);
    check(L"T11_charset_UTF8_nobom", TR ? (int)TR->n : -1, 1);
    text_results_free(TR);

    TR = run_text(dir, L"Apple", 1, 1, NULL, CSF_UTF16LE);/* BOM 优先：仅 uni.txt 命中 */
    check(L"T12_charset_bom_priority", TR ? (int)TR->n : -1, 1);
    text_results_free(TR);

    /* 默认掩码（UTF-8+GB18030）：GBK 的 .sql 与 UTF-8 无 BOM/BOM 的 .md 同时命中 */
    TR = run_text(dir, L"\x4f60\x597d", 0, 0, L"sql", CSF_DEFAULT);
    check(L"T16_default_mask_gbk_sql", TR ? (int)TR->n : -1, 1);
    text_results_free(TR);

    TR = run_text(dir, L"\x4f60\x597d", 0, 0, L"md", CSF_DEFAULT);
    check(L"T17_default_mask_utf8_md", TR ? (int)TR->n : -1, 2);
    text_results_free(TR);

    TR = run_text(dir, L"\x4f60\x597d", 0, 0, L"txt", CSF_BIG5);
    check(L"T18_charset_big5", TR ? (int)TR->n : -1, 1);
    text_results_free(TR);

    /* 结果容器随 job 预创建（增量显示的所有权前提） */
    {
        SearchJob *j = search_job_new(2, NULL, dir, L"x", 0, 0, 0, NULL, CSF_DEFAULT);
        check(L"T19_results_precreated", j && j->results ? 1 : 0, 1);
        results_free(2, j->results);
        j->results = NULL;
        search_job_free(j);
    }

    /* 清理 */
    {
        const wchar_t *files[] = {
            L"Hello.txt", L"data.log", L"Makefile", L"gbk.txt", L"utf8nb.txt",
            L"readme.md", L"sqlgbk.sql", L"big5.txt", NULL
        };
        const wchar_t *subfiles[] = {
            L"note.md", L"Report_2024.txt", L"uni.txt", L"binary.bin", NULL
        };
        int i;
        for (i = 0; files[i]; i++) {
            swprintf(p, MAX_PATH, L"%ls\\%ls", dir, files[i]);
            DeleteFileW(p);
        }
        for (i = 0; subfiles[i]; i++) {
            swprintf(p, MAX_PATH, L"%ls\\%ls", sub, subfiles[i]);
            DeleteFileW(p);
        }
    }
    RemoveDirectoryW(sub);
    RemoveDirectoryW(dir);

    fwprintf(g_log, L"\nRESULT: %ls\n", g_pass ? L"PASS" : L"FAIL");
    fclose(g_log);
    return g_pass ? 0 : 1;
}
