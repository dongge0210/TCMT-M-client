# macOS 27 功率读取限制与 tcmt-powerd 特权助手

> 状态:已实现并验证(2026-09-06)。本文件记录完整的调研证据链,避免后人重复踩坑。

## 问题

macOS 27(Darwin 27)起,TUI 的 Power 面板(CPU/GPU/ANE/Total)恒为 0.00 W,而频率读数正常。

## 调研证据链(全部实测)

| 假设 | 实验 | 结果 |
|---|---|---|
| IOReport 调用姿势问题 | 换订阅解析出的通道描述符采样、CreateSamples 传 NULL 过滤器、原始 merged 字典 | 三种姿势结果相同:287 通道在、"Energy Model" 组 136 通道 scalar 全 0 |
| 采样没跑 | `/tmp/ioreport_debug.log`(PowerMonitor 自带) | sample thread 正常,DVFS 表读出,P-core/E-core/GPU 频率加权计算正常(delta#9: 1874/1941/444 MHz) |
| 通道已消失 | 首个 delta 全通道清单 | "Energy Model" 136 + "GPU Stats" 135 + "CPU Stats" 16;能量通道在但值 0 |
| root 能读? | `sudo powermetrics -n 2 -i 1000` | **root 可读**:CPU 1277 mW / GPU 1063 mW |
| 我们 root 进程能读? | 编译 root LaunchDaemon 直读 IOReport | **不能**:root + 无签名进程仍全 0 → 门控在 **entitlement 级**(Apple 签名二进制才有权),root 身份不够 |
| AppleSMC 无特权键? | 自写 SMC 探针(修正协议:命令放 data8、通道恒 selector 2) | AppleSMC 可无特权打开;M2 基础款 **无分簇功率键**(Heimdall 的 PC02/PC42/PC03/PC43 缺失);PSTR(总功率)存在但 root 与非特权均恒 0(键在该 SoC 无效) |
| PSHM 跨 uid? | daemon(root)创建 0644 共享内存,非 root 读 | **macOS POSIX shm 跨 uid 不可读**:无论 mode,fchmod 无效,非 root `shm_open` EACCES(root 可读) |

结论:macOS 27 上,非 Apple 签名进程(即使 root)读不到 IOReport 能量;**唯一可用的无特权方案是间接使用 Apple 签名的 `/usr/bin/powermetrics`(root)**。

## 方案:tcmt-powerd

架构(数据层,UI 渲染见下文契约):

```
/usr/bin/powermetrics (Apple-signed, 仅 root)
        ▲ root LaunchDaemon: tcmt-powerd (/usr/local/libexec)
        │   每 ~3-5s 采样一次,解析 CPU/GPU/ANE Power(mW)
        ▼
/tmp/tcmt-power — 40B 结构化文件(root:wheel 0644, sticky /tmp 防替换)
        ▼ 非特权 TCMT-M 每采样轮读取 (PowerMonitor::ReadShmPower)
TuiData.cpuPower/gpuPower/anePower + powerAvailable
```

组件:
- `src/helper/powerd/tcmt-powerd_main.cpp` — daemon;popen powermetrics(`-n 2 -i 1000`),sscanf 逐行解析 `CPU/GPU/ANE Power: N mW`,写 `/tmp/tcmt-power`(magic `TCMTPOWR` + seq + 3×double mW;`O_CREAT|O_EXCL` 防抢占,失败 unlink 重试;sticky /tmp 保证创建后不可被替换)。
- `src/core/ioreport/IOReportSampler.mm` — 客户端:`ReadShmPower()` 每采样轮 open+read,fstat 校验属主 root,seq 变化才更新;`kEnergyModelAllowed = macOS major < 27` 门控能量提取(频率采样不受限)。
- `PowerMonitor::IsPowerAvailable()` — direct IOReport(≤26)或 SHM 有效读数时为 true。
- `tools/install-powerd.sh` / `uninstall-powerd.sh` — sudo 安装/卸载(拷贝二进制 + LaunchDaemon plist + `launchctl bootstrap`)。注意:`bootout` 后需 sleep 再 `bootstrap`(launchd 竞态会报 EIO)。
- 构建:CMake target `tcmt-powerd`(macOS only;源 = daemon main + Logger.cpp,不链 TCMTCore)。

## 数据契约(UI 用)

- `PowerMonitor::IsPowerAvailable() → bool`
- `SystemInfo::powerAvailable` / `TuiData::powerAvailable`(bool,Snapshot 填充)
- `powerAvailable == false` 时 `cpuPower/gpuPower/anePower` 保持 0 —— **UI 应显示 "N/A" 而非 0.00 W**,可提示 `sudo tools/install-powerd.sh`。

## 验证记录

- daemon 自检日志(`/var/log/tcmt-powerd.log`,前 3 次 publish):`publish cpu=8019.000000mW gpu=59.000000mW ...`
- 非特权客户端日志:`PowerMonitor: reading power from tcmt-powerd (cpu=10619.000000mW)`
- 数值与 `sudo powermetrics` 同量级。
- macOS ≤26 行为不变(直读路径,gate 保证);Windows 不受影响(macOS-only)。

## 已知边界 / 后续

- 功率更新粒度 ~3-5s(powermetrics 采样窗口),非 1s。
- ANE 在 M2 基础款空闲为 0(通道行为,非错误)。
- macOS ≤26 且装了 daemon 时,客户端仍走直读(不读文件);daemon 无副作用可常驻。
- UI "N/A" 渲染未合入(数据契约已就绪)—— 见 UI agent 提示词(plan: .claude/plans/encapsulated-gathering-cookie.md 第 5 节)。
