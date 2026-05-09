# TCMT Windows Client 架构 / 协议 / 共享内存 / 功能规划总文档（全量更新版）
版本：v0.14

## 目录
1. 目标与阶段路线总览  
2. 当前模块状态一览  
5. 写入完整性：writeSequence 奇偶 + 报警机制  
6. 指令系统设计与实施步骤  
7. 指令清单（现有 / 预留 / 占位）  
8. 错误与日志规范（主日志 + errors.log + 环形对接 + 统一格式 + JSON 错误处理暂缓）  
9. 主板 / BIOS 信息采集规划  
10. 进程枚举（第一版无 CPU%）  
11. USB 枚举与去抖（debounce）  
12. 环形趋势缓冲（short/long + snapshotVersion 定义）  
13. 插件系统最小 SDK 约定（占位）  
14. SMART 复检指令占位与异步策略（含正常刷新间隔）  
15. 进程控制与黑名单策略  
17. 风险与回退策略  
18. 验收与测试步骤总表（含自检分类 + 单元测试 TODO）  
19. 原生温度采集（高优先级，NVAPI / ADL / WMI / SMART 异步）  
21. FAQ / 常见坑提示  
23. 后续扩展路线图（分析 / 安全 / 跨平台）  
25. 收尾与后续触发点  
27. MCP 服务器构想（高优先级，订阅过滤 + 限速行为）  
28. 自检与扩展分析（sys.selftest / SMART 评分趋势 / sys.metrics / 性能优化 / LLM 预备 / critical 分类）  
28. 14 SMART Aging Curve（老化曲线草图 + 配置化）  
29. 事件回放模拟（命名/保留策略）  
30. 硬件扩展补充（字段与无效值约定）  
31. TUI 设计与实施（含 degradeMode 行为 & DATA_NOT_READY 格式）  
32. 单位与精度规范  
33. 并发与线程安全模型  
34. 版本与兼容策略（abiVersion 映射）  
35. 历史与保留策略（快照/事件/Aging 持久化）  
36. 安全与权限初步（角色/RBAC/哈希链占位）  
37. 构建与工具脚本命名（偏移/自检/打包）
38. 开发路线图（源自 plan.md 合并，状态未更新）

---

## 1. 目标与阶段路线总览
| 阶段 | 描述 | 状态 | 备注 |
|------|------|------|------|
| Phase 1 | 基础硬件采集 + 共享内存稳定 | 进行中 | 扩展与奇偶序列即将实现 |
| Phase 2 | 原生温度迁移 + SMART 异步 | 规划完成 | 温度优先，SMART 异步稍后 |
| Phase 3 | 前端日志环形缓冲落地 | 规划完成 | 诊断基础 |
| Phase 3a | 偏移 JSON 自动生成 + 哈希校验 | 前移 | 与环形并行 |
| Phase 4 | 指令系统最小集 | 待实现 | 提供基础操作入口 |
| Phase 5 | SMART 异步刷新 | 待实现 | 评分前置条件 |
| Phase 6 | 自检 sys.selftest | 待实现 | 启动可信度 |
| Phase 7 | 温度事件合并 + urgent | 待实现 | 用户体验 |
| Phase 8 | SMART 寿命评分 | 待实现 | 健康分析核心 |
| Phase 8.1 | SMART Aging Curve | 规划 | 预测剩余安全天数 |
| Phase 9 | sys.metrics 聚合资源 | 待实现 | 指标汇总 |
| Phase 10 | 自适应刷新 | 待实现 | 动态刷新间隔 |
| Phase 11 | LLM 接入预备 | 低优先级 | 描述 + 最小化视图 |
| Phase 12 | MCP MVP | 很低优先级 | 外部协议占位 |
| Phase 13 | Plugin Tick 性能统计 | 最低 | 插件后置 |
| Phase 14 | Event Replay 基础 | 规划 | 历史重建 |
| Phase 15 | Aging Curve 深化 | 规划 | 平滑与多阈值 |
| Phase 16 | Extended Hardware Resources | 规划 | nvme/memory/cpu_features |

---

