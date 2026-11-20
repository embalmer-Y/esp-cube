#!/usr/bin/env bash
# host_tests.sh —— 宿主回归统一入口(docs/18 T02; 零设备/零网络/零凭据)
#
# 用法: tools/host_tests.sh
# 退出码 0 = 全绿。构建退出码独立检查(勿与运行混判, 项目纪律)。
set -u
cd "$(dirname "$0")/.."

FAIL=0
export ASAN_OPTIONS=detect_leaks=0   # soul_mem 按固件语义常驻不释放

echo "== [1/14] 黑板黄金回归(docs/14 §9; L2 评分器 S3 已退役, docs/24 §6/§13) =="
gcc -DSOUL_BB_HOST_TEST -I firmware/components/app_soul/include \
    -o /tmp/test_soul_bb \
    firmware/components/app_soul/test/test_soul_bb_host.c \
    firmware/components/app_soul/src/soul_bb.c -lm
if [ $? -ne 0 ]; then echo "BUILD FAIL: test_soul_bb"; FAIL=1;
else /tmp/test_soul_bb || FAIL=1; fi

echo "== [1b] ke/kd 情绪/驱力黄金回归(docs/24 S1; Unity 垫片; 锚=合同) =="
gcc -std=c11 -g -O1 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE \
    -I firmware/components/app_soul/test/host_stubs \
    -I firmware/components/app_soul/include \
    -o /tmp/test_soul_ke_kd \
    firmware/components/app_soul/test/test_soul_ke_kd_host.c \
    firmware/components/app_soul/test/test_soul_emotion.c \
    firmware/components/app_soul/test/test_soul_drive.c \
    firmware/components/app_soul/test/test_soul_trait.c \
    firmware/components/app_soul/test/test_soul_budget.c \
    firmware/components/app_soul/src/soul_emotion.c \
    firmware/components/app_soul/src/soul_drive.c \
    firmware/components/app_soul/src/soul_trait.c \
    firmware/components/app_soul/src/soul_budget.c -lm
if [ $? -ne 0 ]; then echo "BUILD FAIL: test_soul_ke_kd"; FAIL=1;
else
    /tmp/test_soul_ke_kd drive || FAIL=1
    /tmp/test_soul_ke_kd trait || FAIL=1
    /tmp/test_soul_ke_kd budget || FAIL=1
    # S2 已重锚(docs/24 §2 三层栈新契约, 16 测试全绿)——此数归零;
    # 锚=合同: ke/kd 任何语义变更后失败数变化(无论增减)须显式更新此数。
    EMOT_KNOWN_FAIL=0
    /tmp/test_soul_ke_kd emotion
    rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "emotion: 全绿(旧锚已修复, 请将 EMOT_KNOWN_FAIL 置 0)"
    elif [ "$rc" -le "$EMOT_KNOWN_FAIL" ]; then
        echo "emotion: KNOWN-FAIL($rc/$EMOT_KNOWN_FAIL, docs/24 S1 挂账) — 不阻塞"
    else
        echo "emotion: FAIL($rc > 已知 $EMOT_KNOWN_FAIL) — 新增失守, 锚漂移违规"
        FAIL=1
    fi
fi

echo "== [1c] R3 表达区分度验收(docs/24 R3/S1; soul_sim 三场景月测; 目标态) =="
python3 -I tools/soul_accept.py
rc=$?
# S2+S2.5b 批全部转绿(2026-11-20: 11 判据 C1a-C5c 全 PASS, §55.22);
# 判据由绿转红即回归。此数>0 表示仍有挂账判据。
R3_KNOWN=0
if [ "$rc" -eq 0 ]; then
    echo "R3: 全达标(请将 R3_KNOWN 置 0)"
elif [ "$rc" -le "$R3_KNOWN" ]; then
    echo "R3: KNOWN-FAIL($rc/$R3_KNOWN, v2 实施中) — 不阻塞"
