/* util.c - 基础工具实现 */
#include "util.h"

void *xmalloc(size_t bytes)
{
    void *p = malloc(bytes ? bytes : 1);
    if (!p) {
        MessageBoxW(NULL, L"内存分配失败，程序即将退出。", APP_TITLE, MB_ICONERROR | MB_OK);
        ExitProcess(2);
    }
    return p;
}

void *xrealloc(void *p, size_t bytes)
{
    void *q = realloc(p, bytes ? bytes : 1);
    if (!q) {
        MessageBoxW(NULL, L"内存分配失败，程序即将退出。", APP_TITLE, MB_ICONERROR | MB_OK);
        ExitProcess(2);
    }
    return q;
}

wchar_t *wcs_dup(const wchar_t *s)
{
    size_t n;
    wchar_t *p;
    if (!s) return NULL;
    n = wcslen(s);
    p = (wchar_t *)xmalloc((n + 1) * sizeof(wchar_t));
    memcpy(p, s, (n + 1) * sizeof(wchar_t));
    return p;
}

wchar_t *wcs_dupn(const wchar_t *s, size_t n)
{
    wchar_t *p = (wchar_t *)xmalloc((n + 1) * sizeof(wchar_t));
    if (n) memcpy(p, s, n * sizeof(wchar_t));
    p[n] = 0;
    return p;
}

wchar_t *wcs_lower_dup(const wchar_t *s)
{
    wchar_t *p = wcs_dup(s);
    if (p) wcs_lower_inplace(p);
    return p;
}

void wcs_lower_inplace(wchar_t *s)
{
    for (; s && *s; s++) *s = (wchar_t)towlower(*s);
}

wchar_t *path_join(const wchar_t *parent, const wchar_t *child)
{
    size_t a = wcslen(parent), b = wcslen(child);
    int sep = (a > 0 && parent[a - 1] != L'\\' && parent[a - 1] != L'/');
    wchar_t *p = (wchar_t *)xmalloc((a + (size_t)sep + b + 1) * sizeof(wchar_t));
    memcpy(p, parent, a * sizeof(wchar_t));
    if (sep) p[a] = L'\\';
    memcpy(p + a + sep, child, b * sizeof(wchar_t));
    p[a + (size_t)sep + b] = 0;
    return p;
}

wchar_t *make_prefixed_root(const wchar_t *userRoot, wchar_t **displayOut)
{
    wchar_t *tmp, *out;
    size_t n;
    DWORD attr;

    *displayOut = NULL;
    if (!userRoot || !*userRoot) return NULL;
    attr = GetFileAttributesW(userRoot);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return NULL;

    tmp = wcs_dup(userRoot);
    for (wchar_t *p = tmp; *p; p++)
        if (*p == L'/') *p = L'\\';
    n = wcslen(tmp);
    /* 去掉末尾分隔符（盘符根 "C:\" 保留） */
    while (n > 0 && tmp[n - 1] == L'\\' && !(n == 3 && tmp[1] == L':'))
        tmp[--n] = 0;

    if (tmp[0] == L'\\' && tmp[1] == L'\\') {          /* UNC \\server\share */
        out = (wchar_t *)xmalloc((8 + wcslen(tmp + 2) + 1) * sizeof(wchar_t));
        wcscpy(out, L"\\\\?\\UNC\\");
        wcscat(out, tmp + 2);
    } else {
        out = (wchar_t *)xmalloc((4 + n + 1) * sizeof(wchar_t));
        wcscpy(out, L"\\\\?\\");
        wcscat(out, tmp);
    }
    *displayOut = tmp;                                  /* 规范化显示串 */
    return out;
}

static int is_ext_delim(wchar_t c)
{
    return c == L';' || c == L',' || c == L' ' || c == L'\t' || c == L'|';
}

int ext_list_parse(const wchar_t *spec, wchar_t ***itemsOut, size_t *countOut)
{
    wchar_t **items = NULL;
    size_t n = 0, cap = 0;
    const wchar_t *p = spec;

    *itemsOut = NULL;
    *countOut = 0;
    if (!spec) return 0;
    while (*p) {
        const wchar_t *s;
        size_t len;
        while (*p && is_ext_delim(*p)) p++;
        if (!*p) break;
        s = p;
        while (*p && !is_ext_delim(*p)) p++;
        len = (size_t)(p - s);
        while (len > 0 && s[0] == L'.') { s++; len--; }
        while (len > 0 && s[len - 1] == L'.') len--;
        if (!len) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            items = (wchar_t **)xrealloc(items, cap * sizeof(wchar_t *));
        }
        {
            wchar_t *it = wcs_dupn(s, len);
            wcs_lower_inplace(it);
            items[n++] = it;
        }
    }
    *itemsOut = items;
    *countOut = n;
    return (int)n;
}

