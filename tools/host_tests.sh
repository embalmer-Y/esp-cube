#!/usr/bin/env bash
# host_tests.sh —— 宿主回归统一入口(docs/18 T02; 零设备/零网络/零凭据)
#
# 用法: tools/host_tests.sh
# 退出码 0 = 全绿。构建退出码独立检查(勿与运行混判, 项目纪律)。
set -u
cd "$(dirname "$0")/.."

FAIL=0
export ASAN_OPTIONS=detect_leaks=0   # soul_mem 按固件语义常驻不释放

echo "== [1/14] 黑板+行为评分器黄金回归(docs/14 §9) =="
gcc -DSOUL_BB_HOST_TEST -I firmware/components/app_soul/include \
    -o /tmp/test_soul_bh \
    firmware/components/app_soul/test/test_soul_behavior.c \
    firmware/components/app_soul/src/soul_bb.c \
    firmware/components/app_soul/src/soul_behavior.c -lm
if [ $? -ne 0 ]; then echo "BUILD FAIL: test_soul_bh"; FAIL=1;
else /tmp/test_soul_bh || FAIL=1; fi

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

echo "== [10/12] 凭据安全存储(docs/18 T17 设备侧; ASan+UBSan) =="
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

echo "== [11/13] LLM 服务商 profile(docs/18 T30; ASan+UBSan) =="
gcc -std=c11 -g -O1 -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter \
    -I firmware/components/llmprofile/include \
    -I firmware/managed_components/espressif__cjson/cJSON \
    -o /tmp/test_llm_profile \
    firmware/components/llmprofile/test/test_llm_profile.c \
    firmware/components/llmprofile/src/llm_profile.c \
    firmware/managed_components/espressif__cjson/cJSON/cJSON.c \
    -lm
if [ $? -ne 0 ]; then echo "BUILD FAIL: test_llm_profile"; FAIL=1;
else /tmp/test_llm_profile || FAIL=1; fi

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
    "$EFC_PY" tools/test_efc.py || EFC_FAIL=1
    "$EFC_PY" tools/efc_verify.py --contract firmware/components/app_ui/ui/assets/face \
        || { echo "FAIL: efc_verify contract"; EFC_FAIL=1; }
    "$EFC_PY" tools/test_face_expect.py || { echo "FAIL: test_face_expect"; EFC_FAIL=1; }
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
    echo "FAIL: EFC 编码器环境缺失: $EFC_PY"; EFC_FAIL=1
fi
if [ "$EFC_FAIL" -eq 0 ]; then echo "[18/18] EFC 部分: PASS"; else echo "[18/18] EFC 部分: FAIL"; FAIL=1; fi

echo "== [附] 分层依赖检查(docs/17 §2.2) =="
python3 tools/check_layers.py || FAIL=1

if [ "$FAIL" -eq 0 ]; then
    echo "host_tests: ALL PASS"
else
    echo "host_tests: FAILURES PRESENT"
fi
exit "$FAIL"
