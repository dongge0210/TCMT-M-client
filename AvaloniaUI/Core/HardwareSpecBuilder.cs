using TCMT.Avalonia.Models;

namespace TCMT.Avalonia.Core;

/// <summary>
/// 把 <see cref="SystemInfo"/> 摊成一张规格表。
///
/// 一条口径贯穿全篇：<b>采集端没给的值报 N/A，不按 0 算、也不编一个看起来像真的数</b>。
/// 为什么缺、缺在谁那一侧，写在页底那行说明里。这条是设计稿定的，不是这里发挥的 ——
/// 客户端的界面不该替采集端说谎。
///
/// 能复用模型上现成的 Display 属性就复用（<c>MemoryDisplay</c>、<c>SpeedDisplay</c> …），
/// 那些地方已经处理过单位与空值，重复实现只会把单位搞错。
/// </summary>
public static class HardwareSpecBuilder
{
    public static SpecSheet Build(HardwarePart part, SystemInfo info, bool online)
    {
        var sheet = part switch
        {
            HardwarePart.Cpu => Cpu(info),
            HardwarePart.Gpu => Gpu(info),
            HardwarePart.Memory => Memory(info),
            HardwarePart.Disk => Disk(info),
            HardwarePart.Volume => Volume(info),
            HardwarePart.Nic => Nic(info),
            HardwarePart.Battery => Battery(info),
            HardwarePart.Fan => Fan(info),
            HardwarePart.Sensor => Sensor(info),
            HardwarePart.System => System(info),
            _ => SpecSheet.Empty()
        };

        sheet = WithoutLeadingTitle(sheet);

        // 没连上时整页改画占位符。这时候不能报 N/A —— N/A 断言「这台机器没有这一项」，
        // 而我们根本还没问过；采集端没跑，它一个字都报不出来。
        return online ? sheet : MaskAsUnknown(sheet);
    }

    /// <summary>
    /// 首块的标题一律抹掉。单块页它就是页标题，多块页头一块它还是页标题 —— 页头已经写着，
    /// 再写一遍等于同一屏上打印两次同一个词。第二块起保留，用来把自己和前面分开。
    /// </summary>
    private static SpecSheet WithoutLeadingTitle(SpecSheet sheet) =>
        sheet.Blocks.Count == 0
            ? sheet
            : sheet with { Blocks = [sheet.Blocks[0] with { Title = "" }, .. sheet.Blocks.Skip(1)] };

    /// <summary>
    /// 采集端没连上：整页的值盖成「—」。
    ///
    /// 行标签照旧铺出来 —— 页面骨架不该因为断线换一个样子，变的只是值。
    /// SMART 表整张收掉（逐格盖「—」没有意义，它整张就是一组读数），
    /// 页底说明也收掉 —— 那些说明讲的都是「这一项为什么是 N/A」，此刻那句话不成立。
    /// </summary>
    private static SpecSheet MaskAsUnknown(SpecSheet sheet) =>
        sheet with
        {
            Blocks = sheet.Blocks.Select(MaskBlock).ToArray(),
            Note = ""
        };

    private static SpecBlock MaskBlock(SpecBlock block) =>
        block with { Units = block.Units.Select(MaskUnit).ToArray() };

    private static SpecUnit MaskUnit(SpecUnit unit) =>
        unit with
        {
            Rows = unit.Rows.Select(r => SpecRow.Unknown(r.Label)).ToArray(),
            Table = null
        };

    // ── 小工具 ───────────────────────────────────────────────────────────

    /// <summary>空串与 0 都当"没上报"。0 在这里不是有效读数，是"没读到"。</summary>
    private static SpecRow Row(string label, string? value) =>
        string.IsNullOrWhiteSpace(value) ? SpecRow.Na(label) : new SpecRow(label, value);

    private static SpecRow Count(string label, int value) =>
        value > 0 ? new SpecRow(label, value.ToString()) : SpecRow.Na(label);

    private static SpecRow Count(string label, ulong value) =>
        value > 0 ? new SpecRow(label, value.ToString("N0")) : SpecRow.Na(label);

    private static SpecRow Bytes(string label, ulong value) =>
        value > 0 ? new SpecRow(label, FormatUtils.FormatBytes(value)) : SpecRow.Na(label);

