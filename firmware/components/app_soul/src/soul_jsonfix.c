/*
 * soul_jsonfix.c — 深思回复机械 JSON 恢复实现(S6 D2; 见 soul_jsonfix.h)
 *
 * 扫描状态机: in_string(转义感知)/escape/括号栈('{' '[' 压, '}' ']' 弹,
 * 栈空时的 '}' = 对象终点)。截断补齐: 按"栈余+字符串外"判定——若终点
 * 在字符串内, 先补 '"' 再按栈序补括号。UTF-8: 尾部连续字节 10xxxxxx
 * 计数与首字节声明长度不符则剔除。
 */
#include "soul_jsonfix.h"

#include <string.h>

/* UTF-8 尾部截断剔除: 返回安全长度(不完整多字节序列剔除) */
static size_t utf8_safe_len(const char *s, size_t len)
{
    /* 从尾向前找非续行字节(0x80-0xBF), 核对其声明长度 */
    size_t back = 0;
    while (back < len && back < 4) {
        unsigned char c = (unsigned char)s[len - 1 - back];
        if ((c & 0xC0) != 0x80) {           /* 非续行字节 */
            size_t declared;
            if ((c & 0x80) == 0)          declared = 1;
            else if ((c & 0xE0) == 0xC0)  declared = 2;
            else if ((c & 0xF0) == 0xE0)  declared = 3;
            else if ((c & 0xF8) == 0xF0)  declared = 4;
            else                          declared = back + 2;  /* 越界字节 */
            /* 首字节+续行数 == declared 才完整 */
            if (declared != back + 1)
                return len - back - 1;    /* 剔除不完整序列含孤儿首字节
                                             (S6 审计 P1-1: 旧 len-back 留下
                                             裸 E6/F0 直达 TTS) */
            return len;
        }
        back++;
    }
    return back < len ? len - back : 0;   /* 全是续行字节: 全剔 */
}

bool soul_json_fix(const char *in, size_t len, char *out, size_t cap,
                   size_t *out_len)
{
    if (in == NULL || out == NULL || cap == 0)
        return false;

    /* ①首个 '{' 起(散文剥离) */
    const char *start = memchr(in, '{', len);
    if (start == NULL)
        return false;
    size_t off = (size_t)(start - in);
    len -= off;
    in = start;

    /* ②字符串感知的括号栈扫描, 定对象终点或截断点 */
    char stack[16];
    int sp = 0;
    bool in_string = false, esc = false;
    size_t end = 0;                         /* 有效内容长度(不含补齐) */
    bool complete = false;
    size_t scan = utf8_safe_len(in, len);
    for (size_t i = 0; i < scan; i++) {
        char c = in[i];
        if (esc) {
            esc = false;
            continue;
        }
        if (in_string) {
            if (c == '\\')
                esc = true;
            else if (c == '"')
                in_string = false;
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            if (sp >= (int)sizeof(stack))
                return false;               /* 超深: 视为攻击/乱码 */
            stack[sp++] = c;
        } else if (c == '}' || c == ']') {
            if (sp == 0)
                return false;               /* 逆序括号: 乱码 */
            char open = stack[--sp];
            if ((c == '}' && open != '{') || (c == ']' && open != '['))
                return false;
            if (sp == 0) {
                end = i + 1;                /* 顶层对象闭合 */
                complete = true;
                break;
            }
        }
    }
    if (!complete) {
        /* 截断恢复: 已扫前缀=合法内容(补齐尾即可解析); 无对象内容
         * (首字节即被 UTF-8 剔光) → 放弃 */
        if (scan == 0)
            return false;
        end = scan;
    }

    /* ③输出 = [0,end) + 补齐尾; 长度钳位(cap-1) */
    size_t total = end;
    size_t pad = 0;
    if (!complete) {
        pad = (size_t)sp + (in_string ? 1u : 0u);
    }
    if (total + pad + 1 > cap)
        return false;                       /* 缓冲不足(调用方加缸) */
    memmove(out, in, total);   /* in==out(原位)允许 */
    if (!complete) {
        if (in_string)
            out[total++] = '"';
        for (int i = sp - 1; i >= 0; i--)
            out[total++] = stack[i] == '{' ? '}' : ']';
    }
    out[total] = '\0';
    if (out_len != NULL)
        *out_len = total;
    return true;
}
