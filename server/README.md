# 可选记忆后台与家长页
Python 3.10+、SQLite；HTTPS 反向代理。语音直连火山，后台保存文本记录和档案，生成摘要和可选企业微信日报。
部署及家庭初始化见 `../docs/搭建指南.md`。`deploy/install.sh --execute-on-authorized-server` 需要 root，安装服务但不启动；已有安装应先备份。
配置环境变量：`MEMORY_DB`、`MEMORY_DATA_KEY_FILE`（32 字节密钥的十六进制文本文件）、`MEMORY_ORIGIN`（家长页 HTTPS origin）。私密文件权限 0600；不要提交数据库、密钥、设备凭证或环境文件。
家长密码由 `python -m server.manage provision` 交互输入；`--child Amy --child Ben` 为每名孩子创建空档案，昵称须与音箱 `child_names` 相同，年级和兴趣等在家长页填写。
摘要模型配置文件为 `/etc/xiaoai-memory/ark.env`（设置命令：`deploy/admin.sh set-ark-key`）；摘要与身份模型调用会计费。详见 `ark_setup.py`、`ark_summary.py` 的当前配置项。
可选企业微信：`WECOM_WEBHOOK_FILE`、`WECOM_HOUSEHOLD`、`WECOM_DAILY_TIME`；`PARENT_URL` 或 `PARENT_URL_FILE` 配置家长页网址，为空时日报不附链接。
`deploy/xiaoai-memory-backup.*` 定时加密备份，`backup-key.py` 管理备份密钥；`verify-backup.sh` 在临时目录恢复并验证 SQLite 完整性。
本地测试：在仓库根目录建立 venv，安装 requirements-dev.txt，再执行 `python -m pytest server/tests`；测试使用合成家庭和模拟 HTTP，不调用付费服务。