    private static SpecRow YesNo(string label, bool value) => new(label, value ? "是" : "否");

    private static SpecBlock Block(string title, params SpecUnit[] units) => new(title, units);

    private static SpecUnit Unit(params SpecRow[] rows) => new(rows);

    // ── 实时值的小工具 ───────────────────────────────────────────────────
    //
    // FormatUtils 里那几个函数对越界输入会返回字面量 "N/A" 或空串 —— 那是「值」，不是占位符：
    // 它会以正常颜色渲染出来，混在真正的 N/A 里分不清，而且 AI 从 MCP 那头读到的也会是个字符串
    // "N/A" 而不是缺失。所以统一在这里先拦一道，拦下来的走 SpecRow.Na，落到压暗的那一档。

    /// <summary>百分比。NaN 与负值当没上报；0 是有效读数，不拦。</summary>
    private static SpecRow Percent(string label, double percent) =>
        double.IsFinite(percent) && percent >= 0
            ? new SpecRow(label, FormatUtils.FormatPercentage(percent))
            : SpecRow.Na(label);

    /// <summary>电池电量。这一路采集端用 -1 表示没上报，所以不能按 0 处理。</summary>
    private static SpecRow Percent(string label, int percent) =>
        percent >= 0 ? new SpecRow(label, FormatUtils.FormatPercentage(percent)) : SpecRow.Na(label);

    private static SpecRow Frequency(string label, double megahertz) =>
        double.IsFinite(megahertz) && megahertz > 0
            ? new SpecRow(label, FormatUtils.FormatFrequency(megahertz))
            : SpecRow.Na(label);

    private static SpecRow Temperature(string label, double celsius) =>
        double.IsFinite(celsius) && celsius is > 0 and <= 150
            ? new SpecRow(label, FormatUtils.FormatTemperature(celsius))
            : SpecRow.Na(label);

    /// <summary>功耗。采集端给的是毫瓦，FormatPower 负责换成瓦。</summary>
    private static SpecRow Power(string label, double milliwatts) =>
        double.IsFinite(milliwatts) && milliwatts > 0
            ? new SpecRow(label, FormatUtils.FormatPower(milliwatts))
            : SpecRow.Na(label);

    /// <summary>充电器标称瓦数。这一项本来就是瓦，不走 FormatPower（那个要吃毫瓦）。</summary>
    private static SpecRow ChargerWatts(string label, double watts) =>
        double.IsFinite(watts) && watts > 0 ? new SpecRow(label, $"{watts:F1} W") : SpecRow.Na(label);

    // 0 B/s 是有效读数（网卡闲着），所以这里不判缺失，直接照写。
    private static SpecRow Throughput(string label, ulong bytesPerSecond) =>
        new(label, FormatUtils.FormatSpeed(bytesPerSecond));

    /// <summary>Swap 写成「已用 / 总量」一行 —— 两个数拆两行对不起这张表的密度。</summary>
    private static SpecRow Swap(string label, ulong used, ulong total) =>
        total > 0
            ? new SpecRow(label, $"{FormatUtils.FormatBytes(used)} / {FormatUtils.FormatBytes(total)}")
            : SpecRow.Na(label);

    /// <summary>负载三项并成一行 —— 它们是同一组的三个时刻，拆开就看不出是一组了。</summary>
    private static SpecRow LoadAverage(SystemInfo info) =>
        info.LoadAvg1 > 0 || info.LoadAvg5 > 0 || info.LoadAvg15 > 0
            ? new SpecRow("负载", $"{info.LoadAvg1:F2} / {info.LoadAvg5:F2} / {info.LoadAvg15:F2}")
            : SpecRow.Na("负载");

    /// <summary>运行时长粗到天 / 时 / 分。精确到秒没意义，截到整小时又太糊。</summary>
    private static SpecRow Uptime(SystemInfo info)
    {
        if (info.UptimeSeconds == 0) return SpecRow.Na("运行时长");

        var span = TimeSpan.FromSeconds(info.UptimeSeconds);
        var text = span.TotalDays >= 1
            ? $"{(int)span.TotalDays}d {span.Hours}h {span.Minutes}m"
            : span.TotalHours >= 1
                ? $"{(int)span.TotalHours}h {span.Minutes}m"
                : $"{span.Minutes}m";
        return new SpecRow("运行时长", text);
    }