## 2. 当前模块状态一览
| 模块 | 现状 | 问题 | 下一步 |
|------|------|------|--------|
| SharedMemory | 旧结构 | 无扩展字段 | 扩展 + writeSequence |
| writeSequence | 未实现 | 脏读风险 | 奇偶协议接入 |
| 温度 | 托管桥 | 精度性能不足 | 原生迁移 |
| SMART 异步 | 未实现 | 无健康评分数据 | 异步线程 |
| 主板/BIOS | 未采集 | 字段空 | WMI/SMBIOS 填充 |
| 进程枚举 | 未实现 | 无进程列表 | 基础枚举 |
| USB | 未实现 | 插拔日志风暴 | 去抖策略 |
| 趋势缓冲 | 未实现 | 无曲线数据 | short/long 环形 |
| 指令系统 | 未实现 | 无外部调用入口 | 最小集落地 |
| 环形日志缓冲 | 规划完成 | 无具体结构 | 编码实现 |
| 偏移校验 | 手工核对 | 易错难维护 | 自动 JSON + debug 工具 |
| SMART 评分 | 未实现 | 无健康指标 | 规则+趋势 |
| 温度事件合并 | 未实现 | 刷屏风险 | 合并 + urgent 机制 |
| sys.metrics | 未实现 | 无统一指标输出 | 聚合刷新 |
| 自适应刷新 | 未实现 | 固定间隔不优 | 负载阈值动态调整 |
| MCP | 构想 | 未接入 | 后置 |
| 插件 SDK | 占位 | 无执行 | 后置 |
| Aging Curve | 未实现 | 无趋势预测 | 评分之后 |
| Event Replay | 未实现 | 无历史回放 | 快照与事件文件 |
| Extended Hardware | 未实现 | 诊断不够细 | 扩展资源采集 |

---


## 5. 写入完整性：writeSequence 奇偶 + 报警机制
| 阶段 | seq | 描述 |
|------|-----|------|
| 写开始 | seq 奇数 | 标记写进行中 |
| 写结束 | seq 偶数 | 写完成可读 |
| 读端看到奇数 | 使用旧快照 | 防止撕裂 |
| 连续奇数 ≥N 次 | WARN | 写线程阻塞 |
默认 N=5；后续可升级双缓冲或版本原子号。

---

## 6. 指令系统设计与实施步骤
请求 JSON：`{ "id":"req-1","type":"refresh.hardware","args":{} }`  
响应 JSON：`{ "id":"req-1","type":"refresh.hardware","status":"ok","data":{...} }`  
错误：`{ "id":"req-1","status":"error","error":{"code":"NOT_SUPPORTED","message":"..."} }`  
初版不做鉴权；后续与 MCP 角色权限整合。

---

## 7. 指令清单（现有 / 预留 / 占位）
| 指令 | 状态 | 说明 |
|------|------|------|
| system.ping | 计划 | 健康检测 |
| refresh.hardware | 计划 | 强制刷新快照 |
| disk.smart.scan | 占位 | SMART 复检触发 |
| baseboard.basic | 占位 | 主板信息返回 |
| log.config.update | 预留 | 动态限流级别 |
| process.list | 预留 | 进程枚举 |
| process.kill | 预留 | 高危操作（后期） |
| usb.rescan | 预留 | USB 重新枚举 |
| stats.export | 预留 | 趋势导出 |
| mcp.describe | 预留 | MCP 元信息 |
| plugin.list | 预留 | 插件枚举 |

---

## 8. 错误与日志规范
主日志：DEBUG/INFO/WARN/ERROR 全量。  
errors.log：仅 WARN/ERROR。  
统一格式：`YYYY-MM-DD HH:MM:SS.mmm LEVEL message key=value ...`  
环形日志对接：  
- 超限：标记 `[RATE_LIMITED]`  
- 覆盖：`WARN frontend_log_overrun lost=<count>`  
- 配置更新：`INFO backend_log_config_update ...`  
- 哈希失败：`ERROR frontend_log_hash_mismatch seq=<seq>`  

### 8.5 JSON Error Handling Pending
JSON 错误处理（标准化 error.code/message/hint/detail）暂缓，等待 CPP-parsers 增加错误 API（has()/lastError()/typed get）。当前仅在指令响应中使用简单 error 对象；复杂场景后续补。

---

## 9. 主板 / BIOS 信息采集规划
字段：manufacturer / product / version / serial（可遮蔽） / biosVendor / biosVersion / biosDate / formFactor / memorySlotsTotal / memorySlotsUsed / secureBootEnabled / tpmPresent / virtualizationFlags。  
策略：启动异步一次性采集 → 更新 lastBaseboardRefreshTs。