else
    echo "R3: FAIL($rc > 已知 $R3_KNOWN) — 回归"
    FAIL=1
fi

echo "== [2/14] 记忆系统 P0 回归(docs/15 P0-1/P0-2/M1/M4/M5; ASan+UBSan) =="
gcc -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I firmware/components/app_soul/test/host_stubs \
    -I firmware/components/app_soul/include \
    -I firmware/components/app_storage/include \
    -o /tmp/test_soul_mem \
    firmware/components/app_soul/test/test_soul_mem.c \
    firmware/components/app_soul/src/soul_mem.c
if [ $? -ne 0 ]; then echo "BUILD FAIL: test_soul_mem"; FAIL=1;
else /tmp/test_soul_mem || FAIL=1; fi

echo "== [3/14] 统一动作骨架(docs/18 T09/T10; ASan+UBSan) =="
gcc -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Wall -Wextra -Wno-unused-parameter \
    -I firmware/components/cube_act/include \
    -o /tmp/test_cube_act \
    firmware/components/cube_act/test/test_cube_act.c \
    firmware/components/cube_act/src/cube_act_core.c
if [ $? -ne 0 ]; then echo "BUILD FAIL: test_cube_act"; FAIL=1;
else /tmp/test_cube_act || FAIL=1; fi

echo "== [4/14] 双槽可恢复提交协议(docs/18 T07; ASan+UBSan) =="
gcc -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Wall -Wextra -Wno-unused-parameter \
    -I firmware/components/app_storage/include \
    -o /tmp/test_store_slot \
    firmware/components/app_storage/test/test_store_slot.c \
    firmware/components/app_storage/src/store_slot.c
if [ $? -ne 0 ]; then echo "BUILD FAIL: test_store_slot"; FAIL=1;
else /tmp/test_store_slot || FAIL=1; fi

echo "== [5/14] LLM 提案严格解析(docs/18 T11; ASan+UBSan) =="
gcc -std=c11 -g -O1 -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter \
    -I firmware/components/cube_act/include \
    -o /tmp/test_cact_proposal \
    firmware/components/cube_act/test/test_cact_proposal.c \
    firmware/components/cube_act/src/cact_proposal.c \
    firmware/components/cube_act/src/cube_act_core.c
if [ $? -ne 0 ]; then echo "BUILD FAIL: test_cact_proposal"; FAIL=1;
else /tmp/test_cact_proposal || FAIL=1; fi

echo "== [6/14] 人格服务骨架(docs/18 T19; ASan+UBSan) =="
gcc -std=c11 -g -O1 -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter \
    -I firmware/components/persona/include \
    -I firmware/components/app_storage/include \
    -o /tmp/test_persona \
    firmware/components/persona/test/test_persona.c \
    firmware/components/persona/src/persona_core.c \
    firmware/components/app_storage/src/store_slot.c
if [ $? -ne 0 ]; then echo "BUILD FAIL: test_persona"; FAIL=1;
else /tmp/test_persona || FAIL=1; fi

echo "== [7/14] 人格持久化(docs/18 T20 宿主部分; ASan+UBSan) =="
gcc -std=c11 -g -O1 -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter \
    -I firmware/components/persona/include \
    -I firmware/components/app_storage/include \
    -o /tmp/test_persona_store \
    firmware/components/persona/test/test_persona_store.c \
    firmware/components/persona/src/persona_core.c \
    firmware/components/app_storage/src/store_slot.c
if [ $? -ne 0 ]; then echo "BUILD FAIL: test_persona_store"; FAIL=1;
else /tmp/test_persona_store || FAIL=1; fi

echo "== [8/14] 信任区+EPSCV2 容器(docs/18 T14; ASan+UBSan) =="
gcc -std=c11 -g -O1 -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter \
    -I firmware/components/app_storage/include \
    -I firmware/components/app_soul/test/host_stubs \
    -o /tmp/test_trust_crypt \
    firmware/components/app_storage/test/test_trust_crypt.c \
    firmware/components/app_storage/src/trust.c \
    firmware/components/app_storage/src/store_crypt.c