    // ── CPU ──────────────────────────────────────────────────────────────

    private static SpecSheet Cpu(SystemInfo info) => new(
        [
            Block("CPU", Unit(
                Row("型号", info.CpuName),
                Count("核心", info.PhysicalCores),
                Count("逻辑核", info.LogicalCores),
                Count("性能核", info.PerformanceCores),
                Count("能效核", info.EfficiencyCores),
                Frequency("基频", info.CpuBaseFreq),
                YesNo("超线程", info.HyperThreading),
                YesNo("虚拟化", info.Virtualization))),
            Block("实时", Unit(
                Percent("占用", info.CpuUsage),
                Frequency("P 核频率", info.PerformanceCoreFreq),
                Frequency("E 核频率", info.EfficiencyCoreFreq),
                Temperature("温度", info.CpuTemperature),
                Power("功耗", info.CpuPower)))
        ],
        "大核 / 小核的分法是 Apple Silicon 的，x86 上采集端不上报这两项，故报 N/A —— 不按 0 显示。");

    // ── GPU ──────────────────────────────────────────────────────────────

    private static SpecSheet Gpu(SystemInfo info)
    {
        // 一块显卡都没有时照列五行报 N/A，不收成一句话：「这台机器没有独显」本身就是信息，
        // 而且无显卡的机器和有三块显卡的机器点的是同一个导航项，该看到同一张表，只是值空着。
        if (info.Gpus.Count == 0)
        {
            return new SpecSheet(
                [Block("GPU", Unit(GpuRows(null))), Block("实时", Unit(GpuLiveRows(info, null)))],
                "采集端没有上报任何 GPU（无显卡的机器，或采集端读不到）。");
        }

        // 只有多于一块显卡时才写小标题，单卡机器上那一行是噪音。
        var many = info.Gpus.Count > 1;
        var units = info.Gpus
            .Select((g, i) => new SpecUnit(GpuRows(g), many ? $"GPU {i + 1}" : null))
            .ToArray();

        // 实时那一块始终只有一组，不分卡 —— 线格式里只有 gpu/0 的占用 / 温度 / 显存，
        // 逐卡各画一组等于把同一个数抄成好几份。
        return new SpecSheet(
            [new SpecBlock("GPU", units), Block("实时", Unit(GpuLiveRows(info, info.Gpus[0])))],
            many ? "实时读数只覆盖第一块显卡 —— IPC 里只有 gpu/0 的占用 / 温度 / 显存。" : "");
    }

    /// <summary>
    /// 一块显卡的规格行。<paramref name="g"/> 为 null 表示这台机器没有这一块，各行全报 N/A。
    /// 品牌那一行是有条件的，见 <see cref="BrandRow"/>。
    /// </summary>
    private static SpecRow[] GpuRows(GpuData? g) =>
    [
        Row("型号", g?.Name),
        .. BrandRow(g),
        Row("显存", g is { Memory: > 0 } ? g.MemoryDisplay : null),
        Frequency("核心频率", g?.CoreClock ?? 0),
        g is null ? SpecRow.Na("虚拟设备") : YesNo("虚拟设备", g.IsVirtual)
    ];

    /// <summary>
    /// 品牌那一行，只在它真能补信息时才画。
    ///
    /// 型号里已经带着厂商名的时候（「Apple M2」的品牌、「NVIDIA GeForce RTX 4090」的品牌，
    /// 都是同一件事说两遍），这行不画；采集端压根没给的时候也不画 ——
    /// 一行永远是空的标签不是信息。全项目只有显卡这一处有品牌列，所以没做成通用规则。
    /// </summary>
    private static SpecRow[] BrandRow(GpuData? g)
    {
        if (g is null || string.IsNullOrWhiteSpace(g.Brand)) return [];
        if (g.Name?.Contains(g.Brand, StringComparison.OrdinalIgnoreCase) == true) return [];
        return [Row("品牌", g.Brand)];
    }

    /// <summary>
    /// 显卡的几个实时值。功耗从整机那一路取（<c>power/gpu</c>）—— 它不在 GpuData 上。
    /// <paramref name="g"/> 为 null 时全报 N/A。
    /// </summary>
    private static SpecRow[] GpuLiveRows(SystemInfo info, GpuData? g) =>
    [
        Percent("占用", g?.Usage ?? double.NaN),
        Temperature("温度", g?.Temperature ?? 0),
        Percent("显存占用", g?.MemoryPercent ?? double.NaN),
        Frequency("核心频率", g?.CoreClock ?? 0),
        Power("功耗", info.GpuPower)
    ];

