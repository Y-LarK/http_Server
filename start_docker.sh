#!/bin/bash
# 进入挂载的工作目录
cd /workspace

# 自动创建 build 目录（如果不存在）
# 这是为了确保 clangd 查找 compile-commands-dir 不会报错
if [ ! -d "build" ]; then
    mkdir -p build
    echo "Created build directory."
fi

echo "Starting clangd remote bridge on port 9527..."
# 启动 socat 监听 9527 端口并转发给 clangd
# reuseaddr: 允许快速重启容器而不会因端口占用报错
# fork: 允许多个 IDE 实例（如多个窗口）同时连接
socat TCP-LISTEN:9527,reuseaddr,fork \
  "EXEC:clangd --compile-commands-dir=/workspace/build --query-driver=/usr/bin/g++ --log=verbose" &

echo "HTTP Server Dev Container is ready."
echo "Remote LSP: localhost:9527"
echo "HTTP Server Port: 8080"

# 保持容器运行
tail -f /dev/null
