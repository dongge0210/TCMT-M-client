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

    // ── CPU ──────────────────────────────────────────────────────────────

    private static SpecSheet Cpu(SystemInfo info) => new(
        [Block("CPU", Unit(
            Row("型号", info.CpuName),
            Count("核心", info.PhysicalCores),
            Count("逻辑核", info.LogicalCores),
            Count("性能核", info.PerformanceCores),
            Count("能效核", info.EfficiencyCores),
            Row("基频", info.CpuBaseFreq > 0 ? FormatUtils.FormatFrequency(info.CpuBaseFreq) : null),
            YesNo("超线程", info.HyperThreading),
            YesNo("虚拟化", info.Virtualization)))],
        "大核 / 小核的分法是 Apple Silicon 的，x86 上采集端不上报这两项，故报 N/A —— 不按 0 显示。");

    // ── GPU ──────────────────────────────────────────────────────────────

    private static SpecSheet Gpu(SystemInfo info)
    {
        if (info.Gpus.Count == 0)
        {
            return SpecSheet.Empty("采集端没有上报任何 GPU。");
        }

        // 只有多于一块显卡时才写小标题，单卡机器上那一行是噪音。
        var many = info.Gpus.Count > 1;
        var units = info.Gpus.Select((g, i) => new SpecUnit(
            [
                Row("型号", g.Name),
                Row("品牌", g.Brand),
                Row("显存", g.Memory > 0 ? g.MemoryDisplay : null),
                Row("核心频率", g.CoreClock > 0 ? FormatUtils.FormatFrequency(g.CoreClock) : null),
                YesNo("虚拟设备", g.IsVirtual)
            ],
            many ? $"GPU {i + 1}" : null)).ToArray();

        return new SpecSheet([new SpecBlock("GPU", units)]);
    }

    // ── 内存 ─────────────────────────────────────────────────────────────

    private static SpecSheet Memory(SystemInfo info) => new(
        [Block("内存", Unit(
            Bytes("总容量", info.TotalMemory),
            Row("内存类型", info.RamType),
            Row("内存速率", info.RamSpeed > 0 ? $"{info.RamSpeed} MT/s" : null)))]);

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

        return new SpecSheet(
            [new SpecBlock("磁盘", units)],
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

        return new SpecSheet(
            [new SpecBlock("网卡", units)],
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
        if (!info.BatteryIsPresent)
        {
            return SpecSheet.Empty("这台机器没有电池（台式机，或采集端读不到电池）。");
        }

        return new SpecSheet(
            [Block("电池", Unit(
                Count("循环次数", info.BatteryCycleCount),
                Count("设计容量", info.BatteryDesignCapacity),
                Count("最大容量", info.BatteryMaxCapacity),
                Row("健康度", info.BatteryHealthPercent > 0
                    ? FormatUtils.FormatPercentage(info.BatteryHealthPercent)
                    : null)))]);
    }

    // ── 风扇 ─────────────────────────────────────────────────────────────

    private static SpecSheet Fan(SystemInfo info)
    {
        if (info.Fans.Count == 0)
        {
            return SpecSheet.Empty("这台机器没有上报风扇（无风扇机型，或采集端读不到）。");
        }

        var units = info.Fans
            .Select(f => new SpecUnit([Row("转速", f.RpmDisplay)], f.DisplayName))
            .ToArray();

        return new SpecSheet([new SpecBlock("风扇", units)]);
    }

    // ── 传感器 ───────────────────────────────────────────────────────────

    private static SpecSheet Sensor(SystemInfo info)
    {
        if (info.Temperatures.Count == 0)
        {
            return SpecSheet.Empty("采集端没有上报温度传感器。");
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
                Block("TPM", Unit(tpmRows))
            ],
            tpm is null ? "本机不提供 TPM，六项照列但报 N/A —— 采集端未上报此字段。" : "");
    }
}
