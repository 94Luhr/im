# Linux epoll 服务端

> 项目总览、架构图、性能摘要和文档导航请从根目录 [README.md](README.md) 开始；本文保留Linux服务端的详细运行说明。

本目录仅保留 Linux `epoll` 服务端；Windows Qt 客户端位于独立项目中。

```bash
sudo apt install build-essential cmake libssl-dev default-libmysqlclient-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/im_server_linux
```

服务监听 TCP `56789`，帧格式与现有 Windows 客户端一致：`uint32_t` 网络序长度 + 原协议结构体。

网络层在 `net/EpollTcpServer.*`：一个 `epoll` I/O Reactor 线程管理所有连接，`eventfd` 将业务线程的发送请求安全地唤醒并交回该线程；数量可配置的 worker 处理完整协议帧，并保持同一 TCP 连接内的业务帧顺序。每个连接的待发送缓存上限为 8 MiB，超过时断开慢客户端，避免全局内存被耗尽。

每次接受连接都会生成单调递增且不复用的 `connectionId`。业务任务和发送请求只携带该编号，Reactor 在实际发送前再解析当前 fd，因此旧任务不能误发到复用了相同 fd 的新连接。在线状态同时维护用户到连接、连接到用户的双向映射。

连接保活使用应用层心跳：Windows Qt 客户端每 30 秒发送心跳，服务端通过注册到 `epoll` 的 `timerfd` 每 10 秒检查连接，90 秒无数据则主动断开。客户端断线后每 3 秒重连，成功后恢复心跳及本次进程内的登录状态。

聊天消息会先写入 `t_message`：在线用户实时接收，离线用户登录后补发并返回 ACK；客户端首次打开聊天窗口时加载最近 30 条历史记录。

聊天窗口支持使用消息ID游标继续加载更早记录；好友申请会写入 `t_friend_request`，即使目标用户离线，也会在其下次登录时重新投递并等待同意或拒绝。同意或拒绝结果同样采用持久化通知：申请者离线时在下次登录补发，Windows客户端展示后发送ACK，服务端才记录送达时间。

数据库通过环境变量配置：`IM_DB_HOST`（默认 `127.0.0.1`）、`IM_DB_USER`（默认 `imserver`）、`IM_DB_PASSWORD`（默认空）与 `IM_DB_NAME`（默认 `imdb`）。首次部署时执行 `sudo mysql < database/schema.sql` 创建库表。

从旧版数据库升级时，需要执行好友结果通知迁移：

```bash
mysql -u imserver -p imdb < database/migrate_friend_result_notification.sql
```

密码安全升级需要先扩展密码字段：

```bash
mysql -u imserver -p imdb < database/migrate_password_security.sql
```

新注册账号使用随机16字节盐和120000轮PBKDF2-HMAC-SHA256。已有64位SHA-256密码仍可正常登录，并会在首次验证成功后自动升级。连续5次凭据错误会将对应账号摘要限流60秒，限流状态不会保存或输出原始手机号。

高并发参数可通过环境变量调整：`IM_DB_POOL_SIZE`（默认 4）、`IM_WORKER_THREADS`（默认 4）和 `IM_MAX_PENDING_JOBS`（默认 10000）。连接池和业务线程数允许 1 至 64，任务上限允许 1 至 1000000；非法配置会回退到默认值。

关键运行日志采用一行一个 JSON 对象的格式，并每 30 秒输出一次 `server_metrics`。指标包括连接数、累计收发包和字节、待处理任务、队列拒绝次数、数据库连接池占用及数据库操作成功失败情况，便于通过 `journalctl`、ELK 或脚本采集。核心业务层的普通调试输出已经全部迁移为结构化事件。

Prometheus指标端点默认监听 `0.0.0.0:9108`，避免与Prometheus自身默认端口冲突。端点由同一个 `epoll` Reactor以非阻塞方式处理，监控抓取连接不会计入IM业务连接数。端口可通过 `IM_METRICS_PORT` 调整：

```bash
curl http://127.0.0.1:9108/metrics
```

`monitoring/prometheus.yml` 可用于Linux原生Prometheus进程。也可以使用 `monitoring/compose.yaml` 一键启动Prometheus与Grafana：容器会自动连接数据源并加载中文仪表盘。详细命令见 `monitoring/README.md`。

`tests/load_test.py` 提供无第三方依赖的 `asyncio` 心跳压测，可批量建立连接、模拟粘包和半包，并生成吞吐量、成功率和P50/P95/P99延迟报告。`tests/business_load_test.py` 用独立测试账号覆盖登录、好友校验、消息持久化、离线补发和ACK链路。`tests/run_integration_test.sh` 可一键回归登录、离线消息、历史分页、离线好友申请和可靠ACK语义。具体命令见 `tests/README.md`，1至5000连接的实测数据见 `docs/性能测试报告.md`。

每次功能迭代的实现内容、涉及文件和验证方法记录在 `docs/功能实现记录.md`。

生产部署可使用 `deploy/im-server.service`，由独立低权限用户运行并通过 `systemd` 实现开机启动、异常重启、文件描述符上限与优雅停止；具体安装命令见 `deploy/README.md`。GitHub Actions配置位于 `.github/workflows/ci.yml`，会在MySQL 8环境中自动完成Release构建和核心业务集成测试。
