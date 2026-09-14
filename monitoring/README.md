# IM服务端监控

## 一键启动

先确认IM服务端正在运行，并且以下命令能返回 `im_server_up 1`：

```bash
curl http://127.0.0.1:9108/metrics
```

安装Docker及Compose插件后，在项目根目录执行：

```bash
export GRAFANA_ADMIN_PASSWORD='请替换为自己的密码'
docker compose -f monitoring/compose.yaml up -d
docker compose -f monitoring/compose.yaml ps
```

### VMware共享目录不能绑定挂载

如果项目位于 `/mnt/hgfs`，Docker创建容器时可能出现 `statfs ... input/output error`。这是Docker守护进程无法绑定挂载 `vmhgfs-fuse` 文件，并非Compose内容错误。只需把监控配置复制到Linux原生文件系统后启动，服务端代码仍可保留在共享目录：

```bash
mkdir -p /home/lu/im-monitoring
cp -a monitoring/. /home/lu/im-monitoring/
cd /home/lu/im-monitoring
export GRAFANA_ADMIN_PASSWORD='请替换为自己的密码'
sudo --preserve-env=GRAFANA_ADMIN_PASSWORD docker compose -f compose.yaml up -d
sudo --preserve-env=GRAFANA_ADMIN_PASSWORD docker compose -f compose.yaml ps
```

修改共享目录中的监控配置后，再执行一次 `cp -a` 同步即可。

### Ubuntu 24.04旧版Compose报错

如果 `docker compose version` 出现 `No module named 'distutils'`，说明系统仍在调用Python实现的旧版 `docker-compose` v1。Ubuntu 24.04应替换为原生Compose v2插件：

```bash
sudo apt remove -y docker-compose
sudo apt update
sudo apt install -y docker-compose-v2
docker compose version
```

不要通过补装Python `distutils` 继续维持旧版Compose；v2使用 `docker compose` 子命令，中间没有连字符。

如果拉取Docker Hub时出现 `connection refused`，可分别验证当前Compose使用的两个仓库：

```bash
curl -I --connect-timeout 10 https://quay.io/v2/
curl -I --connect-timeout 10 https://m.daocloud.io/v2/
```

返回401表示仓库可达且正在等待Docker自动获取认证令牌，属于正常结果。

Compose会完成以下工作：

- Prometheus每5秒从宿主机9108端口抓取IM服务端指标。
- Prometheus页面监听宿主机9090端口。
- Grafana页面监听宿主机3000端口。
- Grafana自动创建Prometheus数据源。
- Grafana自动加载“IM Linux服务端监控”仪表盘。
- Prometheus时序数据和Grafana状态使用Docker卷保存。

Prometheus使用官方Quay镜像，Grafana通过DaoCloud代理官方Docker Hub镜像，适用于无法直连 `registry-1.docker.io` 但能访问Quay和DaoCloud的网络环境。

## 打开页面

- Prometheus：`http://127.0.0.1:9090`
- Prometheus目标状态：`http://127.0.0.1:9090/targets`
- Grafana：`http://127.0.0.1:3000`
- Grafana管理员用户名：`admin`

登录后进入“仪表盘 → IM服务端 → IM Linux服务端监控”。Compose不会提供默认管理员密码，启动前必须通过环境变量设置。

启动前设置自己的管理员密码：

```bash
export GRAFANA_ADMIN_PASSWORD='请替换为自己的密码'
docker compose -f monitoring/compose.yaml up -d
```

Prometheus和Grafana镜像使用固定版本，避免 `latest` 在不同时间拉取到未经本项目验证的版本。升级版本时应先运行指标抓取和仪表盘回归测试。

## 验证采集

检查Prometheus目标是否为 `UP`：

```bash
curl -s http://127.0.0.1:9090/api/v1/targets | grep -o '"health":"[^"]*"'
```

直接查询当前IM连接数：

```bash
curl -sG http://127.0.0.1:9090/api/v1/query \
  --data-urlencode 'query=im_server_active_connections'
```

启动Windows客户端或运行压测后，Grafana中的连接、包速率、字节速率和数据库指标应实时变化。

## 停止监控

```bash
docker compose -f monitoring/compose.yaml down
```

该命令仅停止并删除监控容器和网络，不删除命名数据卷。不要附加 `-v`，否则会同时删除历史监控数据。
