/* util.h - 字符串/路径/后缀/格式化 工具 */
#ifndef FS_UTIL_H
#define FS_UTIL_H

#include "common.h"

void     *xmalloc(size_t bytes);
void     *xrealloc(void *p, size_t bytes);

wchar_t *wcs_dup(const wchar_t *s);                 /* NULL 安全 */
wchar_t *wcs_dupn(const wchar_t *s, size_t n);
wchar_t *wcs_lower_dup(const wchar_t *s);
void     wcs_lower_inplace(wchar_t *s);

/* parent\child，自动处理 parent 末尾分隔符；返回新分配串 */
wchar_t *path_join(const wchar_t *parent, const wchar_t *child);

/* 校验并生成查找用根目录：*displayOut 为规范化显示串(新分配)，
   返回带 \\?\ 前缀的串(新分配)；目录无效返回 NULL(*displayOut 为 NULL) */
wchar_t *make_prefixed_root(const wchar_t *userRoot, wchar_t **displayOut);

/* 解析 "txt; cpp, .h"：拆分、去点、转小写；返回条数(可能 0) */
int      ext_list_parse(const wchar_t *spec, wchar_t ***itemsOut, size_t *countOut);
void     ext_list_free(wchar_t **items, size_t count);
/* 文件名(含扩展名)后缀是否命中列表；空列表 = 全部通过；无扩展名 = 不通过 */
int      ext_list_match(wchar_t **items, size_t count, const wchar_t *fileName);

/* 常用文本格式后缀（分号分隔，小写） */
const wchar_t *common_text_ext_spec(void);

/* 宽字符子串查找：区分大小写 / 不敏感(needle 需预先小写) */
const wchar_t *wcs_find(const wchar_t *hay, size_t hayLen, const wchar_t *nd, size_t ndLen);
const wchar_t *wcs_ifind(const wchar_t *hay, size_t hayLen, const wchar_t *ndLower, size_t ndLen);
/* 字节子串查找 */
const unsigned char *mem_find(const unsigned char *hay, size_t hayLen,
                              const unsigned char *nd, size_t ndLen);

/* 取文件名的基本名/扩展名（最后一个'.'，开头的不算） */
void split_filename(const wchar_t *fileName,
                    const wchar_t **base, size_t *baseLen,
                    const wchar_t **ext, size_t *extLen);

void fmt_u64(unsigned long long v, wchar_t *buf, size_t cch);
void fmt_size(unsigned long long bytes, wchar_t *buf, size_t cch);
void fmt_filetime(const FILETIME *ft, wchar_t *buf, size_t cch);

#endif
