namespace TCMT.Avalonia.Models;

/// <summary>
/// 硬件视图里的十个部件。既是导航子项的标识，也是规格表的取数口径。
/// </summary>
public enum HardwarePart
{
    Cpu,
    Gpu,
    Memory,
    Disk,
    Volume,
    Nic,
    Battery,
    Fan,
    Sensor,
    System
}

/// <summary>
/// 部件的显示名与顺序。放在一处，导航子项和页面标题都从这里取 ——
/// 以后加一个部件只改这个枚举和这里，不用去翻界面代码。
/// </summary>
public static class HardwareParts
{
    /// <summary>导航里从上到下的顺序。</summary>
    public static readonly IReadOnlyList<HardwarePart> Ordered =
    [
        HardwarePart.Cpu,
        HardwarePart.Gpu,
        HardwarePart.Memory,
        HardwarePart.Disk,
        HardwarePart.Volume,
        HardwarePart.Nic,
        HardwarePart.Battery,
        HardwarePart.Fan,
        HardwarePart.Sensor,
        HardwarePart.System
    ];

    public static string NameOf(HardwarePart part) => part switch
    {
        HardwarePart.Cpu => "CPU",
        HardwarePart.Gpu => "GPU",
        HardwarePart.Memory => "内存",
        HardwarePart.Disk => "磁盘",
        HardwarePart.Volume => "卷",
        HardwarePart.Nic => "网卡",
        HardwarePart.Battery => "电池",
        HardwarePart.Fan => "风扇",
        HardwarePart.Sensor => "传感器",
        HardwarePart.System => "系统",
        _ => part.ToString()
    };
}

/// <summary>
/// 一行规格：左边标签、右边值。
///
/// <paramref name="IsNa"/> 为真表示这一行的值不是真读数，只是个压暗的占位符 ——
/// 要么是 N/A（这台机器没有这一项），要么是「—」（还没连上，无从断言）。
/// 两种都宁可留白也不编一个看起来像真的数。
/// </summary>
public sealed record SpecRow(string Label, string Value, bool IsNa = false)
{
    /// <summary>没连上时的占位符。与 N/A 一样是压暗的，不参与数值对齐。</summary>
    public const string UnknownValue = "—";

    /// <summary>这台机器没有这一项 —— 恒定结论，与采没采到样本无关。</summary>
    public static SpecRow Na(string label) => new(label, "N/A", IsNa: true);

    /// <summary>
    /// 还没连上，这项是什么不知道。与 <see cref="Na"/> 是两回事：N/A 是查完之后的结论，
    /// 这个是「还没查」。混用会让页面替采集端说谎。
    /// </summary>
    public static SpecRow Unknown(string label) => new(label, UnknownValue, IsNa: true);
}

/// <summary>
/// SMART 属性表的一行。
///
/// 列是定形的而不是 <c>IReadOnlyList&lt;IReadOnlyList&lt;string&gt;&gt;</c> —— 全项目只有这一张表，
/// 定形之后 XAML 能按名字绑定、编译器也能查错；换成嵌套列表就得靠下标，既丢编译检查也没好到哪去。
/// 表头写死在视图里（同样只有这一张表）。
/// </summary>
public sealed record SpecTableRow(
    string Id,
    string Name,
    string Current,
    string Worst,
    string Threshold,
    string Raw);

/// <summary>只读表格，目前只有磁盘的 SMART 属性表在用。</summary>
public sealed record SpecTable(IReadOnlyList<SpecTableRow> Rows);

/// <summary>
/// 一个实例。多数部件只有一组；多 GPU、多磁盘、多网卡时每个各一组，
/// 只有多于一组才显示 <paramref name="Subtitle"/>。
/// </summary>
public sealed record SpecUnit(
    IReadOnlyList<SpecRow> Rows,
    string? Subtitle = null,
    SpecTable? Table = null)
{
    // 给 XAML 用：绑一个 bool 比绑可空值再配转换器干净。
    public bool HasSubtitle => !string.IsNullOrWhiteSpace(Subtitle);
    public bool HasTable => Table is { Rows.Count: > 0 };
}

/// <summary>
/// 一块：一个部件（CPU / 磁盘 …）对应一块。
///
/// <paramref name="Title"/> 为空表示这一块不写标题。首块一律不写 —— 它就是页标题，
/// 页头已经写着；只有第二块起才需要标题把自己和前面分开。同
/// <see cref="SpecUnit.HasSubtitle"/> 的「多于一组才写」，只是这里按位置算。
/// </summary>
public sealed record SpecBlock(string Title, IReadOnlyList<SpecUnit> Units)
{
    public bool HasTitle => !string.IsNullOrWhiteSpace(Title);
}

/// <summary>
/// 一页：若干块，加一行页底说明。
///
/// <see cref="Note"/> 是设计稿的既有形态 —— 用浅色小字讲清楚这块数据的口径或平台差异，
/// 缺项为什么缺也写在里面。空串表示这页不需要说明。
/// </summary>
public sealed record SpecSheet(IReadOnlyList<SpecBlock> Blocks, string Note = "")
{
    public bool HasNote => !string.IsNullOrWhiteSpace(Note);

    public static SpecSheet Empty(string note = "") => new([], note);
}
