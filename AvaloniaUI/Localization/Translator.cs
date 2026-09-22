using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Globalization;

namespace TCMT.Avalonia.Localization;

/// <summary>
/// 界面文案的中英对照。
///
/// **中文原文就是键**（gettext 那一套）。这么做是有意的：全仓 278 条中文字面量，
/// 其中 146 条集中在 HardwareSpecBuilder 里当行标签用，抽成英文 key 要把 25 个文件
/// 全部动一遍，而中文当键只需要一张表 —— 而且代码里读起来还是「Row("型号", ...)」，
/// 不是「Row("hw.model", ...)」，写的时候不用一边写一边编 key。
///
/// 查不到的原文原样返回。所以漏翻一条不会报错、不会显示成空白，只是那一行还是中文，
/// 对着界面就能看出来漏了哪条。
/// </summary>
public static class Translator
{
    /// <summary>默认语言。取值和采集端 <c>ui.language</c> 一致（BCP-47 短码）。</summary>
    public const string DefaultLanguage = "zh-Hans";

    public const string English = "en";

    private static readonly Dictionary<string, string> En = EnglishStrings.ByChinese;

    private static string _language = DefaultLanguage;

    /// <summary>语言变了。绑在界面上的东西靠这个重画。</summary>
    public static event EventHandler? LanguageChanged;

    public static string CurrentLanguage => _language;

    public static void SetLanguage(string? code)
    {
        var next = Normalize(code);
        if (next == _language) return;
        _language = next;
        LanguageChanged?.Invoke(null, EventArgs.Empty);
    }

    /// <summary>
    /// 把采集端那边的语言码归一。它可能给 <c>en</c> / <c>en-US</c> / <c>zh-Hans</c> / <c>zh</c>，
    /// 也可能给空（老版本采集端没有这个键）—— 给不出就维持默认，不要瞎猜成英文。
    /// </summary>
    public static string Normalize(string? code)
    {
        if (string.IsNullOrWhiteSpace(code)) return DefaultLanguage;
        return code.Trim().StartsWith("en", StringComparison.OrdinalIgnoreCase) ? English : DefaultLanguage;
    }

    /// <summary>取一条文案。<paramref name="zh"/> 就是中文原文。</summary>
    public static string T(string zh)
    {
        if (_language == DefaultLanguage || string.IsNullOrEmpty(zh)) return zh;
        return En.TryGetValue(zh, out var en) ? en : zh;
    }

    /// <summary>
    /// 带占位符的文案，占位符写 <c>{0}</c> <c>{1}</c>，形如「磁盘 {0}」。
    /// 先按整串查表，查不到就原样返回（<see cref="T(string)"/> 已经保证了这点）。
    /// </summary>
    public static string T(string zh, params object?[] args) =>
        string.Format(CultureInfo.InvariantCulture, T(zh), args);

    /// <summary>
    /// 把整张规格表翻一遍。
    ///
    /// 做成「建成之后整表再过一遍」而不是「建的时候逐条查表」：HardwareSpecBuilder 里有
    /// 115 处中文字面量，逐处改要动整个文件；而行标签、小标题、页底说明都是纯展示，
    /// 建成之后再翻一遍结果一样，builder 一行都不用动。
    ///
    /// 值也翻 —— 因为「是 / 否」「与硬件地址相同」和模型层的「未知磁盘」这类兜底文案
    /// 都写在值里，它们同样是给用户看的。真数据（型号、序列号、容量）不会正好等于某条
    /// 文案，查不到就原样返回。
    ///
    /// SMART 表的属性名不翻：那是硬盘报上来的原文。
    /// </summary>
    public static Models.SpecSheet TranslateSheet(Models.SpecSheet sheet)
    {
        if (_language == DefaultLanguage) return sheet;

        return sheet with
        {
            Blocks = sheet.Blocks.Select(TranslateBlock).ToArray(),
            Note = T(sheet.Note)
        };
    }

    private static Models.SpecBlock TranslateBlock(Models.SpecBlock block) =>
        block with
        {
            Title = T(block.Title),
            Units = block.Units.Select(u => u with
            {
                Subtitle = u.Subtitle is null ? null : T(u.Subtitle),
                Rows = u.Rows.Select(r => new Models.SpecRow(T(r.Label), T(r.Value), r.IsNa)).ToArray()
            }).ToArray()
        };
}
