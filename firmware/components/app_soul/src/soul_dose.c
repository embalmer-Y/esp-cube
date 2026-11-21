/*
 * soul_dose.c —— 深思 appraisal 快通道(头注见 soul_dose.h)
 * 纯 C 零平台依赖; 唯一外部依赖 cJSON(宿主/板上同源编译)。
 */
#include "soul_dose.h"
#include "cJSON.h"

bool soul_appraisal_extract(const char *json, struct soul_appraisal *out)
{
    if (json == NULL || out == NULL)
        return false;
    cJSON *root = cJSON_Parse(json);
    if (root == NULL)
        return false;
    const cJSON *ap = cJSON_GetObjectItem(root, "appraisal");
    const cJSON *gc = ap != NULL ? cJSON_GetObjectItem(ap, "gc") : NULL;
    const cJSON *im = ap != NULL ? cJSON_GetObjectItem(ap, "imp") : NULL;
    const cJSON *vl = ap != NULL ? cJSON_GetObjectItem(ap, "val") : NULL;
    bool ok = cJSON_IsObject(ap) && cJSON_IsNumber(gc) &&
              cJSON_IsNumber(im) && cJSON_IsNumber(vl);
    if (ok) {
        out->gc = (float)gc->valuedouble;
        out->imp = (float)im->valuedouble;
        out->val = (float)vl->valuedouble;
        if (out->gc > 1.0f) out->gc = 1.0f;
        if (out->gc < -1.0f) out->gc = -1.0f;
        if (out->imp > 1.0f) out->imp = 1.0f;
        if (out->imp < 0.0f) out->imp = 0.0f;
        if (out->val > 0.2f) out->val = 0.2f;
        if (out->val < -0.2f) out->val = -0.2f;
    }
    cJSON_Delete(root);
    return ok;
}

void soul_dose_compute(const struct soul_appraisal *a, float *social,
                       float *ethics, float *val)
{
    float gc = a->gc, imp = a->imp, vl = a->val;
    /* 防御性再钳(调用侧绕过 extract 的直达路径同样有界) */
    if (gc > 1.0f) gc = 1.0f;
    if (gc < -1.0f) gc = -1.0f;
    if (imp > 1.0f) imp = 1.0f;
    if (imp < 0.0f) imp = 0.0f;
    if (vl > 0.2f) vl = 0.2f;
    if (vl < -0.2f) vl = -0.2f;
    if (social != NULL)
        *social = 0.15f * (gc > 0.0f ? gc : 0.0f) -
                  0.10f * (gc < 0.0f ? -gc : 0.0f);
    if (ethics != NULL)
        *ethics = 0.3f * imp * (gc < 0.0f ? -gc : 0.0f);
    if (val != NULL)
        *val = vl;
}
