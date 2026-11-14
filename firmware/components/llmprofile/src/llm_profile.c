/* llm_profile.c —— 服务商 profile 解析/选择实现(见 llm_profile.h; T30)
 *
 * JSON 解析用 cJSON(ESP-IDF espressif__cjson 同源, 宿主测试直接编译
 * cJSON.c); 解析为一次性只读遍历, 无动态保留(出参为值拷贝)。
 */
#include <string.h>
#include <stdio.h>

#include "cJSON.h"
#include "llm_profile.h"

static void cp_str(const cJSON *obj, const char *key, char *out, size_t cap)
{
    const cJSON *it = cJSON_GetObjectItem(obj, key);
    if (it != NULL && cJSON_IsString(it) && it->valuestring != NULL) {
        strncpy(out, it->valuestring, cap - 1);
        out[cap - 1] = '\0';
    } else {
        out[0] = '\0';
    }
}

int llm_profile_validate(const llm_profile_t *p)
{
    if (p == NULL) return -1;
    size_t il = 0;
    while (il < LP_ID_MAX && p->id[il] != '\0') il++;
    if (il == 0 || il >= LP_ID_MAX) return -2;          /* id 缺失/超长 */
    if (strncmp(p->llm_url, "http://", 7) != 0 &&
        strncmp(p->llm_url, "https://", 8) != 0) return -3;
    if (p->llm_model[0] == '\0') return -4;
    return 0;
}

int llm_profiles_parse(const char *json, llm_profile_t out[], int max,
                       char active_id[LP_ID_MAX])
{
    if (json == NULL || out == NULL || max <= 0 || active_id == NULL) {
        return -1;
    }
    active_id[0] = '\0';
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return -1;                        /* 坏 JSON */
    int n = 0;
    cp_str(root, "active", active_id, LP_ID_MAX);
    const cJSON *arr = cJSON_GetObjectItem(root, "profiles");
    if (arr != NULL && cJSON_IsArray(arr)) {
        const cJSON *it = NULL;
        cJSON_ArrayForEach(it, arr) {
            if (!cJSON_IsObject(it)) continue;
            llm_profile_t p;
            memset(&p, 0, sizeof(p));
            cp_str(it, "id", p.id, sizeof(p.id));
            cp_str(it, "llm_url", p.llm_url, sizeof(p.llm_url));
            cp_str(it, "llm_model", p.llm_model, sizeof(p.llm_model));
            cp_str(it, "tts_model", p.tts_model, sizeof(p.tts_model));
            cp_str(it, "tts_voice", p.tts_voice, sizeof(p.tts_voice));
            const cJSON *tl = cJSON_GetObjectItem(it, "tools");
            p.tools = (tl == NULL || !cJSON_IsBool(tl)) ? true
                      : cJSON_IsTrue(tl);
            if (llm_profile_validate(&p) != 0) continue;   /* 剔除无效条目 */
            if (n < max) {
                out[n++] = p;
            }
        }
    }
    cJSON_Delete(root);
    return n;
}

int llm_profiles_pick(const llm_profile_t *list, int n,
                      const char *active_id)
{
    if (list == NULL || n <= 0) return -1;
    if (active_id != NULL && active_id[0] != '\0') {
        for (int i = 0; i < n; i++) {
            if (strcmp(list[i].id, active_id) == 0) return i;
        }
        /* active 无效 → 回退首个有效(H+S0 回退语义) */
    }
    return 0;
}
