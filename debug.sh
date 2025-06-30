#!/bin/bash

while true; do
    # 启动 QEMU 并等待 GDB 连接
    make qemu-gdb &
    QEMU_PID=$!
    
    # 启动 GDB 并执行调试命令
    gdb-multiarch -ex "target remote localhost:26000" \
                  -ex "file kernel/kernel" \
                  -ex "b sys_symlink" \
                  -ex "b sys_unlink" \
                  -ex "b panic" \
                  -ex "c" \
                  kernel/kernel
    
    # QEMU 退出后清理
    kill $QEMU_PID
    sleep 1
    echo "========================================"
    echo " Kernel panic detected, restarting debug"
    echo "========================================"
done