# 构建
在仓库根目录按 `docs/搭建指南.md` 的构建命令执行。需要 Git、Docker、Python 3 和网络下载。
`make -C xiaoai-plus/toolchain build-sherpa-static` 从源码编译带 KWS 观测和关闭线程自旋补丁的 sherpa 静态库；`compile` 编译 ARM Release；`package-release.sh` 打包。
离线测试用 `server/deploy/Dockerfile.host-tests` 和 `offline-tests.sh`；runtime 冒烟测试用 `make -C xiaoai-plus/toolchain runtime-smoke-test`。
不需要固件镜像或家庭安装清单。构建产物在忽略目录，源码不含 API Key。