---

## 10. 进程枚举（第一版无 CPU%）
周期：1.5~2 秒。  
字段：PID / 名称 / WorkingSetMB / blacklistFlag。  
不写入共享内存，使用后台缓存。  
CPU% 后期通过两次时间差值计算。

---

## 11. USB 枚举与去抖
监听设备变化 → 设置 pending → debounceMs=500 延迟实际枚举。  
字段：deviceId / friendlyName / vendorId / productId / class / lastChangeTs。  
减少重复插拔日志风暴。

---

## 12. 环形趋势缓冲
指标初版：CPUUsage%、MemoryUsage%、GpuTemperature。  
short 环形：60 点；long 环形：300 点。  
暂停：snapshotVersion 未变 ≥5 次停止追加。  
未来：磁盘 IO、网络吞吐扩展。

---

## 13. 插件系统最小 SDK 约定（占位）
函数：Plugin_GetInfo / Plugin_Initialize / Plugin_Tick / Plugin_Shutdown / Plugin_HandleMessage。  
仅枚举目录 ./plugins，不执行。  
性能统计（p95/avg）后置实现。

---

## 14. SMART 复检指令占位与异步策略
SMART 异步线程：队列、重试 schedule=[1,5,10]、失败日志。  
disk.smart.scan：触发全盘或单盘复检（初版占位返回）。  

- 全属性刷新：smart.refreshIntervalSeconds=30
- 温度轻量刷新：smart.tempIntervalSeconds=10
- 重试 schedule：[1,5,10]；超过 maxSmartRetries 标记 stale

---

## 15. 进程控制与黑名单策略
黑名单列表（配置）：System / Registry / WinInit / smss.exe / csrss.exe / explorer.exe。  
UI 禁用 kill；后期高危操作需 ADMIN + 二次确认。

---


## 17. 风险与回退策略
| 风险 | 征兆 | 回退 |
|------|------|------|
| 偏移错误 | UI 数据乱码 | static_assert + offsets diff |
| 序列卡奇数 | 连续奇数报警 | 重试写 / 双缓冲 |
| 温度全 N/A | 温度视图空 | 关闭原生温度采集 |
| SMART 阻塞 | score 不更新 | 超时重启线程 |
| 日志覆盖频繁 | overrunCount 激增 | 扩容或限流调整 |
| 哈希失败频繁 | hashOk=0 重复 | 检查写入顺序 / 盐 |
| 评分跳动 | 分数忽高忽低 | 延长趋势窗口 |
| 自适应抖动 | 间隔频繁变化 | 提高 consecutiveCount |
| 结构哈希不匹配 | WARN hash mismatch | degradeMode |
| Aging 预测异常 | remainingSafeDays 巨变 | confidence=low 屏蔽提示 |

---

## 18. 验收与测试步骤总表
| 项目 | 条件 | 标准 |
|------|------|------|
| sizeof 校验 | 编译后 | ==125818 |
| 序列奇偶 | 模拟阻塞写 | 第5次奇数 WARN |
| 温度迁移差值 | 过渡首轮 | 仅一条 temp_diff |
| SMART 异步重试 | 故障盘 | 3 次重试日志 |
| 环形日志限流 | >20 条/s | [RATE_LIMITED] 标记 |
| 覆盖计数 | 压力测试 | WARN frontend_log_overrun |
| 主板字段 | 采集完成 | 字段非空 |
| USB 去抖 | 快速插拔 | 重复减少 |
| 趋势缓冲 | ≥5 分钟 | long=300 点 |
| 偏移 JSON | 启动 | 文件含 sharedmemSha256 |
| debug 程序 | 执行 | 退出码=0 |
| 自检 | 启动 | urgentJumpThresholdC 出现 |
| SMART 评分 | 有盘 | score + advice |
| 温度合并 | 多变化1秒内 | 单条 batch 事件 |
| urgent 温度 | 单跳≥5°C | 即刻 urgent=true |
| 自适应刷新 | 模拟负载 | 间隔日志变化 |
| sys.metrics | 请求 | pending 标记 |
| Aging Curve | ≥2 点 | slope 计算 |
| Event Replay | 人工文件 | quality 合理 |
| Extended 资源 | 有设备 | 返回数据或空数组+message |

