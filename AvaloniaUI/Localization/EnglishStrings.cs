using System.Collections.Generic;

namespace TCMT.Avalonia.Localization;

/// <summary>
/// 中文 → English。键就是代码里写的那句中文原文，见 <see cref="Translator"/> 的说明。
///
/// 漏掉一条不会坏：<see cref="Translator.T"/> 查不到就原样返回中文，对着界面能看出漏了哪条。
/// 带参数的写 <c>{0}</c>（<c>Translator.T</c> 那个带 params 的重载）。
/// </summary>
internal static class EnglishStrings
{
    internal static readonly Dictionary<string, string> ByChinese = new()
    {
        // ── 导航 / 外壳 ─────────────────────────────────────────────────
        ["概览"] = "Overview",
        ["进程"] = "Processes",
        ["硬件"] = "Hardware",
        ["网络"] = "Network",
        ["日志"] = "Log",
        ["设置"] = "Settings",
        ["展开 / 收起"] = "Expand / collapse",
        ["应用"] = "App",
        ["版本"] = "Version",
        ["应用版本"] = "App version",
        ["TCMT — 系统硬件监视器"] = "TCMT — system hardware monitor",
        ["系统硬件监视器"] = "System hardware monitor",
        ["系统硬件监视器 TCMT 0123456789"] = "System hardware monitor TCMT 0123456789",
        ["连接中..."] = "Connecting...",
        ["重新连接中..."] = "Reconnecting...",
        ["重新连接"] = "Reconnect",
        ["已连接"] = "Connected",
        ["已连接 (IPC)"] = "Connected (IPC)",
        ["已断开"] = "Disconnected",
        ["连接已断开"] = "Connection lost",
        ["连接超时"] = "Connection timed out",
        ["连接失败：已达最大重试次数"] = "Connection failed: retry limit reached",
        ["服务器已关闭"] = "Server shut down",
        ["重连失败"] = "Reconnect failed",
        ["读取硬件数据失败"] = "Failed to read hardware data",
        ["从共享内存读取整机信息失败"] = "Failed to read system info from shared memory",
        ["读取硬件数据仍在失败（第 {0} 次）"] = "Hardware read still failing (attempt {0})",
        ["硬件数据读取已恢复（连续失败 {0} 次后）"] = "Hardware reads recovered (after {0} failures)",
        ["错误: {0}"] = "Error: {0}",
        ["重连失败: {0}"] = "Reconnect failed: {0}",
        ["初始化失败: {0}"] = "Initialization failed: {0}",
        ["版本不匹配: {0}"] = "Version mismatch: {0}",
        ["已连接 ({0})"] = "Connected ({0})",
        ["已断开 ({0})"] = "Disconnected ({0})",
        ["已连接，但 SSID 读不到——系统需要开启定位服务才会返回 SSID"] =
            "Connected, but the SSID is not readable — macOS only returns it when location services are on",
        ["未检测到无线网卡——无线信息不可读"] = "No wireless adapter detected — Wi-Fi details unavailable",
        ["该系统不提供定位服务，SSID 与信号强度读不到——不是没连上"] =
            "This system has no location services, so SSID and signal strength cannot be read — this is not a connection problem",

        // ── 设置页 ──────────────────────────────────────────────────────
        ["应用设置"] = "Application settings",
        ["外观"] = "Appearance",
        ["主题"] = "Theme",
        ["字体"] = "Font",
        ["系统字体"] = "System font",
        ["系统字体面板…"] = "System font panel…",
        ["字号"] = "Size",
        ["关于"] = "About",
        ["采集端"] = "Collector",
        ["数据上传"] = "Data upload",
        ["服务器地址"] = "Server URL",
        ["跳过 TLS 校验"] = "Skip TLS verification",
        ["上传间隔（秒）"] = "Upload interval (s)",
        ["采样间隔（毫秒）"] = "Sampling interval (ms)",
        ["语言"] = "Language",
        ["中文"] = "中文",
        ["开"] = "On",
        ["关"] = "Off",
        ["只发改动的那一项，采集端应用后回推一份新值。"] =
            "Only the field you change is sent; the collector pushes the applied value back.",
        ["未连接采集端 —— 这一组现在改不了，值会在连上后自动填进来。"] =
            "Collector not connected — these cannot be changed now. They fill in once it connects.",

        // ── 硬件：页头、块名 ────────────────────────────────────────────
        ["静态规格与实时读数：采集端没提供、或这台机器没有的项，报 N/A 并说明原因，不编值"] =
            "Specs and live readings. Anything the collector does not publish, or this machine does not have, is listed as N/A with a reason — nothing is invented.",
        ["未连接采集端 —— 下面的值等连上再填"] = "Collector not connected — values fill in once it does",
        ["CPU"] = "CPU",
        ["GPU"] = "GPU",
        ["内存"] = "Memory",
        ["磁盘"] = "Disk",
        ["卷"] = "Volume",
        ["网卡"] = "NIC",
        ["电池"] = "Battery",
        ["风扇"] = "Fan",
        ["传感器"] = "Sensors",
        ["系统"] = "System",
        ["实时"] = "Live",
        ["SMART 健康"] = "SMART health",
        ["盘温"] = "Disk temperature",
        ["电池健康"] = "Battery health",
        ["电池供电"] = "On battery",
        ["交流供电"] = "On AC",
        ["逐核温度"] = "Per-core temperature",
        ["窗口峰值"] = "Window peak",
        ["负载 1 / 5 / 15"] = "Load 1 / 5 / 15",

        // ── 硬件：行标签 ────────────────────────────────────────────────
        ["型号"] = "Model",
        ["品牌"] = "Brand",
        ["核心"] = "Cores",
        ["逻辑核"] = "Threads",
        ["性能核"] = "Performance cores",
        ["能效核"] = "Efficiency cores",
        ["基频"] = "Base clock",
        ["超线程"] = "Hyper-threading",
        ["虚拟化"] = "Virtualization",
        ["P 核频率"] = "P-core clock",
        ["E 核频率"] = "E-core clock",
        ["占用"] = "Usage",
        ["温度"] = "Temperature",
        ["功耗"] = "Power",
        ["显存"] = "VRAM",
        ["显存占用"] = "VRAM usage",
        ["核心频率"] = "Core clock",
        ["虚拟设备"] = "Virtual device",
        ["总容量"] = "Total",
        ["内存类型"] = "Type",
        ["内存速率"] = "Speed",
        ["已用"] = "Used",
        ["可用"] = "Available",
        ["已压缩"] = "Compressed",
        ["压缩 / 可用"] = "Compressed / available",
        ["已用 / 总量"] = "Used / total",
        ["序列号"] = "Serial",
        ["类型"] = "Type",
        ["容量"] = "Capacity",
        ["接口"] = "Interface",
        ["固件"] = "Firmware",
        ["系统盘"] = "System disk",
        ["挂载点"] = "Mount point",
        ["分区"] = "Partitions",
        ["健康度"] = "Health",
        ["磨损"] = "Wear",
        ["SMART 启用"] = "SMART enabled",
        ["SMART 支持"] = "SMART supported",
        ["通电时长"] = "Power-on hours",
        ["通电次数"] = "Power cycles",
        ["累计写入"] = "Written",
        ["累计读取"] = "Read",
        ["重映射扇区"] = "Reallocated sectors",
        ["待定扇区"] = "Pending sectors",
        ["不可纠正错误"] = "Uncorrectable errors",
        ["文件系统"] = "File system",
        ["名称"] = "Name",
        ["当前地址"] = "Current address",
        ["与硬件地址相同"] = "Same as the hardware address",
        ["链接速度"] = "Link speed",
        ["循环次数"] = "Cycle count",
        ["设计容量"] = "Design capacity",
        ["最大容量"] = "Full charge capacity",
        ["电量"] = "Charge",
        ["电源"] = "Power source",
        ["已接电源"] = "Plugged in",
        ["使用电池"] = "On battery",
        ["充电中"] = "Charging",
        ["充电功率"] = "Charger",
        ["转速"] = "Speed",
        ["主机型号"] = "Model identifier",
        ["运行时长"] = "Uptime",
        ["负载"] = "Load",
        ["进程数"] = "Processes",
        ["厂商"] = "Vendor",
        ["状态"] = "Status",
        ["自检"] = "Self-test",
        ["已启用"] = "Enabled",
        ["已激活"] = "Activated",
        ["下载"] = "Down",
        ["上传"] = "Up",
        ["是"] = "Yes",
        ["否"] = "No",

        // ── 硬件：SMART 表头 ────────────────────────────────────────────
        ["属性"] = "Attribute",
        ["当前"] = "Cur",
        ["最差"] = "Worst",
        ["阈值"] = "Thr",
        ["原始值"] = "Raw",

        // ── 硬件：页底说明 ──────────────────────────────────────────────
        ["大核 / 小核的分法是 Apple Silicon 的，x86 上采集端不上报这两项，故报 N/A —— 不按 0 显示。"] =
            "Splitting cores into P and E is an Apple Silicon thing; the collector does not report these on x86, so they are N/A rather than shown as 0.",
        ["采集端没有上报任何 GPU（无显卡的机器，或采集端读不到）。"] =
            "The collector reported no GPU (a machine without one, or a read failure).",
        ["实时读数只覆盖第一块显卡 —— IPC 里只有 gpu/0 的占用 / 温度 / 显存。"] =
            "Live readings cover the first GPU only — the wire carries usage, temperature and VRAM for gpu/0.",
        ["macOS 把启动盘拆成两个卷：/ 是只读系统快照（每次系统更新重建），数据都在这个卷上，所以这里报的是数据卷 —— 只有它带真实可用空间。"] =
            "macOS splits the boot disk into two volumes: / is a read-only system snapshot rebuilt on every update, and the data lives on this one. This is the data volume because only it has real free space.",
        ["macOS 把启动盘拆成两个卷：/ 是只读系统快照（每次系统更新重建），数据都在这个卷上，"] =
            "macOS splits the boot disk into two volumes: / is a read-only system snapshot rebuilt on every update, and the data lives on this one,",
        ["所以这里报的是数据卷 —— 只有它带真实可用空间。"] =
            "so this is the data volume — only it carries real free space.",
        ["「挂载点」与「分区」采集端还没接（物理盘到卷的对应关系尚未建立），故报 N/A。"] =
            "Mount point and partitions are not wired up yet (the collector has no physical-disk-to-volume mapping), so they are N/A.",
        ["采集端没有上报已挂载的卷。"] = "The collector reported no mounted volumes.",
        ["采集端没有上报物理磁盘。"] = "The collector reported no physical disks.",
        ["采集端没有上报网络适配器。虚拟网卡（VPN、回环、网桥）不计入。"] =
            "The collector reported no network adapters. Virtual interfaces (VPN, loopback, bridge) are not counted.",
        ["只列真实网卡，VPN / 回环 / 网桥这类虚拟接口由采集端过滤掉。"] =
            "Only real adapters are listed; virtual interfaces (VPN, loopback, bridge) are filtered out by the collector.",
        ["只列真实网卡。VPN / 回环 / 网桥这类虚拟接口由采集端过滤掉，不在这里显示。"] =
            "Only real adapters are listed. Virtual interfaces (VPN, loopback, bridge) are filtered out by the collector and never reach this page.",
        ["「当前地址」在 macOS 上取不到：系统把当前链路地址（私有 Wi-Fi 地址）只给已签名进程，未签名的本地构建读到的永远是全零值，所以这里报 N/A —— 不拿硬件地址冒充。"] =
            "The current address is not available on macOS: the system hands the live link address (the private Wi-Fi address) only to signed processes, and an unsigned local build always reads all zeroes. So it is N/A here — the hardware address is not passed off as this one.",
        ["这台机器没有电池（台式机，或采集端读不到电池），各行照列报 N/A。"] =
            "This machine has no battery (a desktop, or the collector cannot read it); the rows are listed as N/A.",
        ["这台机器没有上报风扇（无风扇机型，或采集端读不到）。"] =
            "No fans were reported (a fanless model, or a read failure).",
        ["采集端没有上报温度传感器。这一页每一行的标签就是传感器的名字，没有传感器就没有行可列。"] =
            "The collector reported no temperature sensors. Each row on this page is named after its sensor, so with none there are no rows to list.",
        ["温度来自 SMC。IPC 线格式里没有逐核占用，所以这里报的是温度，不是负载。"] =
            "Temperatures come from the SMC. The wire format carries no per-core load, so this is temperature, not utilisation.",
        ["本机不提供 TPM，六项照列但报 N/A —— 采集端未上报此字段。"] =
            "This machine has no TPM. The six fields are listed as N/A — the collector does not publish them.",

        // ── 概览 ────────────────────────────────────────────────────────
        ["本机硬件实时读数 · 越界值才着色"] = "Live readings from this machine — only out-of-range values are coloured",
        ["% 占用"] = "% usage",
        ["% 总占用"] = "% total",
        ["% 电量"] = "% charge",
        ["个接口"] = " interfaces",
        ["3 台设备"] = "3 devices",
        ["（{0} 台设备）"] = "({0} devices)",
        ["{0} 适配器"] = "{0} adapters",
        ["最高 {0} RPM · {1} 个"] = "Max {0} RPM · {1}",

        // ── 进程页 ──────────────────────────────────────────────────────
        ["按 CPU 排序 · 单进程阈值 20 / 50 %"] = "Sorted by CPU · per-process thresholds 20 / 50 %",
        ["按名称或 PID 过滤"] = "Filter by name or PID",
        ["结束进程"] = "End process",
        ["确认结束"] = "Confirm",
        ["取消"] = "Cancel",
        ["将向该进程发送 SIGTERM，未保存的数据会丢失。"] =
            "This sends SIGTERM to the process; unsaved data will be lost.",
        ["采集侧未提供进程列表——连上之后这里会列出占用最高的进程"] =
            "The collector has not published a process list — once it connects, the busiest processes appear here",
        ["失败"] = "Failed",
        ["已收到回执 · PID {0} 结束请求已下发"] = "Acknowledged · SIGTERM sent to PID {0}",
        ["失败 · PID {0} · {1}"] = "Failed · PID {0} · {1}",
        ["需要 root 权限"] = "Needs root",
        ["请联系开发者更新"] = "Ask the developer for an update",

        // ── 网络页 ──────────────────────────────────────────────────────
        ["未连接——在系统设置里选择网络，这里会跟着变"] =
            "Not connected — pick a network in System Settings and this follows",
        ["WiFi：已断开"] = "WiFi: disconnected",
        ["WiFi：未检测到适配器"] = "WiFi: no adapter found",
        ["WiFi：已连接  频道:{0}  信号:{1} dBm  {2}{3}"] =
            "WiFi: connected  channel:{0}  signal:{1} dBm  {2}{3}",
        ["频段"] = "Band",
        ["频道"] = "Channel",
        ["协议"] = "Protocol",
        ["这一代"] = "Gen",
        ["下行"] = "Down",
        ["上行"] = "Up",
        ["峰值"] = "Peak",

        // ── 日志页 ──────────────────────────────────────────────────────
        ["暂无日志条目"] = "No log entries yet",
        ["跟随最新"] = "Follow latest",
        ["已暂停跟随"] = "Following paused",
        ["条"] = "entries",
        ["警示"] = "Warn",
        ["通过"] = "Pass",
        ["失败"] = "Fail",

        // ── 通用状态词 ──────────────────────────────────────────────────
        ["未知"] = "Unknown",
        ["不可用"] = "Unavailable",
        ["禁用"] = "Disabled",
        ["已关闭"] = "Closed",
        ["已接入"] = "Connected",
        ["未测试"] = "Not tested",
        ["正常"] = "OK",
        ["未知传感器"] = "Unknown sensor",
        ["未知分区"] = "Unknown partition",
        ["未知显卡"] = "Unknown GPU",
        ["未知磁盘"] = "Unknown disk",
        ["未检测到 TPM"] = "No TPM detected",
        ["蓝牙未检测到"] = "Bluetooth not detected",
        ["供电"] = "Power",
        ["温度"] = "Temperature",
    };
}
