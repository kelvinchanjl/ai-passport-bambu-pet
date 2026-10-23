// main/json_filter.h — v4.4.3 流式 JSON 过滤器:MQTT 分片逐段喂进来,只把需要的字段写进一个小的有界缓冲区。
// 不缓冲整条报文(H2C + 3 台 AMS 的全量报文可以超过 20 KB,工牌没有 PSRAM,最低空闲堆约 74 KB)。
// 规则:对象里的键名在白名单里才保留(任何层级都用同一张白名单),否则整个值跳过;数组元素全部保留(元素内部同样过滤)。
// 输出是紧凑合法 JSON,交给 cJSON 解析(过滤后通常 1-3 KB)。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JF_MAX_DEPTH 24
#define JF_KEY_MAX   32

typedef struct {
    char *out; size_t cap, len;     // 输出缓冲区(调用方提供)
    size_t in_total;                // 已喂入的原始字节数
    uint8_t st, esc, key_len, depth;
    bool key_overflow;
    char key[JF_KEY_MAX];
    struct { uint8_t arr, skip, n; } fr[JF_MAX_DEPTH];
    bool skip_val;                  // 当前值要跳过
    bool err;                       // 语法错误 / 输出溢出 / 嵌套过深
    bool done;                      // 根值已结束
    const char *const *keep; size_t n_keep;
} jf_t;

void jf_init(jf_t *f, char *out, size_t cap, const char *const *keep, size_t n_keep);
void jf_feed(jf_t *f, const char *data, size_t len);
// 结束:成功返回 true,f->out 以 '\0' 结尾,f->len 为长度
bool jf_finish(jf_t *f);
// 固件用的白名单(bambu_parse.c 读的所有键)
extern const char *const JF_BAMBU_KEYS[];
extern const size_t JF_BAMBU_N_KEYS;