if [ $? -ne 0 ]; then echo "BUILD FAIL: test_trust_crypt"; FAIL=1;
else /tmp/test_trust_crypt || FAIL=1; fi

echo "== [9/14] SRP-6a+配对服务(docs/18 T15 首段; ASan+UBSan) =="
gcc -std=c11 -g -O1 -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter \
    -I firmware/components/pair/include \
    -I firmware/components/pair/src \
    -I firmware/components/app_soul/test/host_stubs \
    -o /tmp/test_srp_pair \
    firmware/components/pair/test/test_srp_pair.c \
    firmware/components/pair/src/ubn.c \
    firmware/components/pair/src/srp6a.c \
    firmware/components/pair/src/pair_svc.c
if [ $? -ne 0 ]; then echo "BUILD FAIL: test_srp_pair"; FAIL=1;
else /tmp/test_srp_pair || FAIL=1; fi

echo "== [10/14] 凭据安全存储(docs/18 T17 设备侧; ASan+UBSan) =="
gcc -std=c11 -g -O1 -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter \
    -I firmware/components/credstore/include \
    -I firmware/components/app_storage/include \
    -I firmware/components/app_soul/test/host_stubs \
    -o /tmp/test_credstore \
    firmware/components/credstore/test/test_credstore.c \
    firmware/components/credstore/src/credstore.c \
    firmware/components/app_storage/src/store_crypt.c
if [ $? -ne 0 ]; then echo "BUILD FAIL: test_credstore"; FAIL=1;
else /tmp/test_credstore || FAIL=1; fi

echo "== [11/14] LLM 服务商 profile(docs/18 T30; ASan+UBSan) =="
CJSON=firmware/managed_components/espressif__cjson/cJSON
if [ ! -f "$CJSON/cJSON.c" ]; then
    # CI/无 IDF 环境: managed_components 构建期才拉取(不入库) → 跳过
    echo "SKIP: $CJSON/cJSON.c 不在(managed_components 未拉取; 本地 idf.py build 后自动可用)"
else
    gcc -std=c11 -g -O1 -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter \
        -I firmware/components/llmprofile/include \
        -I "$CJSON" \
        -o /tmp/test_llm_profile \
        firmware/components/llmprofile/test/test_llm_profile.c \
        firmware/components/llmprofile/src/llm_profile.c \
        "$CJSON/cJSON.c" \
        -lm
    if [ $? -ne 0 ]; then echo "BUILD FAIL: test_llm_profile"; FAIL=1;
    else /tmp/test_llm_profile || FAIL=1; fi
fi

echo "== [12/14] 会话守卫(docs/18 T31 首段; ASan+UBSan) =="
gcc -std=c11 -g -O1 -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter \
    -I firmware/components/sessguard/include \
    -o /tmp/test_sessguard \
    firmware/components/sessguard/test/test_sessguard.c \
    firmware/components/sessguard/src/sessguard.c
if [ $? -ne 0 ]; then echo "BUILD FAIL: test_sessguard"; FAIL=1;
else /tmp/test_sessguard || FAIL=1; fi

echo "== [13/14] 模型评估线束 selftest(docs/18 T33; mock 适配器) =="
python3 -I tools/modeval.py selftest || FAIL=1

echo "== [13b] soul_calib 校准层 selftest(docs/24 §5.3 D3; S2.5b) =="
python3 -I tools/soul_calib.py selftest || FAIL=1

echo "== [14/14] 会话生命周期状态机(docs/18 T28 宿主语义; ASan+UBSan) =="
gcc -std=c11 -g -O1 -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter \
    -I firmware/components/sessguard/include \
    -o /tmp/test_sessstate \
    firmware/components/sessguard/test/test_sessstate.c \
    firmware/components/sessguard/src/sessstate.c
