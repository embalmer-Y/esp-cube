/*
 * ai_skills.c —— AI 技能注册表实现(纯注册/路由, 无业务依赖)
 */
#include "ai_skills.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "ai_skills";

typedef struct {
    char name[AI_SKILL_NAME_LEN];
    char desc[AI_SKILL_DESC_LEN];
    cJSON *schema;                     /* 注册时解析, 注销不释放(常驻) */
    ai_skill_fn fn;
} skill_slot_t;

static skill_slot_t s_skills[AI_SKILL_MAX];
static int s_count;

/* UTF-8 边界安全拷贝: 容量截断时回退到字符边界(截在续字节上会让
 * JSON 里的描述变成非法 UTF-8 → 服务端 400, 2026-11-11 板上实测) */
static void strlcpy_utf8(char *dst, const char *src, size_t cap)
{
    strlcpy(dst, src, cap);
    size_t len = strlen(dst);
    if (len == 0 || (unsigned char)src[len] == 0) {
        return;                            /* 未截断 */
    }
    while (len > 0 && ((unsigned char)dst[len - 1] & 0xC0) == 0x80) {
        len--;                             /* 回退续字节 */
    }
    if (len > 0 && ((unsigned char)dst[len - 1] & 0x80) != 0) {
        len--;                             /* 丢残缺首字节 */
    }
    dst[len] = 0;
}

esp_err_t ai_skill_register(const char *name, const char *description,
                            const char *schema_json, ai_skill_fn handler)
{
    if (name == NULL || name[0] == 0 || description == NULL ||
        handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 同名覆盖(幂等) */
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_skills[i].name, name) == 0) {
            strlcpy_utf8(s_skills[i].desc, description, sizeof(s_skills[i].desc));
            if (s_skills[i].schema != NULL) {
                cJSON_Delete(s_skills[i].schema);
            }
            s_skills[i].schema = (schema_json != NULL && schema_json[0] != 0)
                                 ? cJSON_Parse(schema_json) : NULL;
            s_skills[i].fn = handler;
            ESP_LOGI(TAG, "skill 更新: %s", name);
            return ESP_OK;
        }
    }
    if (s_count >= AI_SKILL_MAX) {
        ESP_LOGE(TAG, "注册表满(%d)", AI_SKILL_MAX);
        return ESP_ERR_NO_MEM;
    }
    skill_slot_t *s = &s_skills[s_count++];
    strlcpy(s->name, name, sizeof(s->name));
    strlcpy_utf8(s->desc, description, sizeof(s->desc));
    s->schema = (schema_json != NULL && schema_json[0] != 0)
                ? cJSON_Parse(schema_json) : NULL;
    if (schema_json != NULL && schema_json[0] != 0 && s->schema == NULL) {
        ESP_LOGW(TAG, "skill %s schema 解析失败(按无参处理)", name);
    }
    s->fn = handler;
    ESP_LOGI(TAG, "skill 注册: %s - %s", name, description);
    return ESP_OK;
}

cJSON *ai_skills_tools_json(void)
{
    if (s_count == 0) {
        return NULL;
    }
    cJSON *arr = cJSON_CreateArray();
    if (arr == NULL) {
        return NULL;
    }
    for (int i = 0; i < s_count; i++) {
        const skill_slot_t *s = &s_skills[i];
        cJSON *tool = cJSON_CreateObject();
        cJSON *fn = cJSON_CreateObject();
        if (tool == NULL || fn == NULL) {
            cJSON_Delete(tool);
            cJSON_Delete(fn);
            cJSON_Delete(arr);
            return NULL;
        }
        cJSON_AddStringToObject(tool, "type", "function");
        cJSON_AddStringToObject(fn, "name", s->name);
        cJSON_AddStringToObject(fn, "description", s->desc);
        if (s->schema != NULL) {
            cJSON_AddItemToObject(fn, "parameters",
                                  cJSON_Duplicate(s->schema, 1));
        } else {
            /* OpenAI 规范: 无参也建议给空 schema 对象 */
            cJSON_AddItemToObject(fn, "parameters", cJSON_CreateObject());
        }
        cJSON_AddItemToObject(tool, "function", fn);
        cJSON_AddItemToArray(arr, tool);
    }
    return arr;
}

int ai_skills_count(void)
{
    return s_count;
}

esp_err_t ai_skill_exec(const char *name, const cJSON *args,
                        char *reply, size_t reply_cap)
{
    if (reply == NULL || reply_cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_skills[i].name, name) == 0) {
            ESP_LOGI(TAG, "exec %s", name);
            return s_skills[i].fn(args, reply, reply_cap);
        }
    }
    snprintf(reply, reply_cap, "错误: 设备没有名为 %s 的技能", name);
    ESP_LOGW(TAG, "unknown skill: %s", name);
    return ESP_ERR_NOT_FOUND;
}