### 18.1 Critical Checks
(sharedmem_size / offsets / hash / write_sequence_flip / nvapi_init / adl_init)

### 18.2 Non-Critical Checks
(smart 单盘失败 / baseboard 缺失 / trend 缺数据 / 单传感器不可用)

### 18.3 状态决策
(≥1 critical fail → fail；仅 non-critical fail → partial_warn；全通过 → ok)

### 18.4 Unit Test Plan (TODO)
- [ ] 列出需要测试的模块（sharedmem、hash、sequence、SMART refresh、温度合并）
- [ ] 编写最小断言场景（结构 size、hash 重复计算一致性）
- [ ] 添加失败路径测试（写线程阻塞 / SMART 重试）
- [ ] 以后扩展：fuzz 指令解析 / TUI 参数解析
单元测试代码后续单独实现，不在本文中。

---

## 19. 原生温度采集
范围：CPU(WMI)、GPU(NVAPI/ADL)、磁盘(SMART)。  
过渡：托管桥一次对比 → temp_diff 日志1条 → 移除桥。  
SMART 温度：异步 schedule 重试。  
无效值：-1 显示 N/A。

---


## 21. FAQ / 常见坑提示
| 问题 | 原因 | 解决 |
|------|------|------|
| 温度大量 N/A | 驱动缺失/权限 | 安装驱动或关闭原生温度 |
| 序列长时间奇数 | 写线程卡死 | 重启写线程 / 加双缓冲 |
| SMART 温度异常 | 解析偏移错 | 校正字段索引 |
| 日志覆盖多 | 入口过量 | 调整限流或容量 |
| 哈希频繁失败 | 初始化乱序 | 统一盐和写顺序 |
| 分数突然下降 | 属性激增 | 检查 recentGrowth |
| 自适应频繁跳 | 阈值太紧 | 调整 consecutiveCount |
| Aging 预测不可信 | 点少/波动大 | confidence=low 不提醒 |
| Replay 缺数据 | 快照未写 | 启用历史定时任务 |
| Extended 空 | 无设备 | message 说明不可用 |

---


## 23. 后续扩展路线图
| 类别 | 扩展点 | 描述 |
|------|--------|------|
| 性能 | 双缓冲/差分写入 | 降低撕裂 |
| 分析 | stats.export/告警引擎 | 智能预警 |
| 安全 | RBAC/审计链 | 权限细粒度控制 |
| 插件 | 沙箱/WASM | 隔离与扩展 |
| 硬件 | 风扇/功耗/网络 | 更全面指标 |
| 跨平台 | Linux/macOS Provider | 统一抽象层 |
| AI | context bundle/minimal | 降 token 消耗 |
| 历史 | 快照归档/回放 | 故障复盘 |
| 签名 | 日志/结构签名 | 防篡改审计 |

---


## 25. 收尾与后续触发点
实施阶段变更 → 先改文档再写代码。  
阶段完成写里程碑日志：`INFO milestone_reached phase=<n>`。  
进入编码需明确文字指令："要代码：<模块>"。  

---


## 27. MCP 服务器构想
资源 + 工具 + 事件统一对接模型或外部 UI。  
传输：stdio 行 JSON → 未来 WebSocket。  
资源集合：sys.cpu / sys.memory / sys.gpus / sys.disks.logical / sys.disks.physical / sys.temperatures / sys.baseboard / sys.processes / sys.usb / sys.trend.short / sys.trend.long / sys.log.config / sys.log.latest / sys.health。  
事件：logs.frontend / temperature.change（batch + urgent）。  
错误码：BAD_ARGUMENT / NOT_FOUND / PERMISSION_DENIED / RATE_LIMIT / INTERNAL_ERROR / NOT_SUPPORTED / TIMEOUT / CONFLICT。  
限速：30 req/s。  
LLM 预备：description + minimal + bundle + hint。  

---

## 28. 自检与扩展分析
### 28.1 目标
启动时生成 sys.selftest 验证核心子系统可靠性。  

### 28.2 自检项目
sharedmem_size / sharedmem_offsets / write_sequence_flip / log_ring_hash / nvapi_init / adl_init / smart_sample / baseboard_fields / trend_buffer_basic / urgentJumpThresholdC。  