    // ── 内存 ─────────────────────────────────────────────────────────────

    private static SpecSheet Memory(SystemInfo info) => new(
        [
            Block("内存", Unit(
                Bytes("总容量", info.TotalMemory),
                Row("内存类型", info.RamType),
                Row("内存速率", info.RamSpeed > 0 ? $"{info.RamSpeed} MT/s" : null))),
            Block("实时", Unit(
                Bytes("已用", info.UsedMemory),
                Bytes("可用", info.AvailableMemory),
                Bytes("已压缩", info.CompressedMemory),
                Swap("Swap", info.SwapUsed, info.SwapTotal)))
        ]);

    // ── 磁盘 ─────────────────────────────────────────────────────────────

    private static SpecSheet Disk(SystemInfo info)
    {
        if (info.PhysicalDisks.Count == 0)
        {
            return SpecSheet.Empty("采集端没有上报物理磁盘。");
        }

        var many = info.PhysicalDisks.Count > 1;
        var units = info.PhysicalDisks.Select((d, i) => new SpecUnit(
            [
                Row("型号", d.Model),
                Row("序列号", d.SerialNumber),
                Row("类型", d.DiskType),
                Bytes("容量", d.Capacity),
                Row("接口", d.InterfaceType),
                Row("固件", d.FirmwareVersion),
                YesNo("系统盘", d.IsSystemDisk),
                // 物理盘到卷的对应关系采集端还没建立，这两项两侧都给不出值。
                SpecRow.Na("挂载点"),
                SpecRow.Na("分区"),
                Row("健康度", d.HealthPercentage > 0 ? $"{d.HealthPercentage}%" : null),
                Row("磨损", d.WearLeveling > 0 ? FormatUtils.FormatPercentage(d.WearLeveling) : null),
                YesNo("SMART 启用", d.SmartEnabled),
                YesNo("SMART 支持", d.SmartSupported),
                Count("通电时长", d.PowerOnHours),
                Count("通电次数", d.PowerCycleCount),
                Bytes("累计写入", d.TotalBytesWritten),
                Bytes("累计读取", d.TotalBytesRead),
                Count("重映射扇区", d.ReallocatedSectorCount),
                Count("待定扇区", d.CurrentPendingSector),
                Count("不可纠正错误", d.UncorrectableErrors)
            ],
            many ? $"磁盘 {i + 1}" : null,
            SmartTable(d))).ToArray();

        // 磁盘这一页只有温度是实时的，其余全是读一次就不变的规格。小标题跟着上面那套走，
        // 多盘时两块各自对齐，单盘时两块都是光板。
        var liveUnits = info.PhysicalDisks
            .Select((d, i) => new SpecUnit(
                [Temperature("温度", d.Temperature)],
                many ? $"磁盘 {i + 1}" : null))
            .ToArray();

        return new SpecSheet(
            [new SpecBlock("磁盘", units), new SpecBlock("实时", liveUnits)],
            "macOS 把启动盘拆成两个卷：/ 是只读系统快照（每次系统更新重建），数据都在这个卷上，" +
            "所以这里报的是数据卷 —— 只有它带真实可用空间。" +
            "「挂载点」与「分区」采集端还没接（物理盘到卷的对应关系尚未建立），故报 N/A。");
    }

    /// <summary>SMART 属性表。没有属性时返回 null，界面就不画表。</summary>
    private static SpecTable? SmartTable(PhysicalDiskSmartData disk)
    {
        if (disk.Attributes.Count == 0)
        {
            return null;
        }

        var rows = disk.Attributes
            .Select(a => new SpecTableRow(
                Id: a.Id.ToString("X2"),
                Name: string.IsNullOrWhiteSpace(a.Name) ? a.Description : a.Name,
                Current: a.Current.ToString(),
                Worst: a.Worst.ToString(),
                Threshold: a.Threshold > 0 ? a.Threshold.ToString() : "—",
                Raw: a.RawValue.ToString("N0")))
            .ToList();

        return new SpecTable(rows);
    }

