# systemd部署说明

以下示例将服务端安装到 `/opt/im-server`，使用独立的低权限用户运行，并由 `systemd` 负责开机启动、异常重启和优雅停止。

先完成Release构建，然后执行：

```bash
sudo useradd --system --home /opt/im-server --shell /usr/sbin/nologin imserver
sudo install -d -o imserver -g imserver /opt/im-server
sudo install -m 0755 build/im_server_linux /opt/im-server/im_server_linux

sudo install -d -m 0750 /etc/im-server
sudo install -m 0640 deploy/im-server.env.example /etc/im-server/im-server.env
sudo chown root:imserver /etc/im-server/im-server.env
sudo editor /etc/im-server/im-server.env

sudo install -m 0644 deploy/im-server.service /etc/systemd/system/im-server.service
sudo systemctl daemon-reload
sudo systemctl enable --now im-server
```

查看运行状态与JSON日志：

```bash
systemctl status im-server
journalctl -u im-server -f
curl http://127.0.0.1:9108/metrics
```

发布新版本时，先完成构建与测试，再替换二进制并重启：

```bash
sudo systemctl stop im-server
sudo install -m 0755 build/im_server_linux /opt/im-server/im_server_linux
sudo systemctl start im-server
```

服务收到 `SIGTERM` 后会退出主循环，依次停止Reactor和工作线程、等待在途数据库操作并关闭连接池。日志由 `journald` 管理，可通过系统的 `journald.conf` 设置容量和保留周期，不需要应用直接写日志文件。
