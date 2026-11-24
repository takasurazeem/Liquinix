# Roda DENTRO da VM AuthUX-Teste (copiado para C:\SequoiaMac). Nunca rodar no PC real.
# Troca o visual do próprio Windows pelo do Mac, sem nenhum exe extra:
#   ícones dentro de C:\Windows\SystemResources\imageres.dll.mun, fontes SF Pro no lugar da Segoe UI,
#   cursor e sons do Mac, papel de parede/tela de bloqueio, tema claro com destaque azul, barra enxuta.
# Vale para todos os perfis existentes, para o perfil padrão (novos usuários) e para a tela de logon (.DEFAULT).
# Backup: C:\SequoiaMac\originais (registro.json, arquivos.json e cópias dos arquivos originais).
param([string]$Acao = 'estado')
$ErrorActionPreference = 'Continue'   # 'Stop' quebra com stderr de reg/icacls no PS 5.1
$B    = 'C:\SequoiaMac'
$orig = "$B\originais"
$regJ = "$orig\registro.json"
$arqJ = "$orig\arquivos.json"
$res  = "$env:WINDIR\Resources\macOS"
$out  = New-Object System.Collections.Generic.List[string]
function Say($s) { $out.Add([string]$s); Write-Output $s }
New-Item -ItemType Directory -Force $orig | Out-Null

Add-Type -ReferencedAssemblies System.Drawing @'
using System; using System.IO; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class MacWin {
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] static extern IntPtr BeginUpdateResource(string f, bool del);
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool UpdateResource(IntPtr h, IntPtr type, IntPtr name, ushort lang, byte[] data, uint cb);
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool EndUpdateResource(IntPtr h, bool discard);
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] static extern IntPtr LoadLibraryEx(string f, IntPtr h, uint fl);
  [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr h);
  delegate bool EnumLangProc(IntPtr m, IntPtr t, IntPtr n, ushort lang, IntPtr p);
  [DllImport("kernel32.dll")] static extern bool EnumResourceLanguages(IntPtr m, IntPtr t, IntPtr n, EnumLangProc cb, IntPtr p);
  delegate bool EnumNameProc(IntPtr m, IntPtr t, IntPtr n, IntPtr p);
  [DllImport("kernel32.dll")] static extern bool EnumResourceNames(IntPtr m, IntPtr t, EnumNameProc cb, IntPtr p);
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] public static extern bool MoveFileEx(string a, string b, int fl);
  [DllImport("shell32.dll")] public static extern void SHChangeNotify(int e, uint f, IntPtr a, IntPtr b);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern uint PrivateExtractIcons(string f, int i, int cx, int cy, IntPtr[] h, uint[] id, uint n, uint fl);

  // Troca os grupos de ícone (RT_GROUP_ICON) "ids" pelos .ico "icos" num arquivo de recursos (.mun/.dll).
  public static string PatchIcons(string file, int[] ids, string[] icos) {
    IntPtr m = LoadLibraryEx(file, IntPtr.Zero, 0x22);
    if (m == IntPtr.Zero) return "LoadLibraryEx falhou: " + Marshal.GetLastWin32Error();
    int maxIcon = 0;
    EnumResourceNames(m, (IntPtr)3, (a, t, n, p) => { long v = n.ToInt64(); if ((v >> 16) == 0 && v > maxIcon) maxIcon = (int)v; return true; }, IntPtr.Zero);
    var langs = new Dictionary<int, List<ushort>>();
    foreach (int id in ids) {
      var l = new List<ushort>();
      EnumResourceLanguages(m, (IntPtr)14, (IntPtr)id, (a, t, n, lg, p) => { l.Add(lg); return true; }, IntPtr.Zero);
      if (l.Count == 0) l.Add(0x409);
      langs[id] = l;
    }
    FreeLibrary(m);
    int next = Math.Max(maxIcon + 1, 40000);
    IntPtr h = BeginUpdateResource(file, false);
    if (h == IntPtr.Zero) return "BeginUpdateResource falhou: " + Marshal.GetLastWin32Error();
    for (int k = 0; k < ids.Length; k++) {
      byte[] b = File.ReadAllBytes(icos[k]);
      int count = BitConverter.ToUInt16(b, 4);
      var ms = new MemoryStream(); var w = new BinaryWriter(ms);
      w.Write((ushort)0); w.Write((ushort)1); w.Write((ushort)count);
      for (int i = 0; i < count; i++) {
        int o = 6 + 16 * i;
        ushort planes = BitConverter.ToUInt16(b, o + 4), bpp = BitConverter.ToUInt16(b, o + 6);
        uint size = BitConverter.ToUInt32(b, o + 8), off = BitConverter.ToUInt32(b, o + 12);
        byte[] img = new byte[size]; Array.Copy(b, off, img, 0, size);
        if (next > 65000) { EndUpdateResource(h, true); return "sem IDs livres"; }
        ushort nid = (ushort)next++;
        foreach (ushort lg in langs[ids[k]])
          if (!UpdateResource(h, (IntPtr)3, (IntPtr)nid, lg, img, size)) { int e = Marshal.GetLastWin32Error(); EndUpdateResource(h, true); return "UpdateResource RT_ICON falhou: " + e; }
        w.Write(b[o]); w.Write(b[o + 1]); w.Write(b[o + 2]); w.Write((byte)0);
        w.Write(planes == 0 ? (ushort)1 : planes); w.Write(bpp == 0 ? (ushort)32 : bpp); w.Write(size); w.Write(nid);
      }
      byte[] g = ms.ToArray();
      foreach (ushort lg in langs[ids[k]])
        if (!UpdateResource(h, (IntPtr)14, (IntPtr)ids[k], lg, g, (uint)g.Length)) { int e = Marshal.GetLastWin32Error(); EndUpdateResource(h, true); return "UpdateResource RT_GROUP_ICON falhou: " + e; }
    }
    if (!EndUpdateResource(h, false)) return "EndUpdateResource falhou: " + Marshal.GetLastWin32Error();
    return "ok";
  }
}
'@

