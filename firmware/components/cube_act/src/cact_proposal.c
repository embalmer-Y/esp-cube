/*
 * cact_proposal.c —— LLM 提案严格解析实现(见 cact_proposal.h)
 *
 * 手写递归下降(深度≤2), 无 libc 数字解析(规避 strtod 对
 * NaN/Inf/exponent 的宽容)。任何失败即整体拒绝。
 */
#include <stdio.h>
#include <string.h>

#include "cact_proposal.h"

const cact_proposal_limits_t CACT_PROPOSAL_LIMITS_DEF = {
    .max_text_len = 1024,
    .max_ttl_s = 3600,
};

/* ---- 文法工具 ---- */
typedef struct {
    const char *s;
    size_t len;
    size_t pos;
    const cact_proposal_limits_t *lim;
    cact_proposal_t *out;
} pctx_t;

static bool p_eof(const pctx_t *c) { return c->pos >= c->len; }
static char p_peek(const pctx_t *c) { return c->s[c->pos]; }

static void p_ws(pctx_t *c)
{
    while (!p_eof(c)) {
        char ch = p_peek(c);
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') c->pos++;
        else break;
    }
}

static bool p_char(pctx_t *c, char ch)
{
    p_ws(c);
    if (!p_eof(c) && p_peek(c) == ch) { c->pos++; return true; }
    return false;
}

/* 字符串: 必须双引号; 不允许转义与 <0x20; 输出 NUL 结尾 */
static bool p_string(pctx_t *c, char *dst, size_t cap)
{
    p_ws(c);
    if (p_eof(c) || p_peek(c) != '"') return false;
    c->pos++;
    size_t o = 0;
    while (!p_eof(c)) {
        char ch = p_peek(c);
        if (ch == '"') {
            if (o >= cap) return false;
            dst[o] = '\0';
            c->pos++;
            return true;
        }
        if ((unsigned char)ch < 0x20 || ch == '\\') return false;
        if (o + 1 >= cap) return false;
        dst[o++] = ch;
        c->pos++;
    }
    return false;
}

/* 整数文法: -?[0-9]{1,18}; 成功写 out_v */
static bool p_int(pctx_t *c, int64_t *out_v)
{
    p_ws(c);
    size_t start = c->pos;
    bool neg = false;
    if (!p_eof(c) && p_peek(c) == '-') { neg = true; c->pos++; }
    size_t digits = 0;
    uint64_t v = 0;
    while (!p_eof(c) && p_peek(c) >= '0' && p_peek(c) <= '9') {
        if (digits >= 18) return false;
        v = v * 10 + (uint64_t)(p_peek(c) - '0');
        digits++;
        c->pos++;
    }
    if (digits == 0) { c->pos = start; return false; }
    *out_v = neg ? -(int64_t)v : (int64_t)v;
    return true;
}

/* confidence 文法: 0 | 1 | 0.xx | 1.000(x∈0,00 → 1000 之外拒绝);
 * 产出 milli 0..1000。仅此四种形状, 拒绝指数/长小数。 */
static bool p_confidence(pctx_t *c, int *milli)
{
    p_ws(c);
    size_t start = c->pos;
    if (p_eof(c) || (p_peek(c) != '0' && p_peek(c) != '1')) return false;
    int whole = p_peek(c) - '0';
    c->pos++;
    if (!p_eof(c) && p_peek(c) == '.') {
        c->pos++;
        int frac_digits = 0;
        int frac = 0;
        while (!p_eof(c) && p_peek(c) >= '0' && p_peek(c) <= '9') {
            if (frac_digits >= 3) return false;   /* >3 位小数: 拒绝 */
            frac = frac * 10 + (p_peek(c) - '0');
            frac_digits++;
            c->pos++;
        }
        if (frac_digits == 0) { c->pos = start; return false; }
        while (frac_digits < 3) { frac *= 10; frac_digits++; }
        if (whole == 0) { *milli = frac; return true; }
        *milli = 1000 + frac;                     /* 1.xxx → 上限外, 语义层拒 */
        return true;
    }
    /* 拒绝指数/后随字母(如 0.8e2): 由调用方核验下一 token 边界 */
    *milli = whole * 1000;
    return true;
}