### 28.3 输出示例
```
{
  "status":"partial_warn",
  "generatedTs":1697826000456,
  "urgentJumpThresholdC":5,
  "checks":[ ... ],
  "warnCount":1
}
```

### 28.4 SMART 寿命评分
规则扣分 + 趋势扣分（trendWindow=5）。  
NVMe wearPercent 获取失败 → 不扣。  

### 28.5 示例
```
{
  "diskId":"PhysicalDisk0",
  "score":74,
  "riskLevel":"medium",
  ...
}
```

### 28.6 sys.metrics
聚合指标（含 pending 字段）。  

### 28.7 性能优化
自适应刷新 & 温度事件合并 + urgent。  

### 28.8 温度事件结构
批量与 urgent 两种 JSON 格式。  


### 28.10 Plugin Tick 性能统计
统计 p95/avg/max 等，占位。  

### 28.11 LLM 接入预备
description / bundle / minimal / hint / 白名单。  


### 28.13 顺序确认
扩展顺序已含 14~16 新阶段。  

### 28.14 SMART Aging Curve（老化曲线草图）
- 采样点：`{ts,hoursOn,score}`；条件：hoursOn 增或 score 变化幅度>1。  
- 缓存：最近 50 条。  
- trendWindow=10；斜率：`(score_last - score_first)/(hoursOn_last - hoursOn_first)`。  
- slope<0 时预测达到 score=50 的剩余小时 → remainingSafeDays。  
- 置信度：点少 / |slope| 很小 / 标准差大 → low。  
- high/medium/low 置信度规则 + agingWindow/agingThresholdScore

---

## 29. 事件回放模拟
快照文件 + 事件文件重建指定窗口状态。  
工具占位：tool:replay.build（mode=point/window）。  
质量等级：full / partial / degraded。  
初版不插值，仅拼合。  
文件命名与保留策略：  
- 快照：snapshot_YYYYMMDD_HHMMSS.json  
- 事件：events_YYYYMMDD_HHMMSS.jsonl  
- 保留：快照7天，事件30天  

---

## 30. 硬件扩展补充
新增资源：  
- sys.nvme.ext：队列深度、使用率、错误计数  
- sys.memory.channels：通道/频率/XMP/模块列表  
- sys.cpu.features：指令集、虚拟化、HT  
- sys.gpu.driver（后置）：驱动版本与推荐版本差异  
- sys.net.adapters（后置）：速率、流量、地址  
占位策略：无设备 → 空数组 + message；未知 → -1 或 null。  
无效值约定：温度=-1，百分比=-1，计数=0xFFFFFFFF，字符串空。

---

## 31. TUI 设计与实施（含 degradeMode 行为 & DATA_NOT_READY 格式）

### 31.1 目标
提供命令行工具 `tcm`：快速查看系统状态、偏移校验、刷新、SMART 与 Aging 相关信息；后续支持事件回放与硬件扩展显示。  

### 31.2 范围与非目标
- 第一批：sys-status / offsets verify / temps / refresh（可占位） / smart / smart-aging（数据不足返回码 6）。  
- 不含：交互式 REPL、复杂历史回放、日志注入（后续扩展）。

### 31.9 SMART / Aging 占位策略
- SMART 异步未完成：smart 命令 exitCode=6（DATA_NOT_READY）。  
- Aging 数据点 <2：smart-aging exitCode=6。  

### 31.10 JSON 库
采用 nlohmann/json（单头文件）。  
若后续性能要求提升，可考虑 RapidJSON，但初版不抽象层。  

### 31.11 环境变量
| 变量 | 描述 | 默认 |
|------|------|------|
| TCM_SHM_NAME | 共享内存名 | Global\TCMT_SharedMemory |
| TCM_MODE | local 或 mcp 模式占位 | local |
| TCM_OFFSETS_PATH | 覆盖偏移 JSON 路径 | 未设置 |

优先级：命令行 --file > TCM_OFFSETS_PATH > 默认路径。

### 31.12 验收点（TUI）
| 项目 | 条件 | 标准 |
|------|------|------|
| sys-status degrade | 模拟 degradeMode | 退出码=4 |
| offsets verify 缺文件 | 移除 JSON | exitCode=2 |
| offsets verify 差异 | 手工改偏移 | exitCode=5 |
| SMART 未就绪 | 无 SMART 数据 | exitCode=6 |
| Aging 点不足 | <2 点 | exitCode=6 |
| CPU 未实现 | 未采集 CPU% | 文本显示 (pending) |
| --json 输出 | 各命令带 --json | JSON 格式合法 |
| --pretty | 任一命令 | JSON 缩进 |
| 环境变量覆盖 | 设置 TCM_SHM_NAME | 使用新名成功 |
| refresh 指令失败 | 模拟错误 | exitCode=3 |

