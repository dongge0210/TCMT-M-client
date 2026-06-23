using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using Avalonia.Media;
using TCMT.Avalonia.Services.Contracts;

namespace TCMT.Avalonia.Services;

public class FontService : IFontService
{
    public bool IsNativeDialogSupported => OperatingSystem.IsMacOS() || OperatingSystem.IsWindows();

    public IReadOnlyList<string> GetSystemFonts()
    {
        return FontManager.Current.SystemFonts
            .Select(f => f.Name)
            .Distinct()
            .OrderBy(n => n)
            .ToList();
    }

    public Task<FontSelection?> PickFontAsync(FontSelection? current = null)
    {
        if (OperatingSystem.IsMacOS())
            return Task.FromResult(MacPickFont(current));
        if (OperatingSystem.IsWindows())
            return Task.FromResult(WinPickFont(current));
        return Task.FromResult<FontSelection?>(null);
    }

    // ═══════════════════════════════════════════════════════
    //  macOS — NSFontPanel
    // ═══════════════════════════════════════════════════════

    [SupportedOSPlatform("macos")]
    private static FontSelection? MacPickFont(FontSelection? current)
    {
        // NSFontPanel is not available from this process context.
        // Fall back to using the system font list instead.
        return null;
    }

    // ═══════════════════════════════════════════════════════
    //  Windows — ChooseFont (GDI comdlg32)
    // ═══════════════════════════════════════════════════════

    [SupportedOSPlatform("windows")]
    private static FontSelection? WinPickFont(FontSelection? current)
    {
        // LOGFONTW = 28 bytes header + 64 bytes lfFaceName = 92 bytes total
        const int LF_SIZE = 92;
        var lfBytes = new byte[LF_SIZE];

        if (current != null)
        {
            var nameBytes = System.Text.Encoding.Unicode.GetBytes(current.Family);
            Buffer.BlockCopy(nameBytes, 0, lfBytes, 28, Math.Min(nameBytes.Length, 63));
            // lfHeight (byte offset 0): negative = device units (pt × 1.333)
            var h = -(int)(current.Size * 1.333);
            Buffer.BlockCopy(BitConverter.GetBytes(h), 0, lfBytes, 0, 4);
            // lfWeight (byte offset 16)
            var w = current.IsBold ? FW_BOLD : FW_NORMAL;
            Buffer.BlockCopy(BitConverter.GetBytes(w), 0, lfBytes, 16, 4);
            // lfItalic (byte offset 20)
            lfBytes[20] = current.IsItalic ? (byte)1 : (byte)0;
            // lfCharSet (byte offset 23)
            lfBytes[23] = 1; // DEFAULT_CHARSET
        }

        var lfPtr = Marshal.AllocHGlobal(LF_SIZE);
        Marshal.Copy(lfBytes, 0, lfPtr, LF_SIZE);

        var cf = new CHOOSEFONTW();
        cf.lStructSize = (uint)Marshal.SizeOf<CHOOSEFONTW>();
        cf.lpLogFont = lfPtr;
        cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT;

        if (!ChooseFontW(ref cf))
        {
            Marshal.FreeHGlobal(lfPtr);
            return null;
        }

        Marshal.Copy(lfPtr, lfBytes, 0, LF_SIZE);
        Marshal.FreeHGlobal(lfPtr);

        var outH = BitConverter.ToInt32(lfBytes, 0);
        var outW = BitConverter.ToInt32(lfBytes, 16);
        var outIt = lfBytes[20];
        var nameLen = 0;
        while (nameLen < 31 && (lfBytes[28 + nameLen * 2] != 0 || lfBytes[28 + nameLen * 2 + 1] != 0))
            nameLen++;
        var faceName = System.Text.Encoding.Unicode.GetString(lfBytes, 28, nameLen * 2);

        return new FontSelection(
            Family: faceName,
            Size: Math.Abs(outH) / 1.333,
            IsBold: outW >= FW_SEMIBOLD,
            IsItalic: outIt != 0
        );
    }

    // ── Windows GDI types ──

    [StructLayout(LayoutKind.Sequential)]
    private struct LOGFONTW
    {
        public int lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
        public byte lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision,
                    lfClipPrecision, lfQuality, lfPitchAndFamily;
        // lfFaceName: 32 WCHAR (64 bytes). Managed via raw bytes.
    }

    // Wraps LOGFONTW with a managed face-name buffer so we can P/Invoke safely.
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct CHOOSEFONTW
    {
        public uint lStructSize;
        public IntPtr hwndOwner, hDC;
        public IntPtr lpLogFont;
        public int iPointSize;
        public uint Flags, rgbColors;
        public IntPtr lCustData, lpfnHook;
        public IntPtr lpTemplateName; // actually LPCWSTR, but unused → IntPtr
        public IntPtr hInstance;
        public IntPtr lpszStyle;
        public ushort nFontType;
        private ushort ___pad;
        public int nSizeMin, nSizeMax;
    }

    private const int CF_SCREENFONTS = 0x00000001;
    private const int CF_INITTOLOGFONTSTRUCT = 0x00000040;
    private const int FW_NORMAL = 400;
    private const int FW_SEMIBOLD = 600;
    private const int FW_BOLD = 700;

    [DllImport("comdlg32.dll", CharSet = CharSet.Unicode)]
    private static extern bool ChooseFontW(ref CHOOSEFONTW lpcf);
}