/* confidence 后必须紧跟分隔符(}/,/ whitespace) —— 封死 0.8e2/0.8x */
static bool p_conf_end(pctx_t *c)
{
    if (p_eof(c)) return true;
    char ch = p_peek(c);
    return ch == ',' || ch == '}' || ch == ' ' || ch == '\t' ||
           ch == '\n' || ch == '\r';
}

/* ---- params: 一层对象 ---- */
static bool p_params(pctx_t *c)
{
    if (!p_char(c, '{')) return false;
    p_ws(c);
    if (p_char(c, '}')) return true;            /* 空 params 合法 */
    for (;;) {
        char key[CACT_PARAM_KEY_MAX];
        if (!p_string(c, key, sizeof(key))) return false;
        if (key[0] == '\0') return false;
        for (int i = 0; i < c->out->params_n; i++) {   /* 重复键拒绝 */
            if (strcmp(c->out->params[i].key, key) == 0) return false;
        }
        if (!p_char(c, ':')) return false;
        p_ws(c);
        if (c->out->params_n >= CACT_PROPOSAL_PARAMS_MAX) return false;
        int pidx = c->out->params_n;
        char vstr[CACT_PARAM_VAL_MAX];
        p_ws(c);
        if (!p_eof(c) && p_peek(c) == '"') {
            if (!p_string(c, vstr, sizeof(vstr))) return false;
        } else if (!p_eof(c) && (p_peek(c) == '-' ||
                   (p_peek(c) >= '0' && p_peek(c) <= '9'))) {
            int64_t v = 0;
            if (!p_int(c, &v)) return false;
            /* 数字后必须跟分隔符(封死 3e2/3.5 形状) */
            if (!p_eof(c)) {
                char ch = p_peek(c);
                if (!(ch == ',' || ch == '}' || ch == ' ' || ch == '\t' ||
                      ch == '\n' || ch == '\r')) {
                    return false;
                }
            }
            snprintf(vstr, sizeof(vstr), "%lld", (long long)v);
        } else if (!p_eof(c) && (strncmp(c->s + c->pos, "true", 4) == 0)) {
            snprintf(vstr, sizeof(vstr), "true");
            c->pos += 4;
        } else if (!p_eof(c) && (strncmp(c->s + c->pos, "false", 5) == 0)) {
            snprintf(vstr, sizeof(vstr), "false");
            c->pos += 5;
        } else {
            return false;                        /* null/嵌套/数组: 拒绝 */
        }
        snprintf(c->out->params[pidx].key,
                 sizeof(c->out->params[pidx].key), "%s", key);
        snprintf(c->out->params[pidx].val,
                 sizeof(c->out->params[pidx].val), "%s", vstr);
        c->out->params_n++;
        if (p_char(c, ',')) continue;
        return p_char(c, '}');
    }
}

/* ---- evidence: 字符串数组 ---- */
static bool p_evidence(pctx_t *c)
{
    if (!p_char(c, '[')) return false;
    p_ws(c);
    if (p_char(c, ']')) return true;
    for (;;) {
        char ref[CACT_EVIDENCE_LEN_MAX];
        if (!p_string(c, ref, sizeof(ref))) return false;
        if (ref[0] == '\0') return false;
        if (c->out->evidence_n >= CACT_EVIDENCE_MAX) return false;
        snprintf(c->out->evidence[c->out->evidence_n],
                 CACT_EVIDENCE_LEN_MAX, "%s", ref);
        c->out->evidence_n++;
        if (p_char(c, ',')) continue;
        return p_char(c, ']');
    }
}

/* ---- 顶层 ---- */
static bool act_id_legal(const char *s)
{
    if (s[0] == '\0') return false;
    for (const char *p = s; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '.' || *p == '_')) {
            return false;
        }
    }
    return true;
}