### 31.13 阶段划分（TUI 专属）
| 阶段 | 内容 | 备注 |
|------|------|------|
| TUI-A | sys-status + offsets verify | 基础读取与校验 |
| TUI-B | temps + refresh | 温度与刷新入口 |
| TUI-C | smart + smart-aging 占位 | 等 SMART 异步完成 |
| TUI-D | 日志注入 (可选) | 环形日志测试 |
| TUI-E | MCP 模式切换 | 待 MCP MVP |
| TUI-F | replay / hw 扩展命令 | 高级功能 |

### 31.14 与主程序协同
- 偏移 JSON 必须在主程序启动时生成。  
- TUI 的字段偏移数组与主程序结构保持同步（由公共头文件导出）。  
- 结构升级时先更新偏移 JSON 和头文件，再编译 TUI。  

### 31.16 未来扩展占位
命令：`tcm replay` / `tcm hw nvme` / `tcm hw memory` / `tcm hw cpu-features` / `tcm log --level INFO "..."` / `tcm trend export` / `tcm diag dump`。

---

## 32. 单位与精度规范
- 温度：0.1°C 精度 (valueC_x10)
- CPU 使用：*10 未实现为 -1
- 内存：MB（近似 1024*1024 bytes）
- 不可用数值：统一 -1 或空数组
- 时间戳：Unix ms

---

## 33. 并发与线程安全模型
- 单写多读依赖 writeSequence 奇偶
- 自检遇奇数用旧快照
- SMART 异步独立线程
- TUI 只读映射

---

## 34. 版本与兼容策略
- abiVersion 结构变化即递增
- 0xMMMMmmpp 末尾字节可对应文档序号（参考非强制）
- 引入 salt 属结构变化需 bump abiVersion

---

## 35. 历史与保留策略
- 快照每整点一次，保留 7 天
- 事件日志按天存储
- Aging 曲线点暂不持久化（后续可写 disk_age_history.json）

---

## 36. 安全与权限初步
- 角色占位：READ_SYS / CONTROL / ADMIN / HIGH_RISK
- process.kill 需 ADMIN + 二次确认
- 日志哈希链 future prevHash 计划

---

## 37. 构建与工具脚本命名
- generate_offsets.exe
- verify_sharedmem.exe
- selftest_dump.exe
- release_pack.ps1

## 38. 开发路线图（源自 plan.md 合并，状态未更新）

### 当前进行 (feat/restore-fixes)
- [ ] Windows IPHLPAPI.lib 链接修复
- [ ] macOS Avalonia 实时网速验证
- [ ] Avalonia 重复速度显示修复

### 近期 (1-2 个月)

#### WebSocket 远程监控
- 嵌入式 HTTP 服务器 (端口 8080)
- JSON 数据推送
- 浏览器仪表盘 (Canvas 图表)
- 依赖: 无 (纯 C++ asio)

#### SQLite 历史数据
- 本地存储 30 天指标
- 简单趋势查询 API
- 依赖: SQLite3 (单头文件版)

#### ETW 进程级磁盘监控 (Windows)
- 先写原型验证可行性
- 评估管理员权限影响
- 依赖: Windows SDK (已有)

### 中期 (3-6 个月)

#### IPC 协议增强
- 握手/心跳检测
- 协议版本号
- 优雅关闭信号

#### 告警系统
- 阈值触发 (温度/负载)
- 系统通知中心集成
- 可选: Telegram Webhook

#### 自动化脚本
- Lua 插件接口
- 示例: CPU 过热降频脚本

### 远期/不确定

- AI 异常检测 (需要研究轻量模型)
- 云端同步 (需要后端 + 加密方案)
- Home Assistant 集成 (看需求)
- 内核级监控 (Windows Driver / macOS DriverKit)
  - **阻塞**: 开发者账号费用
  - **优先级**: 低 (ETW 足够现阶段)

### 其他方向

