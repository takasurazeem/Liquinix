# Roda DENTRO da VM (C:\SequoiaMac\resolucao.ps1), na sessão aberta: muda a resolução do monitor (padrão 2560x1440).
# O Hyper-V precisa permitir a resolução (tools\vm-windows-mac.ps1 -Acao video2k). Grava o resultado em C:\SequoiaMac\resumo.txt.
param([int]$Largura = 2560, [int]$Altura = 1440)
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class Tela {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct DEVMODE {
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
    public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra; public int dmFields;
    public int dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
    public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
    public short dmLogPixels; public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
    public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
  }
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool EnumDisplaySettings(string dev, int mode, ref DEVMODE dm);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int ChangeDisplaySettings(ref DEVMODE dm, int flags);
  public static string Mudar(int w, int h) {
    DEVMODE dm = new DEVMODE(); dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
    EnumDisplaySettings(null, -1, ref dm);
    string antes = dm.dmPelsWidth + "x" + dm.dmPelsHeight;
    dm.dmPelsWidth = w; dm.dmPelsHeight = h; dm.dmFields = 0x80000 | 0x100000;
    int r = ChangeDisplaySettings(ref dm, 1);  // CDS_UPDATEREGISTRY
    return "resolução " + antes + " -> " + w + "x" + h + ": " + (r == 0 ? "ok" : "erro " + r);
  }
}
'@
[Tela]::Mudar($Largura, $Altura) | Set-Content C:\SequoiaMac\resumo.txt -Encoding utf8
Start-Process notepad.exe C:\SequoiaMac\resumo.txt