if [ $? -ne 0 ]; then echo "BUILD FAIL: test_sessstate"; FAIL=1;
else /tmp/test_sessstate || FAIL=1; fi

echo "== [附] freeze_watch selftest(freeze-and-face-blackscreen-fix 任务 11; 分类重放 + PBT + 串口独占) =="
python3 tools/test_freeze_watch.py || { echo "FAIL: freeze_watch selftest"; FAIL=1; }

# EFC 部分(任务 30.3; Property 8): 往返、键控语义、可复现来源, 以及契约检查。
# 表情纯 C 部分(任务 31.2): alpha0/lit 宿主测试(ASan+UBSan), 期望值与 MANIFEST 的核对。
# 统计件(任务 4.1): face_perf_stats 宿主测试。
# 固定使用 EFC 编码器环境 ~/.venvs/efc(Pillow 13.0.0, numpy 2.5.4), 不用 IDF python3。
echo "== [18/18] EFC 部分(任务 30.3; Property 8) + 表情纯 C(任务 31.2) + 统计件(任务 4.1) =="
EFC_FAIL=0
EFC_PY="$HOME/.venvs/efc/bin/python"
if [ -x "$EFC_PY" ]; then
    if ls firmware/components/app_ui/ui/assets/face/*.efc >/dev/null 2>&1; then
        "$EFC_PY" tools/test_efc.py || EFC_FAIL=1
        "$EFC_PY" tools/efc_verify.py --contract firmware/components/app_ui/ui/assets/face \
            || { echo "FAIL: efc_verify contract"; EFC_FAIL=1; }
        "$EFC_PY" tools/test_face_expect.py || { echo "FAIL: test_face_expect"; EFC_FAIL=1; }
    else
        echo "素材不在库(§55.20 出库裁决): efc 契约/期望测试跳过; 见 tools/fetch_face_assets.sh"
    fi
    gcc -std=c11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
        -fno-omit-frame-pointer -ffp-contract=off -Wall -Wextra -Wno-unused-parameter \
        -I firmware/components/app_ui/ui -o /tmp/test_ui_face_fmt \
        firmware/main/test/test_ui_face_fmt.c
    if [ $? -ne 0 ]; then echo "BUILD FAIL: test_ui_face_fmt"; EFC_FAIL=1;
    else /tmp/test_ui_face_fmt || EFC_FAIL=1; fi
    gcc -std=c11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
        -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter \
        -I firmware/main -o /tmp/test_face_perf_stats \
        firmware/main/test/test_face_perf_stats.c firmware/main/face_perf_stats.c
    if [ $? -ne 0 ]; then echo "BUILD FAIL: test_face_perf_stats"; EFC_FAIL=1;
    else /tmp/test_face_perf_stats || EFC_FAIL=1; fi
else
    if ls firmware/components/app_ui/ui/assets/face/*.efc >/dev/null 2>&1; then
        echo "FAIL: EFC 编码器环境缺失(素材在库须可验): $EFC_PY"; EFC_FAIL=1
    else
        echo "SKIP: EFC 编码器环境缺失且素材已出库(§55.20) — efc 契约测试无处可施"
    fi
fi
if [ "$EFC_FAIL" -eq 0 ]; then echo "[18/18] EFC 部分: PASS"; else echo "[18/18] EFC 部分: FAIL"; FAIL=1; fi

echo "== [附] 分层依赖检查(docs/17 §2.2) =="
python3 tools/check_layers.py || FAIL=1

echo "== [附] 内存预算 map 校验(docs/22 §7; 需先构建双目标) =="
python3 -I tools/mem_budget_check.py || FAIL=1

if [ "$FAIL" -eq 0 ]; then
    echo "host_tests: ALL PASS"
else
    echo "host_tests: FAILURES PRESENT"
fi
exit "$FAIL"