void ext_list_free(wchar_t **items, size_t count)
{
    size_t i;
    if (!items) return;
    for (i = 0; i < count; i++) free(items[i]);
    free(items);
}

int ext_list_match(wchar_t **items, size_t count, const wchar_t *fileName)
{
    const wchar_t *dot;
    size_t i;
    if (!items || !count) return 1;                     /* 空列表 = 不过滤 */
    dot = wcsrchr(fileName, L'.');
    if (!dot || dot == fileName) return 0;              /* 无扩展名 */
    for (i = 0; i < count; i++) {
        size_t l = wcslen(items[i]);
        if (!wcsncmp(dot + 1, items[i], l) && dot[1 + l] == 0)
            return 1;
    }
    return 0;
}

const wchar_t *common_text_ext_spec(void)
{
    return L"txt;log;ini;cfg;conf;config;csv;tsv;properties;env;toml;reg;"
           L"c;h;cpp;hpp;cc;cxx;hh;inc;cs;java;py;pyw;rb;go;rs;swift;kt;scala;lua;pl;pm;php;d;nim;zig;r;m;"
           L"js;mjs;cjs;ts;tsx;jsx;json;html;htm;xhtml;css;scss;less;xml;xaml;svg;yaml;yml;md;markdown;rst;"
           L"sql;bat;cmd;ps1;psm1;sh;bash;awk;sed;mak;mk;gradle;"
           L"v;sv;vhd;vhdl;asm;s;rc;pas;dpr;vb;bas;f;for;f90;f95;fs;tex;bib;el;lisp;scm;clj;asp;aspx;jsp";
}

const wchar_t *wcs_find(const wchar_t *hay, size_t hayLen, const wchar_t *nd, size_t ndLen)
{
    size_t i, j;
    if (!ndLen) return NULL;
    for (i = 0; i + ndLen <= hayLen; i++) {
        for (j = 0; j < ndLen && hay[i + j] == nd[j]; j++) {}
        if (j == ndLen) return hay + i;
    }
    return NULL;
}

const wchar_t *wcs_ifind(const wchar_t *hay, size_t hayLen, const wchar_t *ndLower, size_t ndLen)
{
    size_t i, j;
    if (!ndLen) return NULL;
    for (i = 0; i + ndLen <= hayLen; i++) {
        for (j = 0; j < ndLen && (wchar_t)towlower(hay[i + j]) == ndLower[j]; j++) {}
        if (j == ndLen) return hay + i;
    }
    return NULL;
}

const unsigned char *mem_find(const unsigned char *hay, size_t hayLen,
                              const unsigned char *nd, size_t ndLen)
{
    size_t i, j;
    if (!ndLen) return NULL;
    for (i = 0; i + ndLen <= hayLen; i++) {
        for (j = 0; j < ndLen && hay[i + j] == nd[j]; j++) {}
        if (j == ndLen) return hay + i;
    }
    return NULL;
}

void split_filename(const wchar_t *fileName,
                    const wchar_t **base, size_t *baseLen,
                    const wchar_t **ext, size_t *extLen)
{
    const wchar_t *dot = wcsrchr(fileName, L'.');
    if (!dot || dot == fileName) {
        *base = fileName; *baseLen = wcslen(fileName);
        *ext = L"";      *extLen = 0;
    } else {
        *base = fileName; *baseLen = (size_t)(dot - fileName);
        *ext = dot + 1;  *extLen = wcslen(dot + 1);
    }
}

void fmt_u64(unsigned long long v, wchar_t *buf, size_t cch)
{
    swprintf(buf, cch, L"%llu", v);
}

void fmt_size(unsigned long long bytes, wchar_t *buf, size_t cch)
{
    double v = (double)bytes;
    if (bytes < 1024)                      swprintf(buf, cch, L"%llu B", bytes);
    else if ((v /= 1024.0) < 1024.0)       swprintf(buf, cch, L"%.2f KB", v);
    else if ((v /= 1024.0) < 1024.0)       swprintf(buf, cch, L"%.2f MB", v);
    else if ((v /= 1024.0) < 1024.0)       swprintf(buf, cch, L"%.2f GB", v);
    else { v /= 1024.0;                    swprintf(buf, cch, L"%.2f TB", v); }
}

void fmt_filetime(const FILETIME *ft, wchar_t *buf, size_t cch)
{
    SYSTEMTIME st, lt;
    if (FileTimeToSystemTime(ft, &st) &&
        SystemTimeToTzSpecificLocalTime(NULL, &st, &lt)) {
        swprintf(buf, cch, L"%04u-%02u-%02u %02u:%02u:%02u",
                 lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond);
    } else {
        wcscpy(buf, L"-");
    }
}