# ---------------- registro (com backup) ----------------
$script:bk = New-Object System.Collections.Generic.List[object]
if (Test-Path $regJ) { foreach ($e in (Get-Content $regJ -Raw -Encoding utf8 | ConvertFrom-Json)) { $script:bk.Add($e) } }
$script:carregadas = @()
$script:avisos = @()

function Hives {
    # devolve @{tag; sub} onde sub é o caminho dentro de HKEY_USERS ('' = HKLM)
    $l = @(@{ tag = 'HKLM'; sub = $null }, @{ tag = '.DEFAULT'; sub = '.DEFAULT' })
    $dp = "$env:SystemDrive\Users\Default\NTUSER.DAT"
    reg.exe load 'HKU\SM_Default' $dp 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) { $script:carregadas += 'SM_Default'; $l += @{ tag = 'DEFAULT'; sub = 'SM_Default' } }
    foreach ($p in Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList') {
        $sid = $p.PSChildName
        if ($sid -notlike 'S-1-5-21-*') { continue }
        $path = $p.GetValue('ProfileImagePath')
        if ([Microsoft.Win32.Registry]::Users.OpenSubKey($sid)) { $l += @{ tag = "SID:$sid"; sub = $sid }; continue }
        $nt = Join-Path $path 'NTUSER.DAT'
        if (-not (Test-Path $nt)) { continue }
        reg.exe load "HKU\SM_$sid" $nt 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) { $script:carregadas += "SM_$sid"; $l += @{ tag = "SID:$sid"; sub = "SM_$sid" } }
        else { $script:avisos += "aviso: perfil $path não pôde ser carregado" }
    }
    $l
}
function Descarregar {
    [GC]::Collect(); [GC]::WaitForPendingFinalizers()
    foreach ($c in $script:carregadas) { reg.exe unload "HKU\$c" 2>&1 | Out-Null }
    $script:carregadas = @()
}
function Raiz($h) { if ($h.sub) { [Microsoft.Win32.Registry]::Users.OpenSubKey($h.sub, $true) } else { [Microsoft.Win32.Registry]::LocalMachine } }

