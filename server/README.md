# tcmt-server

中间层（relay / organize / display）：接收 TCMT-M 客户端推送的快照，整理成可查询的
历史和摘要，通过 REST + WebSocket 提供给 viewer，并直接托管 viewer 页面。

## 角色划分

| 角色 | 说明 |
| --- | --- |
| client (TCMT-M) | 采集硬件信息，`--http` 模式下每 2s 推送快照（现有 ServerProbe 逻辑） |
| **server** (本目录) | 中转 + 整理：注册设备、保留环形历史、字段索引、派生 summary，对外提供 REST/WS |
| viewer (server/viewer) | 展示端：纯前端页面，只读 server 数据，无任何业务/存储逻辑（随 server 保留） |

## 运行

```bash
node server.js                 # 默认 0.0.0.0:8080（跨设备，打印局域网访问地址）
node server.js --port 9000     # 换端口
node server.js --host 127.0.0.1 # 仅本机访问（更安全）
node server.js --auth-token <secret>              # 读接口 + /ws 需要 Bearer token
node server.js --tls-cert fullchain.pem --tls-key privkey.pem --auth-token <secret>  # HTTPS/WSS
node server.js --public-url https://tcmt.example.com  # 对外展示地址（hello 消息里返回）
```

启动客户端（同一局域网任意机器）：

```bash
./build/src/TCMT-M --http --server http://<server-ip>:8080
# 例如 --server http://192.168.1.20:8080
# 不指定 --server 时默认 http://127.0.0.1:8080；也可用环境变量 TCMT_SERVER
```

任意设备浏览器打开 `http://<server-ip>:8080/` 即为 viewer（多设备同屏显示，
点击设备卡片切换详情）。

viewer 是独立的展示端，随 server 一起保留在 `server/viewer/`，由 server 直接托管；
也可以单独双击 `viewer/index.html` 使用（自动回退到 127.0.0.1:8080 取数）。

## 跨设备 / 网络说明

- server 默认监听 `0.0.0.0`，启动时会打印所有 LAN 访问地址；
  客户端 `ServerProbe` 已支持 IP / 主机名 / 反向代理 base path（`http://host:8080/tcmt`）。
- 读接口默认无鉴权；`--auth-token` 开启后所有 `/api/*` 读接口与 `/ws` 需要
  `Authorization: Bearer <token>`（浏览器 WS 用 `?access_token=`）。写接口
  `/api/ingest` 始终需要设备 token。
- 设备身份：客户端用持久化的随机 `clientKey`（`~/.tcmt/client.json`）注册，
  不依赖 IP/主机名——NAT 重绑、换网络、公网共享 server 都不会串设备；
  重启复用同一设备条目，列表不会膨胀；token 失效（401）会自动重新注册。
- 客户端网络卫生：8s 连接/读写超时（不会挂死）、断线自动重试、
  支持 `https://`（需编译时找到 OpenSSL；自签证书用 `--server-insecure` 跳过校验）。

## NAT / 公网部署

链路始终是 **client 主动外连 server**（出站即可穿透 NAT），viewer 打开 server
托管的页面。按 server 所在位置选择拓扑：

### A. 全内网（默认）
`node server.js` → 各机器 client 填 `--server http://<内网IP>:8080`，viewer 同网段访问。

### B. server 在公网 VPS（推荐公网方案）
```bash
# VPS 上（先申请 Let's Encrypt 或任意可信证书）
node server.js --tls-cert /etc/letsencrypt/live/tcmt.example.com/fullchain.pem \
               --tls-key  /etc/letsencrypt/live/tcmt.example.com/privkey.pem \
               --auth-token <secret> --public-url https://tcmt.example.com
```
```bash
# 各采集机（NAT 后面无需任何端口转发）
./build/src/TCMT-M --http --server https://tcmt.example.com
```
防火墙只放行 443（或反向代理端口）。viewer 打开 `https://tcmt.example.com`，
首次访问会提示输入 `--auth-token` 的值，浏览器记住后自动携带。

### C. server 在家庭/公司 NAT 后面
- 路由器端口转发（TCP 8443 → server 内网 IP），配合 DDNS 域名；或
- 出站隧道（无需公网 IP/端口转发）：Cloudflare Tunnel、frp、ngrok、
  Tailscale（含 Funnel）。隧道会把 TLS 终结在边缘，本地 server 可以跑明文
  `node server.js --auth-token <secret>`，客户端和 viewer 都指向隧道域名。

### 动态 IP / 断线
- client 启动后每 2s 重试注册/推送；server 恢复后自动续传，无需重启 client。
- 域名用 DDNS（如 DuckDNS / Cloudflare）或直接走隧道，动态 IP 无感。
- server 重启不影响已注册设备：token 持久化在 `server/data/devices.json`；
  若库被清空，client 收到 401 会自动重新注册（仍复用原 clientKey）。

### 安全建议
- 公网必须开 `--auth-token` + TLS；仅内网可跑明文。
- 默认绑 0.0.0.0 是方便局域网/公网直连；不想暴露就 `--host 127.0.0.1`。
- 更严格时在前面加 nginx/caddy 反向代理：限 IP、加 rate limit、WS 转发。

## API

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/ping` | 健康检查 |
| POST | `/api/register` | 注册设备 `{name,os,model}` → `{id,token,name}` |
| POST | `/api/ingest` | 推送快照 `{token, ...fields}` |
| GET | `/api/devices` | 设备列表（含 online 状态） |
| GET | `/api/devices/:id` | 设备信息 |
| GET | `/api/devices/:id/latest` | 最近一次快照 |
| GET | `/api/devices/:id/summary` | 整理后的摘要（cpu/memory/gpu/motion/temperatures） |
| GET | `/api/devices/:id/fields` | 字段索引（min/max/last/count） |
| GET | `/api/devices/:id/history?field=x&from=-1h&to=&limit=1000` | 时间序列 |
| GET | `/api/devices/:id/temperatures` | 温度数组 |
| GET | `/ws` | WebSocket：`devices` 列表每 0.5s 广播，`snapshot` 实时推送 |
| GET | `/` | 托管 viewer 静态页面 |

## 数据

- 设备注册信息持久化在 `data/devices.json`（token 重启后保持有效）。
- 快照保留在内存环形缓冲（每设备 1800 条 ≈ 1 小时 @2Hz），重启即清空；
  需要长期落盘时可在 `store.ingest` 里追加 JSONL 或 SQLite。
- `/api/ingest` 需要 token 鉴权；快照中的 `token` 字段不会外泄（存储前剥离）。
- 默认只绑定 127.0.0.1，对外暴露请自行加反向代理/鉴权。

## 结构

```
server/
├── server.js          # 入口：HTTP + WebSocket + 广播
├── lib/store.js       # 设备注册表 + 环形历史 + 字段索引 + 派生 summary
├── lib/api.js         # REST 路由 + 静态文件
├── lib/ws.js          # RFC-6455 握手/帧/心跳（零依赖）
├── viewer/            # 展示端：纯前端仪表盘（独立端，由 server 托管）
└── data/              # 运行时数据（gitignore）
```
