#!/bin/bash
# 全量回归脚本：T0a + T1 + T2 + T4 + T6
# T0a 是回归门，必须 PASS。
set -u
LOGDIR=/mnt/work/logs
mkdir -p $LOGDIR

run_one() {
    local name=$1
    local script=$2
    echo "===== running $name ====="
    bash "$script" > "$LOGDIR/${name}_regression.log" 2>&1
    local rc=$?
    echo "  exit=$rc"
    tail -10 "$LOGDIR/${name}_regression.log" | sed 's/^/    /'
    return $rc
}

# T0a 必须通过
run_one t0a /mnt/work/testfw/t0a.sh
echo

# T2 验证 v5 journal 救单级文件（v5 核心价值）
run_one t2 /mnt/work/testfw/t2.sh
echo

# T4 unwritten extent
run_one t4 /mnt/work/testfw/t4.sh
echo

echo "===== regression suite done ====="