function SetV($h, $key, $name, $val, $kind = 'String') {
    $root = Raiz $h
    $created = @()
    $parts = $key -split '\\'
    for ($i = 1; $i -le $parts.Length; $i++) {
        $p = ($parts[0..($i - 1)] -join '\'); $t = $root.OpenSubKey($p)
        if ($t) { $t.Close() } else { $created += $p }
    }
    [array]::Reverse($created)
    $k = $root.CreateSubKey($key)
    $ja = $script:bk | Where-Object { $_.tag -eq $h.tag -and $_.key -eq $key -and $_.name -eq $name }
    if (-not $ja) {
        $old = $k.GetValue($name, $null, 'DoNotExpandEnvironmentNames')
        $ok = if ($old -ne $null) { $k.GetValueKind($name).ToString() } else { $null }
        if ($old -is [byte[]]) { $old = [Convert]::ToBase64String($old) }
        $script:bk.Add([pscustomobject]@{ tag = $h.tag; key = $key; name = $name; existed = ($old -ne $null); old = $old; kind = $ok; created = $created })
    }
    $k.SetValue($name, $val, [Microsoft.Win32.RegistryValueKind]$kind)
    $k.Close()
    $shown = if ($val -is [byte[]]) { ($val | ForEach-Object { $_.ToString('X2') }) -join '' } else { $val }
    Say "$($h.tag)\$key [$(if ($name) { $name } else { '(Padrão)' })] -> $shown"
}
function SalvarRegistro { $script:bk | ConvertTo-Json -Depth 5 | Set-Content $regJ -Encoding utf8 }

# ---------------- arquivos do sistema (com backup) ----------------
$script:arqs = New-Object System.Collections.Generic.List[object]
if (Test-Path $arqJ) { foreach ($e in (Get-Content $arqJ -Raw -Encoding utf8 | ConvertFrom-Json)) { $script:arqs.Add($e) } }
function Liberar($f) {
    takeown.exe /f "$f" /a 2>&1 | Out-Null
    icacls.exe "$f" /grant '*S-1-5-32-544:F' 2>&1 | Out-Null
}
function Gravar($dest, $src) {
    # escreve $src em $dest; se o arquivo está em uso, renomeia o antigo e apaga ele no próximo boot
    try { Copy-Item $src $dest -Force -ErrorAction Stop }
    catch {
        $velho = "$dest.$([DateTime]::Now.Ticks).old"
        Rename-Item $dest $velho -ErrorAction Stop
        [MacWin]::MoveFileEx($velho, $null, 4) | Out-Null
        Copy-Item $src $dest -Force -ErrorAction Stop
    }
}
function Trocar($dest, $src) {
    $reg = $script:arqs | Where-Object { $_.dest -eq $dest }
    if (-not $reg) {
        $existed = Test-Path $dest
        $copia = Join-Path $orig ('arq-' + ($dest -replace '[:\\ ]', '_'))
        if ($existed) {
            Copy-Item $dest $copia -Force
            icacls.exe "$dest" /save "$copia.acl" 2>&1 | Out-Null
        }
        $script:arqs.Add([pscustomobject]@{ dest = $dest; existed = $existed; copia = $copia })
        $script:arqs | ConvertTo-Json -Depth 3 | Set-Content $arqJ -Encoding utf8
    }
    if (Test-Path $dest) { Liberar $dest }
    New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
    Gravar $dest $src
    Say "arquivo $dest -> $(Split-Path $src -Leaf)"
}
function DevolverAcl($dest, $copia) {
    if (Test-Path "$copia.acl") { icacls.exe (Split-Path $dest) /restore "$copia.acl" 2>&1 | Out-Null }
    icacls.exe "$dest" /setowner 'NT SERVICE\TrustedInstaller' 2>&1 | Out-Null
}

function LimparCacheIcones {
    foreach ($f in Get-ChildItem "$env:SystemDrive\Users\*\AppData\Local\Microsoft\Windows\Explorer\iconcache_*.db" -Force -ErrorAction SilentlyContinue) {
        try { Remove-Item $f.FullName -Force -ErrorAction Stop } catch { [MacWin]::MoveFileEx($f.FullName, $null, 4) | Out-Null }
    }
    try { [MacWin]::SHChangeNotify(0x08000000, 0x1000, [IntPtr]::Zero, [IntPtr]::Zero) } catch {}
    Say 'cache de ícones apagado (o resto some no próximo boot)'
}

# ======================================================================
if ($Acao -eq 'estado') {
    $v = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    Say "VM build $($v.CurrentBuild).$($v.UBR) | usuário da sessão: $env:USERNAME"
    Say ("sessões: " + ((quser.exe 2>&1) -join ' | '))
    Say ("backup registro: $(if (Test-Path $regJ) { $script:bk.Count } else { 0 }) valores | arquivos trocados: $($script:arqs.Count)")
    Say ("Shell = " + (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon').Shell)
    Say ("reinício pendente: " + [bool](Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending'))
    return
}

if ($Acao -eq 'desfazer') {
    $hs = Hives
    $map = @{}; foreach ($h in $hs) { $map[$h.tag] = $h }
    $lista = @($script:bk); [array]::Reverse($lista)
    foreach ($e in $lista) {
        $h = $map[$e.tag]; if (-not $h) { Say "aviso: hive $($e.tag) indisponível"; continue }
        $root = Raiz $h; $k = $root.OpenSubKey($e.key, $true)
        if (-not $k) { continue }
        if ($e.existed) {
            $v = $e.old; if ($e.kind -eq 'Binary') { $v = [Convert]::FromBase64String($v) }
            if ($e.kind -eq 'MultiString') { $v = [string[]]$v }
            $k.SetValue($e.name, $v, [Microsoft.Win32.RegistryValueKind]$e.kind)
        } else { try { $k.DeleteValue($e.name) } catch {} }
        $k.Close()
    }
    foreach ($e in $lista) {
        $h = $map[$e.tag]; if (-not $h) { continue }
        $root = Raiz $h
        foreach ($c in @($e.created)) {
            if (-not $c) { continue }
            $k = $root.OpenSubKey($c)
            if ($k -and $k.ValueCount -eq 0 -and $k.SubKeyCount -eq 0) { $k.Close(); try { $root.DeleteSubKey($c, $false) } catch {} } elseif ($k) { $k.Close() }
        }
    }
    Say "registro: $($lista.Count) valores devolvidos"
    Descarregar
    foreach ($a in $script:arqs) {
        if (Test-Path $a.dest) { Liberar $a.dest }
        if ($a.existed) { Gravar $a.dest $a.copia; DevolverAcl $a.dest $a.copia; Say "arquivo devolvido: $($a.dest)" }
        elseif (Test-Path $a.dest) {
            try { Remove-Item $a.dest -Force -ErrorAction Stop } catch { [MacWin]::MoveFileEx($a.dest, $null, 4) | Out-Null }
            Say "arquivo removido: $($a.dest)"
        }
    }
    LimparCacheIcones
    Remove-Item $regJ, $arqJ -ErrorAction SilentlyContinue
    Remove-Item "$orig\arq-*" -ErrorAction SilentlyContinue
    Say 'desfeito: reinicie a VM'
    return
}

if ($Acao -ne 'aplicar') { Say "ação desconhecida: $Acao"; return }

# ---------------- 0. recursos em C:\Windows\Resources\macOS ----------------
foreach ($d in 'icones','sons','cursor') { New-Item -ItemType Directory -Force "$res\$d" | Out-Null }
Copy-Item "$B\icones\*.ico" "$res\icones" -Force
Copy-Item "$B\cursor\*" "$res\cursor" -Force
Copy-Item "$B\sons\*.wav" "$res\sons" -Force
Say "recursos copiados para $res"
function I($n) { "$res\icones\$n.ico,0" }

# ---------------- 1. ícones dentro do imageres.dll.mun ----------------
$mapa = [ordered]@{
    2 = 'doc-generico'; 102 = 'doc-texto'; 22 = 'doc-audio'; 1026 = 'doc-audio'; 23 = 'doc-video'
    21 = 'doc-imagem'; 70 = 'doc-imagem'; 71 = 'doc-imagem'; 72 = 'doc-imagem'
    3 = 'pasta'; 4 = 'pasta'; 5 = 'pasta'; 123 = 'pasta'; 181 = 'pasta'; 185 = 'pasta'; 186 = 'pasta'; 1020 = 'pasta'; 1023 = 'pasta'
    109 = 'computador'
    30 = 'disco'; 32 = 'disco'; 35 = 'disco'; 36 = 'disco'
    37 = 'cd'; 38 = 'cd'; 39 = 'cd'; 40 = 'cd'; 41 = 'cd'; 56 = 'cd'; 61 = 'cd'; 62 = 'cd'; 63 = 'cd'; 64 = 'cd'; 180 = 'cd'
    54 = 'lixeira-cheia'; 55 = 'lixeira-vazia'
    108 = 'pasta-music'; 112 = 'pasta-docs'; 113 = 'pasta-pictures'; 183 = 'pasta-desktop'; 184 = 'pasta-downloads'; 189 = 'pasta-videos'; 1040 = 'pasta-cloud'
}
$mun = "$env:WINDIR\SystemResources\imageres.dll.mun"
$base = $mun
$ja = $script:arqs | Where-Object { $_.dest -eq $mun }
if ($ja) { $base = $ja.copia }   # reaplicar sempre a partir do original
New-Item -ItemType Directory -Force "$B\trabalho" | Out-Null
$tmp = "$B\trabalho\imageres.dll.mun"
Copy-Item $base $tmp -Force
$ids = [int[]]@($mapa.Keys); $icos = [string[]]@($mapa.Values | ForEach-Object { "$B\icones\$_.ico" })
if (-not ('Rsrc' -as [type])) { Add-Type -Path "$B\rsrc.cs" }   # UpdateResource recusa .mun (tem recurso MUI): reescrevemos a .rsrc
$r = [Rsrc]::Patch($tmp, $ids, $icos)
Say "imageres.dll.mun: patch dos ícones -> $r ($($ids.Count) grupos)"
if ($r -like 'ok*') { Trocar $mun $tmp }

# ---------------- 2. fontes SF Pro no lugar da Segoe UI ----------------
$hs = Hives
foreach ($a in $script:avisos) { Say $a }
$HKLM = $hs | Where-Object { $_.tag -eq 'HKLM' }
$fk = 'SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts'
foreach ($f in Get-ChildItem "$B\fontes\*.otf") {
    $dest = "$env:WINDIR\Fonts\$($f.Name)"
    if (-not (Test-Path $dest)) { Copy-Item $f.FullName $dest }
    SetV $HKLM $fk ("$($f.BaseName -replace '-', ' ') (OpenType)") $f.Name
}
$fonts = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($fk)
foreach ($n in $fonts.GetValueNames()) {
    # a Segoe UI Variable fica: a interface XAML do Windows (Terminal, busca, Configurações) cai sem ela (0xc000027b, visto em 11/11/2026)
    if ($n -like 'Segoe UI*' -and $n -notmatch 'Emoji|Symbol|Historic|Variable') { SetV $HKLM $fk $n '' }
}
$fonts.Close()
$sk = 'SOFTWARE\Microsoft\Windows NT\CurrentVersion\FontSubstitutes'
foreach ($p in @(
    @('Segoe UI', 'SF Pro Text'), @('Segoe UI Light', 'SF Pro Text'), @('Segoe UI Semibold', 'SF Pro Text'), @('Segoe UI Semilight', 'SF Pro Text'), @('Segoe UI Black', 'SF Pro Display'),
    @('MS Shell Dlg', 'SF Pro Text'), @('MS Shell Dlg 2', 'SF Pro Text'))) { SetV $HKLM $sk $p[0] $p[1] }

# ---------------- 3. cursor ----------------
$cur = [ordered]@{ Arrow = 'arrowS.cur'; Help = 'helpS.cur'; AppStarting = 'waitS.ani'; Wait = 'busyS.ani'; Crosshair = 'crossS.cur'; IBeam = 'beamS.cur'
    NWPen = 'penS.cur'; No = 'noS.cur'; SizeNS = 'HsizeS.cur'; SizeWE = 'VsizeS.cur'; SizeNWSE = 'D2sizeS.cur'; SizeNESW = 'D1sizeS.cur'
    SizeAll = 'moveS.cur'; UpArrow = 'upS.cur'; Hand = 'linkS.cur'; Pin = 'locationS.cur'; Person = 'personS.cur' }
$curPath = @{}; foreach ($k in $cur.Keys) { $curPath[$k] = "$res\cursor\$($cur[$k])" }
SetV $HKLM 'SOFTWARE\Microsoft\Windows\CurrentVersion\Control Panel\Cursors\Schemes' 'macOS Tahoe' ((@($cur.Keys) | ForEach-Object { $curPath[$_] }) -join ',') 'ExpandString'

# ---------------- 4. sons ----------------
$snd = [ordered]@{
    '.Default\.Default' = 'Tink'; '.Default\SystemAsterisk' = 'Glass'; '.Default\SystemExclamation' = 'Funk'; '.Default\SystemHand' = 'Sosumi'
    '.Default\SystemQuestion' = 'Purr'; '.Default\SystemNotification' = 'Glass'; '.Default\Notification.Default' = 'Glass'; '.Default\Notification.IM' = 'ReceivedMessage'
    '.Default\Notification.Mail' = 'new-mail'; '.Default\MailBeep' = 'new-mail'; '.Default\Notification.Reminder' = 'Glass'; '.Default\Notification.SMS' = 'ReceivedMessage'
    '.Default\DeviceConnect' = 'Volume_Mount'; '.Default\DeviceDisconnect' = 'Volume_Unmount'; '.Default\DeviceFail' = 'Basso'; '.Default\MessageNudge' = 'Pop'
    '.Default\WindowsUAC' = 'Ping'; '.Default\LowBatteryAlarm' = 'Sosumi'; '.Default\CriticalBatteryAlarm' = 'Basso'; '.Default\WindowsLogon' = ''
    '.Default\SystemExit' = ''; 'Explorer\EmptyRecycleBin' = 'empty_trash'; 'Explorer\Navigating' = ''; 'Explorer\MoveMenuItem' = ''
}

# ---------------- 5. papel de parede e tela de bloqueio ----------------
Add-Type -AssemblyName System.Drawing
$wp = "$env:WINDIR\Web\Wallpaper\macOS\sequoia.jpg"
New-Item -ItemType Directory -Force (Split-Path $wp) | Out-Null
Copy-Item "$B\papel\sequoia-floresta.jpg" $wp -Force
function Jpeg($src, $dst, $w, $h, $maxKB) {
    $img = [System.Drawing.Image]::FromFile($src)
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.InterpolationMode = 'HighQualityBicubic'
    $esc = [math]::Max($w / $img.Width, $h / $img.Height); $iw = $img.Width * $esc; $ih = $img.Height * $esc
    $g.DrawImage($img, ($w - $iw) / 2, ($h - $ih) / 2, $iw, $ih); $g.Dispose(); $img.Dispose()
    $enc = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object MimeType -eq 'image/jpeg'
    foreach ($q in 90, 80, 70, 60, 50, 40) {
        $ps = New-Object System.Drawing.Imaging.EncoderParameters 1
        $ps.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), ([long]$q)
        $bmp.Save($dst, $enc, $ps)
        if (-not $maxKB -or (Get-Item $dst).Length -lt $maxKB * 1024) { break }
    }
    $bmp.Dispose()
}
Jpeg $wp "$B\trabalho\fundo.jpg" 3840 2160 0
Jpeg $wp "$B\trabalho\fundo-logon.jpg" 1920 1080 250
foreach ($f in @(Get-ChildItem "$env:WINDIR\Web\Wallpaper\Windows\img0*.jpg") + @(Get-ChildItem "$env:WINDIR\Web\4K\Wallpaper\Windows\*.jpg" -ErrorAction SilentlyContinue) + @(Get-ChildItem "$env:WINDIR\Web\Screen\img100*.jpg" -ErrorAction SilentlyContinue)) {
    Trocar $f.FullName "$B\trabalho\fundo.jpg"
}
Trocar "$env:WINDIR\System32\oobe\info\backgrounds\backgroundDefault.jpg" "$B\trabalho\fundo-logon.jpg"
SetV $HKLM 'SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\LogonUI\Background' 'OEMBackground' 1 'DWord'
$csp = 'SOFTWARE\Microsoft\Windows\CurrentVersion\PersonalizationCSP'
SetV $HKLM $csp 'DesktopImagePath' $wp; SetV $HKLM $csp 'DesktopImageUrl' $wp; SetV $HKLM $csp 'DesktopImageStatus' 1 'DWord'
SetV $HKLM $csp 'LockScreenImagePath' $wp; SetV $HKLM $csp 'LockScreenImageUrl' $wp; SetV $HKLM $csp 'LockScreenImageStatus' 1 'DWord'

# ---------------- 6. ícones extras pelo registro da máquina ----------------
$si = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\Shell Icons'
foreach ($p in @(@('0', 'doc-generico'), @('1', 'doc-generico'), @('3', 'pasta'), @('4', 'pasta'), @('8', 'disco'), @('11', 'cd'), @('15', 'computador'), @('31', 'lixeira-vazia'), @('32', 'lixeira-cheia'))) {
    SetV $HKLM $si $p[0] (I $p[1])
}
# barra de tarefas/Widgets/Chat por política
SetV $HKLM 'SOFTWARE\Policies\Microsoft\Dsh' 'AllowNewsAndInterests' 0 'DWord'
SetV $HKLM 'SOFTWARE\Policies\Microsoft\Windows\Windows Chat' 'ChatIcon' 3 'DWord'

# ---------------- 7. por usuário (todos os perfis, perfil padrão e tela de logon) ----------------
$accent = [byte[]](0x99,0xC9,0xFF,0, 0x66,0xAE,0xFF,0, 0x33,0x95,0xFF,0, 0x00,0x7A,0xFF,0, 0x00,0x66,0xD9,0, 0x00,0x52,0xB0,0, 0x00,0x3E,0x86,0, 0x88,0x88,0x88,0)
foreach ($h in $hs | Where-Object { $_.tag -ne 'HKLM' }) {
    # cursor
    SetV $h 'Control Panel\Cursors' '' 'macOS Tahoe'
    foreach ($k in $cur.Keys) { SetV $h 'Control Panel\Cursors' $k $curPath[$k] 'ExpandString' }
    SetV $h 'Control Panel\Cursors' 'Scheme Source' 2 'DWord'
    # sons
    SetV $h 'AppEvents\Schemes' '' 'macos'
    SetV $h 'AppEvents\Schemes\Names\macos' '' 'macOS'
    foreach ($ev in $snd.Keys) {
        $arq = if ($snd[$ev]) { "$res\sons\$($snd[$ev]).wav" } else { '' }
        SetV $h "AppEvents\Schemes\Apps\$ev\.Current" '' $arq
        SetV $h "AppEvents\Schemes\Apps\$ev\macos" '' $arq
    }
    # papel de parede e cores de seleção
    SetV $h 'Control Panel\Desktop' 'Wallpaper' $wp
    SetV $h 'Control Panel\Desktop' 'WallpaperStyle' '10'
    SetV $h 'Control Panel\Desktop' 'TileWallpaper' '0'
    SetV $h 'Control Panel\Colors' 'Hilight' '0 122 255'
    SetV $h 'Control Panel\Colors' 'HotTrackingColor' '0 102 204'
    SetV $h 'Control Panel\Colors' 'MenuHilight' '0 122 255'
    if ($h.tag -eq '.DEFAULT') { continue }   # o resto não vale para a tela de logon
    # tema claro, transparência e azul do Mac
    $pz = 'Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
    SetV $h $pz 'AppsUseLightTheme' 1 'DWord'; SetV $h $pz 'SystemUsesLightTheme' 1 'DWord'
    SetV $h $pz 'EnableTransparency' 1 'DWord'; SetV $h $pz 'ColorPrevalence' 0 'DWord'
    $ac = 'Software\Microsoft\Windows\CurrentVersion\Explorer\Accent'
    SetV $h $ac 'AccentPalette' $accent 'Binary'
    SetV $h $ac 'AccentColorMenu' ([int]0xFFFF7A00) 'DWord'
    SetV $h $ac 'StartColorMenu' ([int]0xFFD96600) 'DWord'
    SetV $h 'Software\Microsoft\Windows\DWM' 'AccentColor' ([int]0xFFFF7A00) 'DWord'
    SetV $h 'Software\Microsoft\Windows\DWM' 'ColorizationColor' ([int]0xC4007AFF) 'DWord'
    SetV $h 'Software\Microsoft\Windows\DWM' 'ColorizationAfterglow' ([int]0xC4007AFF) 'DWord'
    SetV $h 'Software\Microsoft\Windows\DWM' 'ColorPrevalence' 0 'DWord'
    # barra de tarefas no centro e enxuta, como o Dock
    $adv = 'Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced'
    SetV $h $adv 'TaskbarAl' 1 'DWord'; SetV $h $adv 'ShowTaskViewButton' 0 'DWord'; SetV $h $adv 'TaskbarMn' 0 'DWord'
    SetV $h $adv 'ShowCopilotButton' 0 'DWord'; SetV $h $adv 'HideFileExt' 1 'DWord'
    try { SetV $h $adv 'TaskbarDa' 0 'DWord' } catch { Say "aviso: TaskbarDa bloqueado em $($h.tag) (a política já esconde os Widgets)" }
    SetV $h 'Software\Microsoft\Windows\CurrentVersion\Search' 'SearchboxTaskbarMode' 0 'DWord'
    # Lixeira (o ícone dela é por usuário)
    $rb = 'Software\Microsoft\Windows\CurrentVersion\Explorer\CLSID\{645FF040-5081-101B-9F08-00AA002F954E}\DefaultIcon'
    SetV $h $rb '' (I 'lixeira-cheia'); SetV $h $rb 'full' (I 'lixeira-cheia'); SetV $h $rb 'empty' (I 'lixeira-vazia')
}
SalvarRegistro
Descarregar
LimparCacheIcones

# ---------------- verificação: prancha com os ícones do novo imageres ----------------
try {
    $sheet = "$B\verificacao-icones.png"
    $bmp = New-Object System.Drawing.Bitmap (10 * 70), ([math]::Ceiling($ids.Count / 10) * 70)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear([System.Drawing.Color]::White)
    $fnt = New-Object System.Drawing.Font 'Arial', 8
    $i = 0
    foreach ($id in $ids) {
        $hh = New-Object IntPtr[] 1; $x = New-Object uint32[] 1
        $n = [MacWin]::PrivateExtractIcons($mun, -$id, 48, 48, $hh, $x, 1, 0)
        $cx = ($i % 10) * 70; $cy = [math]::Floor($i / 10) * 70
        if ($n -gt 0 -and $hh[0] -ne [IntPtr]::Zero) { $g.DrawIcon([System.Drawing.Icon]::FromHandle($hh[0]), $cx + 11, $cy + 2) }
        $g.DrawString("$id", $fnt, [System.Drawing.Brushes]::Black, $cx + 22, $cy + 54); $i++
    }
    $bmp.Save($sheet); Say "prancha de verificação: $sheet"
} catch { Say "aviso: prancha falhou: $($_.Exception.Message)" }
Say "pronto: $($script:bk.Count) valores no backup do registro, $($script:arqs.Count) arquivos trocados. Reinicie a VM."
