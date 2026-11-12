/*
 * cube_event_bus.h —— 轻量级进程内事件总线(发布/订阅)
 *
 * 特性:
 *   - 订阅回调在【发布者任务上下文】同步执行, 回调必须短小、不得阻塞;
 *   - payload 指针仅在回调执行期间有效, 订阅者如需持有必须自行拷贝;
 *   - 线程安全(spinlock 保护订阅列表与派发过程)。
 *
 * 使用约定(见设计文档 §2 依赖原则):
 *   - UI 订阅音频/AI 事件; AI 订阅唤醒事件; —— 全部解耦。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 全局事件 ID 定义(模块间唯一通信词汇表, 新事件必须登记在此) ---- */
typedef enum {
    CUBE_EVT_NONE = 0,
    /* 网络 */
    CUBE_EVT_WIFI_CONNECTED,
    CUBE_EVT_WIFI_DISCONNECTED,
    /* 蓝牙 */
    CUBE_EVT_BT_CONNECTED,
    CUBE_EVT_BT_DISCONNECTED,
    CUBE_EVT_BT_META,        /* AVRCP 元数据, payload: cube_bt_meta_t */
    CUBE_EVT_BT_PLAY_STATE,  /* payload: bool(是否播放中) */
    CUBE_EVT_BT_AUDIO_ACTIVE, /* BT音频流入=true(首次数据) / A2DP断开=false, payload: bool */
    /* 播放器 */
    CUBE_EVT_PLAYER_STATE,   /* payload: int(app_player_state_t) */
    CUBE_EVT_STREAM_TITLE,   /* ICY StreamTitle, payload: char*(临时) */
    /* 音频 */
    CUBE_EVT_VOLUME_CHANGED, /* payload: int(0-100) */
    /* 语音 AI */
    CUBE_EVT_WAKE_DETECTED,  /* 唤醒词命中 */
    CUBE_EVT_VOICE_CMD,      /* 本地命令词, payload: int(app_voice_cmd_t) */
    CUBE_EVT_AI_STATE,       /* AI 会话状态, payload: int(ai_state_t) */
    CUBE_EVT_AI_TEXT,        /* AI 文本(字幕/表情), payload: char*(临时) */
    CUBE_EVT_AI_TOOL_HINT,   /* 工具参数流式提示(speculative 预取),
                                payload: cube_ai_tool_hint_t(临时) */
    /* 助理服务(app_assist) */
    CUBE_EVT_TIME_SYNCED,    /* SNTP 首次校时成功, payload: NULL */
    CUBE_EVT_ASSIST_CHANGED, /* 琐事/提醒增删/到点移除, payload: NULL(UI 重拉) */
    CUBE_EVT_REMINDER_FIRED, /* 提醒到点, payload: cube_reminder_t(临时) */
    /* 灵魂(app_soul) */
    CUBE_EVT_SOUL_MOOD,      /* 情绪快照(节流: anim_tag 变化或 60s) */
    /* 存储(app_storage) */
    CUBE_EVT_SD_READY,       /* SD 卡挂载成功(记忆/素材切 SD 后端), payload: NULL */
} cube_evt_id_t;

/* 提醒到点(CUBE_EVT_REMINDER_FIRED payload; 与 app_assist 存储解耦,
 * UI/日志只消费此快照) */
typedef struct {
    uint32_t id;
    int64_t  due;            /* unix 秒 */
    char     text[128];
} cube_reminder_t;

/* 情绪快照(CUBE_EVT_SOUL_MOOD payload; anim_tag 词表: happy/relaxed/
 * angry/sad/surprised/confused/sleepy —— valence×arousal 平面查表) */
typedef struct {
    char  anim_tag[16];
    float valence;           /* [-1,1] */
    float arousal;           /* [0,1] */
    float urgency;           /* [0,1] */
    float social_bond;       /* [0,1] 互动强度积分 */
    bool  ethics_latched;    /* 安抚模式(只允许温柔语气) */
} cube_soul_mood_t;

/* AVRCP 曲目元数据(蓝牙播放页显示用) */
typedef struct {
    char title[64];
    char artist[64];
    char album[64];
} cube_bt_meta_t;

/* AI 会话状态(CUBE_EVT_AI_STATE payload)。定义在总线侧: app_voice 订阅
 * 此事件做麦克风所有权切换(docs/06 §3), 免依赖 app_ai_session 头。 */
typedef enum {
    AI_IDLE = 0,
    AI_WAKE,
    AI_LISTENING,
    AI_THINKING,
    AI_SPEAKING,
} ai_state_t;

/* 工具参数流式提示(CUBE_EVT_AI_TOOL_HINT payload, 同步派发生命周期):
 * LLM 吐 tool_calls 的 arguments 期间按 delta 推送累计值, 供技能层
 * speculative 预取(搜歌抢跑等)。args 为截断的累计 JSON(非完整)。 */
typedef struct {
    char name[48];
    char args[256];
} cube_ai_tool_hint_t;

/* 事件回调原型: data 生命周期仅限回调期间 */
typedef void (*cube_evt_cb_t)(cube_evt_id_t id, const void *data, size_t len, void *user_ctx);

/* 订阅事件(全量订阅: 简单优先, 事件量级低频, 无需按 ID 过滤) */
esp_err_t cube_event_subscribe(cube_evt_cb_t cb, void *user_ctx);
esp_err_t cube_event_unsubscribe(cube_evt_cb_t cb);

/* 发布事件(同步派发; data 可为 NULL) */
esp_err_t cube_event_publish(cube_evt_id_t id, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