    // ── 卷 ───────────────────────────────────────────────────────────────

    private static SpecSheet Volume(SystemInfo info)
    {
        if (info.Disks.Count == 0)
        {
            return SpecSheet.Empty("采集端没有上报已挂载的卷。");
        }

        var many = info.Disks.Count > 1;
        var units = info.Disks.Select((d, i) => new SpecUnit(
            [
                Row("名称", d.Label),
                Row("挂载点", d.MountPoint),
                Row("文件系统", d.FileSystem),
                Bytes("容量", d.TotalSize),
                Bytes("已用", d.UsedSpace),
                Bytes("可用", d.FreeSpace)
            ],
            many ? $"卷 {i + 1}" : null)).ToArray();

        return new SpecSheet([new SpecBlock("卷", units)]);
    }

    // ── 网卡 ─────────────────────────────────────────────────────────────

    private static SpecSheet Nic(SystemInfo info)
    {
        if (info.Adapters.Count == 0)
        {
            return SpecSheet.Empty("采集端没有上报网络适配器。虚拟网卡（VPN、回环、网桥）不计入。");
        }

        var many = info.Adapters.Count > 1;
        var units = info.Adapters.Select((a, i) => new SpecUnit(
            [
                Row("接口", a.Name),
                Row("类型", a.AdapterType),
                Row("IPv4", a.IpAddress),
                Row("MAC", a.Mac),
                CurrentAddressRow(a),
                // SpeedDisplay 已经处理过单位，别在这里自己拼
                Row("链路速率", a.Speed > 0 ? a.SpeedDisplay : null)
            ],
            many ? $"网卡 {i + 1}" : null)).ToArray();

        // 采集端给的这两个值是字节/秒（NetworkAdapter.cpp 里算的就是 bytes/sec），
        // 所以走 FormatSpeed。别用 FormatNetworkSpeed —— 那是按位算的，会把 B/s 标成 bps。
        var liveUnits = info.Adapters.Select((a, i) => new SpecUnit(
            [
                Throughput("下载", a.DownloadSpeed),
                Throughput("上传", a.UploadSpeed)
            ],
            many ? $"网卡 {i + 1}" : null)).ToArray();

        return new SpecSheet(
            [new SpecBlock("网卡", units), new SpecBlock("实时", liveUnits)],
            OperatingSystem.IsMacOS()
                ? "只列真实网卡，VPN / 回环 / 网桥这类虚拟接口由采集端过滤掉。" +
                  "「当前地址」在 macOS 上取不到：系统把当前链路地址（私有 Wi-Fi 地址）只给已签名进程，" +
                  "未签名的本地构建读到的永远是全零值，所以这里报 N/A —— 不拿硬件地址冒充。"
                : "只列真实网卡。VPN / 回环 / 网桥这类虚拟接口由采集端过滤掉，不在这里显示。");
    }

    /// <summary>
    /// 「当前地址」那一行。三种情况分开说，不拿硬件地址冒充：
    ///   拿到了且与硬件地址不同 → 显示它（macOS 开私有 Wi-Fi 地址时的形态）
    ///   拿到了且相同           → 说明相同（Windows / Linux）
    ///   没拿到                 → N/A，原因见页底说明
    /// </summary>
    private static SpecRow CurrentAddressRow(NetworkAdapterData a)
    {
        if (string.IsNullOrWhiteSpace(a.CurrentMac)) return SpecRow.Na("当前地址");
        if (a.CurrentMac == a.Mac) return new SpecRow("当前地址", "与硬件地址相同");
        return new SpecRow("当前地址", a.CurrentMac);
    }

    // ── 电池 ─────────────────────────────────────────────────────────────

    private static SpecSheet Battery(SystemInfo info)
    {
        // 没有电池时照列四项报 N/A，不收成一句话 —— 「这是台台式机」本身就是信息。
        // 四项本来就走 Count/Row 的空值口径，所以这里不需要分支，只需要换掉页底那句说明。
        return new SpecSheet(
            [
                Block("电池", Unit(
                    Count("循环次数", info.BatteryCycleCount),
                    Count("设计容量", info.BatteryDesignCapacity),
                    Count("最大容量", info.BatteryMaxCapacity),
                    Percent("健康度", info.BatteryHealthPercent))),
                Block("实时", Unit(
                    Percent("电量", info.BatteryPercent),
                    PowerSourceRow(info),
                    ChargingRow(info),
                    Temperature("温度", info.BatteryTemperature),
                    ChargerWatts("充电功率", info.BatteryChargerWatts)))
            ],
            info.BatteryIsPresent ? "" : "这台机器没有电池（台式机，或采集端读不到电池），各行照列报 N/A。");
    }

