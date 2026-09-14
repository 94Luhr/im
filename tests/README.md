# 并发压测工具

`load_test.py` 仅依赖 Python 3 标准库，会建立大量 TCP 连接并按照服务端协议发送心跳。默认将两个心跳帧合并到一次写入，并随机拆分 10% 的写入，用于同时覆盖正常包、粘包和半包。

先启动服务端，再在另一个 Linux 终端执行：

```bash
cd /mnt/hgfs/linuxgongxiang/imServer
python3 tests/load_test.py \
  --host 127.0.0.1 \
  --connections 1000 \
  --duration 30 \
  --interval 1 \
  --burst 2 \
  --fragment-ratio 0.1 \
  --output load-test-report.json
```

逐级提高连接数时，先确认当前用户的文件描述符限制：

```bash
ulimit -n
```

如果限制小于目标连接数，可仅对当前终端临时提高：

```bash
ulimit -n 65535
```

建议依次测试 100、1000、5000 个连接，不要第一次就使用极端参数。报告中的 `response_success_percent` 应接近 100%，重点记录 `responses_per_second` 与 `latency_ms.p99`，并和服务端每30秒输出的 `server_metrics` 对照。

示例高吞吐测试：

```bash
python3 tests/load_test.py --connections 1000 --duration 30 --interval 0.1 --burst 5
```

只验证半包处理：

```bash
python3 tests/load_test.py --connections 100 --duration 10 --fragment-ratio 1 --burst 1
```

## 数据库业务压测

`business_load_test.py` 使用独立账号对验证真实业务链路：发送者登录、好友关系校验、消息写入MySQL、接收者离线登录补发，以及客户端ACK更新送达状态。测试账号仅使用 `load_s` 和 `load_r` 前缀。

先停止服务端，执行项目数据库结构脚本以确保离线消息表已经创建。脚本使用 `CREATE TABLE IF NOT EXISTS`，不会删除已有用户数据：

```bash
cd /mnt/hgfs/linuxgongxiang/imServer
mysql -u imserver -p < database/schema.sql
```

然后准备1000对测试账号：

```bash
cd /mnt/hgfs/linuxgongxiang/imServer
mysql -u imserver -p imdb < tests/prepare_business_load.sql
```

重新启动服务端，在另一个终端先执行小规模验证：

```bash
python3 tests/business_load_test.py \
  --pairs 10 \
  --messages-per-pair 5 \
  --concurrency 10 \
  --output business-report-10.json
```

确认登录、持久化、离线投递和ACK数量全部一致且错误数为0后，再执行100对账号、每对100条消息的基线测试。共10000条消息，可以获得比短时冒烟测试更稳定的吞吐量和P99数据：

```bash
python3 tests/business_load_test.py \
  --pairs 100 \
  --messages-per-pair 100 \
  --concurrency 50 \
  --output business-report-100.json
```

这项测试包含多次SQL查询、插入和更新，其吞吐量不能与纯心跳网络压测直接比较。测试结束后应同时保存压测JSON和服务端 `server_metrics` 日志。

## 一键核心业务集成测试

`integration_test.py` 使用四个以 `itest_` 开头的专用账号，自动验证以下完整链路：

- 登录与好友信息下发；
- 消息持久化、离线补发与消息ACK；
- ACK后重新登录不重复补发；
- 聊天历史分页及历史消息来源标记；
- 允许向离线用户添加好友；
- 好友申请登录补发、拒绝处理、结果补发及结果ACK；
- 好友结果ACK后重新登录不重复通知；
- 连续登录失败达到阈值后触发账号摘要限流。

先启动服务端并设置数据库环境变量，然后在另一个终端执行：

```bash
cd /mnt/hgfs/linuxgongxiang/imServer
export IM_DB_USER=imserver
export IM_DB_PASSWORD=123456
bash tests/run_integration_test.sh --output integration-report.json
```

脚本会先运行 `prepare_integration_test.sql`，仅重置四个专用账号之间的消息、好友关系和好友申请，不会修改普通用户数据。业务测试结束后还会查询数据库，确认四个账号都已升级为PBKDF2且使用四个不同的随机盐。测试全部通过时退出码为0且报告中的 `passed` 为 `true`；任何协议字段、投递次数、ACK语义或密码安全条件不符合预期时退出码为1。

也可以跳过数据准备，单独运行Python测试：

```bash
python3 tests/integration_test.py \
  --host 127.0.0.1 \
  --output integration-report.json
```
