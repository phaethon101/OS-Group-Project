#!/bin/bash
# 自动：后台起 QEMU(make debug) -> GDB 批处理采集 -> 清理 -> 拷出结果
set +e
cd /home/xzh58/oslab/lab1/code || exit 1

pkill -f qemu-system-riscv64
sleep 1

# 后台启动调试 QEMU（-S 冻结，等待 gdb）
make debug > /tmp/qemu_debug.log 2>&1 &
QPID=$!
sleep 6

echo "QEMU_PROCS:"
pgrep -a qemu-system-riscv64

echo "RUN_GDB:"
riscv64-unknown-elf-gdb -batch -x gdb_trace.gdb > /tmp/gdb_out.txt 2>&1
echo "GDB_EXIT=$?"

# 清理
kill "$QPID" 2>/dev/null
pkill -f qemu-system-riscv64

cp /tmp/gdb_out.txt /mnt/c/Users/xzh58/AppData/Local/Temp/opencode/gdb_out.txt
echo "FINISHED"
wc -l /tmp/gdb_out.txt