#### 进程管理 (调研中)
- 进程级资源占用详情
- 快速结束高占用进程
- **顾虑**: 数据量大，代码复杂度高，需调研可行性

#### 笔记本特定
- 电池健康度追踪 (循环次数、容量衰减)
- 电源适配器功率检测
- 合盖后台监控 (服务化)

#### 硬件扩展
- 蓝牙设备电量 (耳机、鼠标)
- 外接显示器信息 (分辨率、刷新率、HDR)
- USB 设备插拔历史
- **状态**: 调研可行性

#### 国际化 (i18n)
- 中英文切换
- Avalonia 本地化支持

#### UI 美化
- 暗黑/亮色主题切换
- 图表类型切换（折线/柱状/数字）
- 自定义仪表盘布局（拖拽组件）

#### MCP 服务器
- TCMT-M 暴露 MCP 接口 (stdio 或 HTTP)
- 工具: `get_cpu_status`, `get_disk_health`, `get_battery_info`
- 场景: AI 远程诊断、自动化脚本触发
- 复杂度: 低 (MCP 协议简单)

#### 硬件检测增强
- 识别 USB 设备具体型号
- 检测显示器连接/断开事件
- 识别蓝牙设备详细规格

#### 多厂商 GPU 监控 (用户态)
- **AMD GPU**: ADL2 SDK (AMD 官方用户态库，免费) — 温度/频率/使用率/功耗
  - 参考: LibreHardwareMonitor 的 ADL 封装代码
  - 条件: 有对应硬件时实现
- **Intel GPU**: Intel PresentMon (开源 MIT) 或 GPA — GPU Busy/帧时间/频率估算
  - 通过 ETW 事件采集，无需特殊驱动
  - GitHub: GameTechDev/PresentMon
  - 条件: 有对应硬件时实现
- **NVIDIA GPU**: 已有 NVML 实现，保持不变

#### WiFi 信号与网络详情 (Native Wifi API)
- Windows 原生 wlanapi.dll (系统自带，无需驱动签名)
- 可读: 当前连接 SSID 信号强度(dBm + 百分比)、信道、频段(2.4G/5G)、收发速率
- 可控 (可选): 连接/断开/切换 AP (需评估是否属于 TCMT 范围)
- 优先级: 中 (有无线网卡即可实现)

#### WinRT 备选方案 (Windows 10+ 仅限)
- **系统 Toast 通知**: 替代托盘气球通知，更现代的告警方式
- **蓝牙设备详情/电量**: Windows.Devices.Bluetooth API
- **电源适配器实时功率**: WMI 读不到的实时功率数据
- **条件**: 不考虑 Win7/8 兼容性；等需要 Windows 原生功能时再引入
- **注意**: TCMT 已有 #ifdef _WIN32 分平台架构，Windows 侧引入 WinRT 不影响 macOS

#### 智能场景检测 (低优先级)
- 基于进程名 + 硬件负载模式自动识别使用场景
- 场景分类: Gaming / Compiling / Video / Idle / Unknown
- 判断依据:
  - Gaming: GPU>80% + UDP网络活跃 + CPU波动大
  - Compiling: CPU全核满载 + 磁盘随机读 + GPU低
  - Video: GPU解码活跃 + 网络下载持续
  - Idle: CPU<10% + GPU<5%
- 应用:
  - 自动切换监控频率 (游戏时降频省资源)
  - 异常行为标记 (和平时对比，今天哪里不对劲)
  - 进程智能标签 ("Unity 编辑器" vs "Unity 游戏")
- 初版可用规则判断实现，后续可升级轻量 AI 模型
- **状态**: 低优先级，标签保留

#### 调试/开发工具
- 内置日志查看器
- TCMT 自身性能分析（监控工具的监控）

### 明确不做

- eBPF (macOS 支持差)
- WPP (需要内核驱动开发)
- DriverKit (99美元/年开发者账号)
- **WinRing0 及依赖内核签名的方案** (驱动签名证书费用高昂，数千美元级别)
  - AMD CPU 温度/功耗 (MSR 访问需 WinRing0)
  - 主板传感器/SuperIO 芯片读取
  - 内存 SPD / SMBus 访问
  - 替代: 使用用户态 API (WMI/ADL/PresentMon/Native Wifi) 尽可能覆盖

---

**说明: 大部分功能实现时间不明确，仅作参考。优先级和排期根据实际需求动态调整。**