    /// <summary>
    /// 电源那一行。没电池时也报 N/A —— 采集端这时给的是 acOnline=false，
    /// 照直写就成了「使用电池」，等于在描述一块并不存在的电池。
    /// </summary>
    private static SpecRow PowerSourceRow(SystemInfo info) =>
        !info.BatteryIsPresent
            ? SpecRow.Na("电源")
            : new SpecRow("电源", info.AcOnline ? "已接电源" : "使用电池");

    private static SpecRow ChargingRow(SystemInfo info) =>
        !info.BatteryIsPresent
            ? SpecRow.Na("充电中")
            : new SpecRow("充电中", info.BatteryIsCharging ? "是" : "否");

    // ── 风扇 ─────────────────────────────────────────────────────────────

    private static SpecSheet Fan(SystemInfo info)
    {
        // 没有风扇时照列一行「转速」报 N/A。无风扇机型（MacBook Air、部分一体机）是常见配置，
        // 那一行本身就是答案；而且这一页的行集是固定的，编得出来 —— 传感器那页就编不出来，见下。
        SpecUnit[] units = info.Fans.Count == 0
            ? [new SpecUnit([SpecRow.Na("转速")])]
            : info.Fans
                .Select(f => new SpecUnit([Row("转速", f.RpmDisplay)], f.DisplayName))
                .ToArray();

        return new SpecSheet(
            [new SpecBlock("风扇", units)],
            info.Fans.Count == 0 ? "这台机器没有上报风扇（无风扇机型，或采集端读不到）。" : "");
    }

    // ── 传感器 ───────────────────────────────────────────────────────────

    private static SpecSheet Sensor(SystemInfo info)
    {
        // 全项目唯一「照列不出来」的一页：每一行的标签就是那一路传感器的名字，没有传感器就没有
        // 标签可列。别的页行集是固定的，缺了就照列报 N/A；这里编不出一行来，只能把话说清楚。
        if (info.Temperatures.Count == 0)
        {
            return SpecSheet.Empty("采集端没有上报温度传感器。这一页每一行的标签就是传感器的名字，没有传感器就没有行可列。");
        }

        var rows = info.Temperatures
            .Select(t => Row(t.DisplayName, t.TemperatureDisplay))
            .ToArray();

        return new SpecSheet(
            [Block("传感器", Unit(rows))],
            "温度来自 SMC。IPC 线格式里没有逐核占用，所以这里报的是温度，不是负载。");
    }

    // ── 系统 ─────────────────────────────────────────────────────────────

    private static SpecSheet System(SystemInfo info)
    {
        var tpm = info.Tpm;

        // 没有 TPM 时六项照列报 N/A —— 与设计稿一致。列空着比整块消失更能说明"这台机器没有"。
        var tpmRows = tpm is null
            ? new[]
              {
                  SpecRow.Na("厂商"), SpecRow.Na("固件"), SpecRow.Na("状态"),
                  SpecRow.Na("自检"), SpecRow.Na("已启用"), SpecRow.Na("已激活")
              }
            : new[]
              {
                  Row("厂商", tpm.Manufacturer),
                  Row("固件", tpm.FirmwareVersion),
                  Row("状态", tpm.Status),
                  Row("自检", tpm.SelfTestStatus),
                  YesNo("已启用", tpm.IsEnabled),
                  YesNo("已激活", tpm.IsActive)
              };

        return new SpecSheet(
            [
                Block("系统", Unit(
                    Row("OS", info.OsVersion),
                    Row("主机型号", info.HardwareModel),
                    Row("应用版本", info.Version))),
                Block("TPM", Unit(tpmRows)),
                Block("实时", Unit(
                    LoadAverage(info),
                    Count("进程数", info.ProcessCount),
                    Uptime(info)))
            ],
            tpm is null ? "本机不提供 TPM，六项照列但报 N/A —— 采集端未上报此字段。" : "");
    }
}