cact_res_t cact_proposal_parse(const char *text, size_t len,
                               const cact_proposal_limits_t *lim,
                               cact_proposal_t *out)
{
    if (text == NULL || out == NULL) return CACT_RES_INVALID;
    if (lim == NULL) lim = &CACT_PROPOSAL_LIMITS_DEF;
    if ((int64_t)len > lim->max_text_len) return CACT_RES_INVALID;

    memset(out, 0, sizeof(*out));
    out->confidence_milli = -1;

    pctx_t c = { .s = text, .len = len, .pos = 0, .lim = lim, .out = out };

    enum {
        K_NONE = 0, K_ACT, K_PARAMS, K_CONF, K_EVID, K_TTL, K_PERSONA,
    };
    unsigned seen = 0;

    if (!p_char(&c, '{')) return CACT_RES_INVALID;
    p_ws(&c);
    if (p_char(&c, '}')) return CACT_RES_INVALID;   /* 空对象: 缺 act */
    for (;;) {
        char key[24];
        if (!p_string(&c, key, sizeof(key))) return CACT_RES_INVALID;
        if (!p_char(&c, ':')) return CACT_RES_INVALID;

        unsigned bit;
        if (strcmp(key, "act") == 0) {
            bit = 1u << K_ACT;
            if (seen & bit) return CACT_RES_INVALID;
            if (!p_string(&c, out->act, sizeof(out->act))) return CACT_RES_INVALID;
            if (!act_id_legal(out->act)) return CACT_RES_INVALID;
        } else if (strcmp(key, "params") == 0) {
            bit = 1u << K_PARAMS;
            if (seen & bit) return CACT_RES_INVALID;
            if (!p_params(&c)) return CACT_RES_INVALID;
        } else if (strcmp(key, "confidence") == 0) {
            bit = 1u << K_CONF;
            if (seen & bit) return CACT_RES_INVALID;
            if (!p_confidence(&c, &out->confidence_milli)) return CACT_RES_INVALID;
            if (!p_conf_end(&c)) return CACT_RES_INVALID;
        } else if (strcmp(key, "evidence") == 0) {
            bit = 1u << K_EVID;
            if (seen & bit) return CACT_RES_INVALID;
            if (!p_evidence(&c)) return CACT_RES_INVALID;
        } else if (strcmp(key, "ttl_s") == 0) {
            bit = 1u << K_TTL;
            if (seen & bit) return CACT_RES_INVALID;
            if (!p_int(&c, &out->ttl_s)) return CACT_RES_INVALID;
            if (out->ttl_s < 1 || out->ttl_s > lim->max_ttl_s) {
                return CACT_RES_INVALID;            /* 必须为正且有界 */
            }
        } else if (strcmp(key, "persona") == 0) {
            bit = 1u << K_PERSONA;
            if (seen & bit) return CACT_RES_INVALID;
            int64_t v = 0;
            if (!p_int(&c, &v) || v < 0 || v > 0x7FFFFFFLL) {
                return CACT_RES_INVALID;
            }
            out->persona_epoch = (uint32_t)v;
        } else {
            return CACT_RES_INVALID;                /* 未知键: 白名单外 */
        }
        seen |= bit;

        if (p_char(&c, ',')) continue;
        break;
    }
    if (!p_char(&c, '}')) return CACT_RES_INVALID;
    p_ws(&c);
    if (!p_eof(&c)) return CACT_RES_INVALID;        /* 尾随垃圾 */
    if (!(seen & (1u << K_ACT))) return CACT_RES_INVALID;  /* act 必填 */
    if (out->confidence_milli > 1000) return CACT_RES_INVALID;
    return CACT_RES_OK;
}

bool cact_proposal_evidence_known(const cact_proposal_t *p,
                                  bool (*known)(const char *ref, void *ud),
                                  void *ud)
{
    if (p == NULL || known == NULL) return false;
    for (int i = 0; i < p->evidence_n; i++) {
        if (!known(p->evidence[i], ud)) return false;
    }
    return true;
}

cact_res_t cact_proposal_to_req(const cact_proposal_t *p, cact_origin_t origin,
                                int64_t now_ms, cact_req_t *req)
{
    if (p == NULL || req == NULL) return CACT_RES_INVALID;
    memset(req, 0, sizeof(*req));
    req->origin = origin;
    snprintf(req->capability, sizeof(req->capability), "%s", p->act);
    size_t off = 0;
    for (int i = 0; i < p->params_n; i++) {
        size_t kl = strlen(p->params[i].key);
        size_t vl = strlen(p->params[i].val);
        if (off + kl + 1 + vl + 1 > CACT_PARAMS_MAX) return CACT_RES_INVALID;
        memcpy(req->params + off, p->params[i].key, kl);
        off += kl;
        req->params[off++] = '\0';
        memcpy(req->params + off, p->params[i].val, vl);
        off += vl;
        req->params[off++] = '\0';
    }
    req->params_len = off;
    req->expires_ms = (p->ttl_s > 0) ? now_ms + p->ttl_s * 1000 : 0;
    req->persona_epoch = p->persona_epoch;
    return CACT_RES_OK;
}
