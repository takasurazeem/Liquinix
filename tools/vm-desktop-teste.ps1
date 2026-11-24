# Roda DENTRO da VM (usuário comum): força o desktop (Progman) a redesenhar o papel de parede e anota o que existe.
# Resultado em C:\Users\Public\MacBarra-desktop.txt
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class W {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, string c, string t);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeoutW(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint t, out IntPtr r);
  [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr r, IntPtr g, uint f);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
}
'@
$o = @()
$pm = [IntPtr]::Zero; [W]::EnumWindows({ param($h,$l) $sb = New-Object System.Text.StringBuilder 64; [void][W]::GetClassNameW($h,$sb,64); if ($sb.ToString() -eq 'Progman') { $script:pm = $h }; return $true }, [IntPtr]::Zero) | Out-Null; $pm = $script:pm
$o += "Progman $pm visivel $([W]::IsWindowVisible($pm))"
$r = [IntPtr]::Zero
[void][W]::SendMessageTimeoutW($pm, 0x052C, [IntPtr]0xD, [IntPtr]1, 0, 1000, [ref]$r)
[void][W]::SendMessageTimeoutW($pm, 0x052C, [IntPtr]0, [IntPtr]0, 0, 1000, [ref]$r)
$o += "0x052C enviado"
$lista = New-Object System.Collections.Generic.List[string]
[W]::EnumWindows({ param($h, $l)
    $sb = New-Object System.Text.StringBuilder 64; [void][W]::GetClassNameW($h, $sb, 64)
    $c = $sb.ToString()
    if ($c -in 'Progman', 'WorkerW') { $rc = New-Object W+RECT; [void][W]::GetWindowRect($h, [ref]$rc); $def = [W]::FindWindowExW($h, [IntPtr]::Zero, 'SHELLDLL_DefView', $null); $lista.Add("$c $h visivel=$([W]::IsWindowVisible($h)) ($($rc.L),$($rc.T))-($($rc.R),$($rc.B)) defview=$def") }
    return $true }, [IntPtr]::Zero) | Out-Null
$o += $lista
[void][W]::RedrawWindow($pm, [IntPtr]::Zero, [IntPtr]::Zero, 0x0001 -bor 0x0004 -bor 0x0080 -bor 0x0100)
$o += "RedrawWindow feito"
$o | Set-Content 'C:\Users\Public\MacBarra-desktop.txt' -Encoding utf8
