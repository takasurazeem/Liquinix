# "Windows Mac" — troca o visual do próprio Windows pelo do Mac DENTRO da VM AuthUX-Teste (Hyper-V).
# Nada do PC real é alterado: tudo roda na VM via PowerShell Direct. Sem SequoiaShell, só recursos nativos.
# Uso (pede administrador):
#   tools\vm-windows-mac.ps1 -Acao estado|checkpoint|copiar|aplicar|desfazer|reiniciar|fotos
# O trabalho dentro da VM é feito por tools\vm-windows-mac-dentro.ps1 (copiado para C:\SequoiaMac na VM).
# Resultado de cada execução: docs\ultima-execucao-vm.txt
param([switch]$Admin, [string]$Acao = 'estado', [string]$Nome = 'AuthUX-Teste')
$root = Split-Path $PSScriptRoot -Parent
$out  = Join-Path $root 'docs\ultima-execucao-vm.txt'
$log  = Join-Path $root 'docs\registro-mudancas.txt'
if (-not $Admin) {
    Remove-Item $out -ErrorAction SilentlyContinue
    $p = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -WindowStyle Hidden -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Admin -Acao $Acao -Nome `"$Nome`""
    exit $p.ExitCode
}
$r = New-Object System.Collections.Generic.List[string]
function Cred {
    $sec = ConvertTo-SecureString 'teste123' -AsPlainText -Force
    New-Object System.Management.Automation.PSCredential('.\Teste', $sec)
}
function Nota($s) { Add-Content $log -Encoding utf8 ("`r`n[$(Get-Date -Format 'yyyy-MM-dd HH:mm')] Só na VM AuthUX-Teste (não no PC): " + $s) }
try {
    switch ($Acao) {
        'checkpoint' {
            Checkpoint-VM -Name $Nome -SnapshotName 'antes do Windows Mac'
            $r.Add('ponto de verificação "antes do Windows Mac" criado')
            Nota "ponto de verificação 'antes do Windows Mac' criado (tools\vm-windows-mac.ps1 -Acao checkpoint). Voltar: Restore-VMSnapshot -VMName AuthUX-Teste -Name 'antes do Windows Mac'."
        }
        'authux-mac' {
            # ponto de verificação + envia o AuthUX estilo Mac (Documents\AuthUX\x64\Release.GE) e o instalador dele
            if (-not (Get-VMSnapshot -VMName $Nome -Name 'antes do AuthUX Mac' -ErrorAction SilentlyContinue)) {
                Checkpoint-VM -Name $Nome -SnapshotName 'antes do AuthUX Mac'
                $r.Add('ponto de verificação "antes do AuthUX Mac" criado')
            }
            Enable-VMIntegrationService -VMName $Nome -Name 'Guest Service Interface'
            $dll = Join-Path (Split-Path $root -Parent) 'AuthUX\x64\Release.GE\AuthUX.dll'
            Copy-VMFile -Name $Nome -SourcePath $dll -DestinationPath 'C:\SequoiaMac\AuthUX-mac.dll' -FileSource Host -CreateFullPath -Force
            Copy-VMFile -Name $Nome -SourcePath (Join-Path $PSScriptRoot 'vm-resolucao.ps1') -DestinationPath 'C:\SequoiaMac\resolucao.ps1' -FileSource Host -CreateFullPath -Force
            # papel dinâmico do Tahoe (claro/escuro): vai para C:\SequoiaMac\tahoe e o authux-mac.ps1 põe em Windows\Web\Screen\macOS
            foreach ($f in Get-ChildItem (Join-Path $root 'icones\bloqueio') -File -Filter 'tahoe-*') {
                $marca = "C:\SequoiaMac\tahoe\$($f.Name)"
                Copy-VMFile -Name $Nome -SourcePath $f.FullName -DestinationPath $marca -FileSource Host -CreateFullPath -Force
                $r.Add("papel dinâmico enviado: $($f.Name) ($([int]($f.Length/1MB)) MB)")
            }
            Copy-VMFile -Name $Nome -SourcePath (Join-Path $PSScriptRoot 'vm-gpu-estado.ps1') -DestinationPath 'C:\SequoiaMac\gpu.ps1' -FileSource Host -CreateFullPath -Force
            Copy-VMFile -Name $Nome -SourcePath (Join-Path $PSScriptRoot 'vm-authux-mac-dentro.ps1') -DestinationPath 'C:\SequoiaMac\authux-mac.ps1' -FileSource Host -CreateFullPath -Force
            $r.Add("AuthUX-mac.dll ($((Get-Item $dll).Length) bytes) e authux-mac.ps1 enviados para C:\SequoiaMac")
            Nota "ponto de verificação 'antes do AuthUX Mac' e AuthUX estilo Mac enviado para C:\SequoiaMac (instalar na VM: C:\SequoiaMac\authux-mac.ps1 aplicar; voltar: authux-mac.ps1 desfazer ou restaurar o ponto de verificação)."
        }
        'copiar' {
            $s = New-PSSession -VMName $Nome -Credential (Cred) -ErrorAction Stop
            Invoke-Command -Session $s { foreach ($d in 'fontes','cursor','sons','icones','papel') { New-Item -ItemType Directory -Force "C:\SequoiaMac\$d" | Out-Null } }
            $dist = Join-Path $root 'dist\SequoiaShell-2026-11-10'
            Copy-Item (Join-Path $dist 'fontes\*') -Destination 'C:\SequoiaMac\fontes' -ToSession $s -Force
            Copy-Item (Join-Path $dist 'cursor\MacOS Tahoe Cursor (With Shadow)\*') -Destination 'C:\SequoiaMac\cursor' -ToSession $s -Force
            Copy-Item (Join-Path $root 'sons\mac\*.wav') -Destination 'C:\SequoiaMac\sons' -ToSession $s -Force
            Copy-Item (Join-Path $root 'bin\icones-sistema\*.ico') -Destination 'C:\SequoiaMac\icones' -ToSession $s -Force
            Copy-Item (Join-Path $root 'icones\bloqueio\sequoia-floresta.jpg') -Destination 'C:\SequoiaMac\papel' -ToSession $s -Force
            Copy-Item (Join-Path $PSScriptRoot 'vm-windows-mac-dentro.ps1') -Destination 'C:\SequoiaMac' -ToSession $s -Force
            $r.AddRange([string[]](Invoke-Command -Session $s { Get-ChildItem C:\SequoiaMac -Directory | ForEach-Object { "$($_.Name): $((Get-ChildItem $_.FullName -File).Count) arquivos" } }))
            Remove-PSSession $s
            Nota "recursos do Mac copiados para C:\SequoiaMac na VM (fontes SF Pro, cursor, sons, ícones .ico, papel de parede)."
        }
        { $_ -in 'aplicar','desfazer','estado' } {
            $s = New-PSSession -VMName $Nome -Credential (Cred) -ErrorAction Stop
            Copy-Item (Join-Path $PSScriptRoot 'vm-windows-mac-dentro.ps1') -Destination 'C:\SequoiaMac' -ToSession $s -Force -ErrorAction SilentlyContinue
            $res = Invoke-Command -Session $s -ArgumentList $Acao {
                param($a)
                if (-not (Test-Path C:\SequoiaMac\vm-windows-mac-dentro.ps1)) { return 'C:\SequoiaMac ainda não existe na VM (rode -Acao copiar)' }
                & C:\SequoiaMac\vm-windows-mac-dentro.ps1 -Acao $a 2>&1 | ForEach-Object { "$_" }
            }
            $r.AddRange([string[]]@($res))
            Remove-PSSession $s
            if ($Acao -eq 'aplicar') { Nota ("Windows trocado pelo visual do Mac de forma nativa (tools\vm-windows-mac.ps1 -Acao aplicar): ícones dentro de imageres.dll.mun, fontes SF Pro no lugar da Segoe UI, cursor e sons do Mac, papel de parede e tela de bloqueio, tema claro e destaque azul, barra de tarefas enxuta, para todos os usuários e para o perfil padrão. Originais e backup do registro em C:\SequoiaMac\originais da VM. Desfazer: tools\vm-windows-mac.ps1 -Acao desfazer (e reiniciar a VM) ou restaurar o ponto de verificação 'antes do Windows Mac'.`r`n" + (($res | Where-Object { $_ -like '*->*' -or $_ -like 'arquivo*' } | ForEach-Object { "    $_" }) -join "`r`n")) }
            if ($Acao -eq 'desfazer') { Nota "Windows Mac DESFEITO na VM (tools\vm-windows-mac.ps1 -Acao desfazer): registro e arquivos originais devolvidos a partir de C:\SequoiaMac\originais." }
        }
        'enviar' {
            # sem credencial: manda os scripts pela integração do Hyper-V (Guest Service Interface)
            Enable-VMIntegrationService -VMName $Nome -Name 'Guest Service Interface'
            Start-Sleep 3
            Copy-VMFile -Name $Nome -SourcePath (Join-Path $PSScriptRoot 'vm-windows-mac-dentro.ps1') -DestinationPath 'C:\SequoiaMac\vm-windows-mac-dentro.ps1' -FileSource Host -CreateFullPath -Force
            Copy-VMFile -Name $Nome -SourcePath (Join-Path $PSScriptRoot 'vm-windows-mac-rodar.ps1') -DestinationPath 'C:\SequoiaMac\rodar.ps1' -FileSource Host -CreateFullPath -Force
            Copy-VMFile -Name $Nome -SourcePath (Join-Path $PSScriptRoot 'vm-windows-mac-rsrc.cs') -DestinationPath 'C:\SequoiaMac\rsrc.cs' -FileSource Host -CreateFullPath -Force
            Copy-VMFile -Name $Nome -SourcePath (Join-Path $PSScriptRoot 'vm-windows-mac-conferir.ps1') -DestinationPath 'C:\SequoiaMac\conferir.ps1' -FileSource Host -CreateFullPath -Force
            $r.Add('scripts enviados para C:\SequoiaMac (dentro + rodar.ps1)')
        }
        'reiniciar' {
            Invoke-Command -VMName $Nome -Credential (Cred) { shutdown.exe /r /t 5 /f /c "Windows Mac: reiniciando para aplicar" }
            $r.Add('VM reiniciando (5 s)')
        }
        'video' {
            $v = Get-VMVideo -VMName $Nome
            $r.Add("vídeo: $($v.ResolutionType) $($v.HorizontalResolution)x$($v.VerticalResolution); VM: $((Get-VM $Nome).State)")
            $r.Add("sessão avançada no host: $((Get-VMHost).EnableEnhancedSessionMode)")
        }
        'video2k' {
            # 2560x1440 no monitor virtual (a VM precisa estar desligada para mudar)
            if ((Get-VM $Nome).State -ne 'Off') { Stop-VM -Name $Nome -Force; $r.Add('VM desligada (desligamento normal do Windows)') }
            Set-VMVideo -VMName $Nome -HorizontalResolution 2560 -VerticalResolution 1440 -ResolutionType Maximum
            Start-VM -Name $Nome
            $v = Get-VMVideo -VMName $Nome
            $r.Add("vídeo agora: $($v.ResolutionType) $($v.HorizontalResolution)x$($v.VerticalResolution); VM ligada")
            Nota "monitor virtual da VM com até 2560x1440 (Set-VMVideo -ResolutionType Maximum). Voltar: Set-VMVideo -VMName AuthUX-Teste -HorizontalResolution 1920 -VerticalResolution 1080 com a VM desligada."
        }
        'gpu' {
            # só consulta: GPUs do PC que o Hyper-V consegue dividir (GPU-P) e o que a VM tem hoje
            $r.Add("GPUs particionáveis no PC: " + ((Get-VMHostPartitionableGpu -ErrorAction SilentlyContinue | ForEach-Object { $_.Name }) -join ' | '))
            $r.Add("GPU na VM: " + ((Get-VMGpuPartitionAdapter -VMName $Nome -ErrorAction SilentlyContinue | Measure-Object).Count) + " adaptador(es)")
            $r.Add("VM: geração $((Get-VM $Nome).Generation), checkpoints: $((Get-VMSnapshot -VMName $Nome | Measure-Object).Count)")
            $m = Get-VMMemory -VMName $Nome
            $r.Add("memória: dinâmica=$($m.DynamicMemoryEnabled), inicial=$([int]($m.Startup/1MB)) MB")
            $a = Get-VMGpuPartitionAdapter -VMName $Nome -ErrorAction SilentlyContinue
            if ($a) { $r.Add("adaptador: VRAM $($a.MinPartitionVRAM)-$($a.MaxPartitionVRAM)") }
            $v = Get-VM $Nome
            $r.Add("MMIO: baixo $([int]($v.LowMemoryMappedIoSpace/1MB)) MB, alto $([int]($v.HighMemoryMappedIoSpace/1MB)) MB, cache $($v.GuestControlledCacheTypes)")
        }
        'gpu-ligar' {
            # GPU-P: divide a placa NVIDIA do PC com a VM e copia o driver do PC para dentro dela (HostDriverStore).
            # A VM é desligada; com a VM desligada também instala o AuthUX novo direto no disco dela.
            # Desfazer: tools\vm-windows-mac.ps1 -Acao gpu-desligar (tira a placa; os arquivos copiados ficam inertes).
            $vm = Get-VM $Nome
            if ($vm.State -ne 'Off') { Stop-VM -Name $Nome -Force; $r.Add('VM desligada') }
            $gpu = Get-VMHostPartitionableGpu | Where-Object Name -like '*VEN_10DE*' | Select-Object -First 1
            if (-not $gpu) { throw 'placa NVIDIA particionável não encontrada' }
            if (-not (Get-VMGpuPartitionAdapter -VMName $Nome -ErrorAction SilentlyContinue)) {
                Add-VMGpuPartitionAdapter -VMName $Nome -InstancePath $gpu.Name
            }
            Set-VMGpuPartitionAdapter -VMName $Nome -MinPartitionVRAM 80000000 -MaxPartitionVRAM 100000000 -OptimalPartitionVRAM 100000000 `
                -MinPartitionEncode 80000000 -MaxPartitionEncode 100000000 -OptimalPartitionEncode 100000000 `
                -MinPartitionDecode 80000000 -MaxPartitionDecode 100000000 -OptimalPartitionDecode 100000000 `
                -MinPartitionCompute 80000000 -MaxPartitionCompute 100000000 -OptimalPartitionCompute 100000000
            Set-VM -VMName $Nome -GuestControlledCacheTypes $true -LowMemoryMappedIoSpace 1GB -HighMemoryMappedIoSpace 32GB -AutomaticStopAction TurnOff
            $r.Add("placa ligada na VM: NVIDIA ($((Get-CimInstance Win32_VideoController | Where-Object PNPDeviceID -like '*VEN_10DE*').Name))")

            # disco da VM montado no PC só durante a cópia
            $vhd = (Get-VMHardDiskDrive -VMName $Nome | Select-Object -First 1).Path
            $disk = Mount-VHD -Path $vhd -PassThru | Get-Disk
            try {
                $win = $null
                foreach ($p in ($disk | Get-Partition)) {
                    if (-not $p.DriveLetter) { $p | Add-PartitionAccessPath -AssignDriveLetter -ErrorAction SilentlyContinue; $p = Get-Partition -DiskNumber $p.DiskNumber -PartitionNumber $p.PartitionNumber }
                    if ($p.DriveLetter -and (Test-Path "$($p.DriveLetter):\Windows\System32")) { $win = "$($p.DriveLetter):" }
                }
                if (-not $win) { throw 'Windows da VM não encontrado no disco' }
                # arquivos do driver NVIDIA do PC (mesmo método do Easy-GPU-PV)
                $dev = Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceID -like '*VEN_10DE*' -and $_.DeviceClass -eq 'DISPLAY' } | Select-Object -First 1
                $files = Get-CimAssociatedInstance -InputObject (Get-CimInstance Win32_PnPSignedDriver -Filter "DeviceID='$($dev.DeviceID.Replace('\','\\'))'") -ResultClassName Win32_PnPSignedDriverCIMDataFile -ErrorAction SilentlyContinue
                $n = 0
                foreach ($f in $files) {
                    $src = $f.Name
                    if ($src -match '\\DriverStore\\FileRepository\\') { $dst = $win + ($src.Substring(2) -replace '\\DriverStore\\FileRepository\\', '\HostDriverStore\FileRepository\') }
                    else { $dst = $win + $src.Substring(2) }
                    if (-not (Test-Path $src)) { continue }
                    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
                    Copy-Item $src $dst -Force -ErrorAction SilentlyContinue
                    $n++
                }
                # a pasta inteira do driver na FileRepository (alguns arquivos não aparecem na lista)
                $inf = (Get-CimInstance Win32_PnPSignedDriver | Where-Object DeviceID -eq $dev.DeviceID).InfName
                $repo = Get-ChildItem "$env:WINDIR\System32\DriverStore\FileRepository" -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'nvlddmkm.sys') } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
                if ($repo) { Copy-Item $repo.FullName "$win\Windows\System32\HostDriverStore\FileRepository\" -Recurse -Force; $r.Add("driver: $($repo.Name)") }
                $r.Add("arquivos do driver copiados: $n (inf $inf)")
                # AuthUX novo direto no System32 da VM
                $dll = Join-Path (Split-Path $root -Parent) 'AuthUX\x64\Release.GE\AuthUX.dll'
                Copy-Item $dll "$win\Windows\System32\AuthUX.dll" -Force
                $r.Add("AuthUX.dll novo instalado no disco da VM ($((Get-Item $dll).Length) bytes)")
            } finally {
                Dismount-VHD -Path $vhd
            }
            Start-VM -Name $Nome
            $r.Add('VM ligada de novo')
            Nota "GPU-P: placa NVIDIA do PC dividida com a VM (Add-VMGpuPartitionAdapter, MMIO 1GB/32GB, AutomaticStopAction TurnOff) e driver do PC copiado para Windows\System32\HostDriverStore da VM com o disco montado no PC só durante a cópia. Com GPU-P o Hyper-V não cria pontos de verificação novos. Desfazer: tools\vm-windows-mac.ps1 -Acao gpu-desligar."
        }
        'gpu-arquivos' {
            # arquivos do driver NVIDIA que ficam no System32/SysWOW64 do PC (nvapi64.dll etc.): sem eles a placa
            # aparece com erro na VM. Copiados pela integração do Hyper-V com a VM ligada; vale depois de reiniciar.
            $n = 0
            foreach ($dir in 'System32', 'SysWOW64') {
                foreach ($f in Get-ChildItem "$env:WINDIR\$dir" -File -Filter 'nv*' -ErrorAction SilentlyContinue) {
                    # System32 da VM não aceita cópia direta: vai para C:\SequoiaMac\nv e o authux-mac.ps1 move (como administrador)
                    try { Copy-VMFile -Name $Nome -SourcePath $f.FullName -DestinationPath "C:\SequoiaMac\nv\$dir\$($f.Name)" -FileSource Host -CreateFullPath -Force -ErrorAction Stop; $n++ }
                    catch { $r.Add("falhou $dir\$($f.Name): $($_.Exception.Message)") }
                }
            }
            $r.Add("arquivos nv* copiados para a VM: $n")
            Nota "GPU-P: arquivos nv* do System32/SysWOW64 do PC copiados para a VM ($n arquivos)."
        }
        'resgate' {
            # VM trancada na tela de logon: desliga, monta o disco no PC e devolve o AuthUX do Windows 7 guardado
            if ((Get-VM $Nome).State -ne 'Off') { Stop-VM -Name $Nome -Force; $r.Add('VM desligada') }
            $vhd = (Get-VMHardDiskDrive -VMName $Nome | Select-Object -First 1).Path
            $disk = Mount-VHD -Path $vhd -PassThru | Get-Disk
            try {
                $win = $null
                foreach ($p in ($disk | Get-Partition)) {
                    if (-not $p.DriveLetter) { $p | Add-PartitionAccessPath -AssignDriveLetter -ErrorAction SilentlyContinue; $p = Get-Partition -DiskNumber $p.DiskNumber -PartitionNumber $p.PartitionNumber }
                    if ($p.DriveLetter -and (Test-Path "$($p.DriveLetter):\Windows\System32")) { $win = "$($p.DriveLetter):" }
                }
                if (-not $win) { throw 'Windows da VM não encontrado' }
                $orig = "$win\SequoiaMac\originais\AuthUX-win7.dll"
                if (-not (Test-Path $orig)) { throw "original não encontrado em $orig" }
                Copy-Item $orig "$win\Windows\System32\AuthUX.dll" -Force
                $r.Add("AuthUX do Windows 7 devolvido ($((Get-Item $orig).Length) bytes)")
            } finally { Dismount-VHD -Path $vhd }
            Start-VM -Name $Nome
            $r.Add('VM ligada')
            Nota "resgate: AuthUX do Windows 7 devolvido com o disco montado no PC (a versão Mac escondia a mensagem de bloqueio do PIN)."
        }
        'authux-offline' {
            # instala o AuthUX estilo Mac (Documents\AuthUX\x64\Release.GE) com a VM desligada e o disco montado no PC
            if ((Get-VM $Nome).State -ne 'Off') { Stop-VM -Name $Nome -Force; $r.Add('VM desligada') }
            $vhd = (Get-VMHardDiskDrive -VMName $Nome | Select-Object -First 1).Path
            $disk = Mount-VHD -Path $vhd -PassThru | Get-Disk
            try {
                $win = $null
                foreach ($p in ($disk | Get-Partition)) {
                    if (-not $p.DriveLetter) { $p | Add-PartitionAccessPath -AssignDriveLetter -ErrorAction SilentlyContinue; $p = Get-Partition -DiskNumber $p.DiskNumber -PartitionNumber $p.PartitionNumber }
                    if ($p.DriveLetter -and (Test-Path "$($p.DriveLetter):\Windows\System32")) { $win = "$($p.DriveLetter):" }
                }
                if (-not $win) { throw 'Windows da VM não encontrado' }
                $dll = Join-Path (Split-Path $root -Parent) 'AuthUX\x64\Release.GE\AuthUX.dll'
                Copy-Item $dll "$win\Windows\System32\AuthUX.dll" -Force
                $r.Add("AuthUX Mac instalado ($((Get-Item $dll).Length) bytes)")
                # transições pretas da sessão (rundll32 AuthUX.dll,MacFadeIn no Userinit) e sem a tela de "apps impedindo
                # o desligamento" (BlockedShutdownResolverUX -> AuthUX, que fecha os apps como o Mac)
                $hive = 'HKLM\VM_SOFT'
                reg.exe load $hive "$win\Windows\System32\config\SOFTWARE" | Out-Null
                try {
                    Add-Type -ErrorAction SilentlyContinue @'
using System; using System.Runtime.InteropServices;
public static class RegOff {
  [DllImport("advapi32.dll", CharSet=CharSet.Unicode)] static extern int RegCreateKeyExW(IntPtr hKey, string sub, int res, string cls, int opt, int sam, IntPtr sa, out IntPtr result, out int disp);
  [DllImport("advapi32.dll", CharSet=CharSet.Unicode)] static extern int RegSetValueExW(IntPtr h, string name, int res, int type, byte[] data, int cb);
  [DllImport("advapi32.dll", CharSet=CharSet.Unicode)] static extern int RegQueryValueExW(IntPtr h, string name, IntPtr res, out int type, byte[] data, ref int cb);
  [DllImport("advapi32.dll")] static extern int RegCloseKey(IntPtr h);
  [DllImport("advapi32.dll", SetLastError=true)] static extern bool OpenProcessToken(IntPtr p, int acc, out IntPtr tok);
  [DllImport("advapi32.dll", SetLastError=true, CharSet=CharSet.Unicode)] static extern bool LookupPrivilegeValueW(string sys, string name, out long luid);
  [StructLayout(LayoutKind.Sequential, Pack=4)] struct TP { public int Count; public long Luid; public int Attr; }
  [DllImport("advapi32.dll", SetLastError=true)] static extern bool AdjustTokenPrivileges(IntPtr tok, bool dis, ref TP n, int len, IntPtr p, IntPtr r);
  [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
  public static void Enable(string n) { IntPtr t; OpenProcessToken(GetCurrentProcess(), 0x28, out t); TP tp = new TP(); tp.Count = 1; LookupPrivilegeValueW(null, n, out tp.Luid); tp.Attr = 2; AdjustTokenPrivileges(t, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero); }
  public static string Get(string sub, string name) {
    IntPtr h; int d; IntPtr hklm = new IntPtr(unchecked((int)0x80000002));
    if (RegCreateKeyExW(hklm, sub, 0, null, 4, 0x20019, IntPtr.Zero, out h, out d) != 0) return null;
    byte[] b = new byte[4096]; int cb = b.Length; int type;
    int r = RegQueryValueExW(h, name, IntPtr.Zero, out type, b, ref cb); RegCloseKey(h);
    if (r != 0) return null; return System.Text.Encoding.Unicode.GetString(b, 0, cb).TrimEnd('\0');
  }
  public static int Set(string sub, string name, string val, int type) {
    IntPtr h; int d; IntPtr hklm = new IntPtr(unchecked((int)0x80000002));
    int r = RegCreateKeyExW(hklm, sub, 0, null, 4, 0x20006, IntPtr.Zero, out h, out d); if (r != 0) return r;
    byte[] b = System.Text.Encoding.Unicode.GetBytes(val + "\0");
    r = RegSetValueExW(h, name, 0, type, b, b.Length); RegCloseKey(h); return r;
  }
}
'@
                    [RegOff]::Enable('SeRestorePrivilege'); [RegOff]::Enable('SeBackupPrivilege')
                    $orig = "$win\SequoiaMac\originais"
                    New-Item -ItemType Directory -Force $orig | Out-Null
                    $wl = 'VM_SOFT\Microsoft\Windows NT\CurrentVersion\Winlogon'
                    $ui = [RegOff]::Get($wl, 'Userinit')
                    if (-not (Test-Path "$orig\userinit.txt")) { Set-Content "$orig\userinit.txt" $ui -Encoding utf8 }
                    if ($ui -notmatch 'MacFadeIn') {
                        $novo = $ui.TrimEnd(',') + ',rundll32.exe C:\Windows\System32\AuthUX.dll,MacFadeIn,'
                        $r.Add("Userinit -> $novo : " + [RegOff]::Set($wl, 'Userinit', $novo, 1))
                    }
                    $bs = 'VM_SOFT\Microsoft\WindowsRuntime\ActivatableClassId\Windows.Internal.UI.Logon.Controller.BlockedShutdownResolverUX'
                    $dp = [RegOff]::Get($bs, 'DllPath')
                    if (-not (Test-Path "$orig\bsdr.txt")) { Set-Content "$orig\bsdr.txt" $dp -Encoding utf8 }
                    $r.Add("BlockedShutdownResolverUX DllPath (antes $dp) -> AuthUX : " + [RegOff]::Set($bs, 'DllPath', '%SystemRoot%\System32\AuthUX.dll', 2))
                } finally { [GC]::Collect(); reg.exe unload $hive | Out-Null }
                $web = "$win\Windows\Web\Screen\macOS"
                New-Item -ItemType Directory -Force $web | Out-Null
                Copy-Item (Join-Path $root 'icones\bloqueio\tahoe-*') $web -Force
                $r.Add("papel dinâmico do Tahoe em Windows\Web\Screen\macOS")
            } finally { Dismount-VHD -Path $vhd }
            Start-VM -Name $Nome
            $r.Add('VM ligada')
            Nota "AuthUX Mac (mensagens do Windows a partir do 3º PIN errado; bloco fixo) instalado com o disco da VM montado no PC. Desfazer: -Acao resgate."
        }
        'gpu-driver' {
            # refaz o driver NVIDIA da VM (GPU-P): VM desligada, disco montado no PC.
            #  - apaga drivers NVIDIA que o Windows Update instalou dentro da VM (DriverStore\FileRepository\nv*)
            #  - recopia a pasta do driver do PC para Windows\System32\HostDriverStore\FileRepository
            #  - copia os nv*.dll/exe do System32 e SysWOW64 do PC
            #  - bloqueia drivers pelo Windows Update na VM (política ExcludeWUDriversInQualityUpdate)
            if ((Get-VM $Nome).State -ne 'Off') { Stop-VM -Name $Nome -Force; $r.Add('VM desligada') }
            $vhd = (Get-VMHardDiskDrive -VMName $Nome | Select-Object -First 1).Path
            $disk = Mount-VHD -Path $vhd -PassThru | Get-Disk
            $hive = $null
            try {
                $win = $null
                foreach ($p in ($disk | Get-Partition)) {
                    if (-not $p.DriveLetter) { $p | Add-PartitionAccessPath -AssignDriveLetter -ErrorAction SilentlyContinue; $p = Get-Partition -DiskNumber $p.DiskNumber -PartitionNumber $p.PartitionNumber }
                    if ($p.DriveLetter -and (Test-Path "$($p.DriveLetter):\Windows\System32")) { $win = "$($p.DriveLetter):" }
                }
                if (-not $win) { throw 'Windows da VM não encontrado' }
                $bak = "$win\SequoiaMac\originais\drivers-nvidia-vm"
                New-Item -ItemType Directory -Force $bak | Out-Null
                # 1) drivers NVIDIA da própria VM (vindos do Windows Update): guardados e removidos
                foreach ($d in Get-ChildItem "$win\Windows\System32\DriverStore\FileRepository" -Directory -Filter 'nv*' -ErrorAction SilentlyContinue) {
                    if (Test-Path (Join-Path $d.FullName 'nvlddmkm.sys')) {
                        takeown.exe /f $d.FullName /r /d y /a 2>&1 | Out-Null
                        icacls.exe $d.FullName /grant '*S-1-5-32-544:F' /t /q 2>&1 | Out-Null
                        Move-Item $d.FullName (Join-Path $bak $d.Name) -Force
                        $r.Add("driver NVIDIA da VM removido (guardado): $($d.Name)")
                    }
                }
                # 2) pasta do driver do PC
                $repo = Get-ChildItem "$env:WINDIR\System32\DriverStore\FileRepository" -Directory -Filter 'nv*' | Where-Object { Test-Path (Join-Path $_.FullName 'nvlddmkm.sys') } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
                $hds = "$win\Windows\System32\HostDriverStore\FileRepository"
                if (Test-Path $hds) {
                    takeown.exe /f "$win\Windows\System32\HostDriverStore" /r /d y /a 2>&1 | Out-Null
                    icacls.exe "$win\Windows\System32\HostDriverStore" /grant '*S-1-5-32-544:F' /t /q 2>&1 | Out-Null
                    Remove-Item "$hds\*" -Recurse -Force -ErrorAction SilentlyContinue
                }
                New-Item -ItemType Directory -Force $hds | Out-Null
                Copy-Item $repo.FullName $hds -Recurse -Force
                $n = (Get-ChildItem (Join-Path $hds $repo.Name) -Recurse -File).Count
                $r.Add("driver do PC copiado: $($repo.Name) ($n arquivos)")
                # 3) nv* do System32 e SysWOW64
                $k = 0
                foreach ($dir in 'System32', 'SysWOW64') {
                    foreach ($f in Get-ChildItem "$env:WINDIR\$dir" -File -Filter 'nv*' -ErrorAction SilentlyContinue) {
                        $dst = "$win\Windows\$dir\$($f.Name)"
                        if (Test-Path $dst) { takeown.exe /f $dst /a 2>&1 | Out-Null; icacls.exe $dst /grant '*S-1-5-32-544:F' /q 2>&1 | Out-Null }
                        try { Copy-Item $f.FullName $dst -Force -ErrorAction Stop; $k++ } catch { $r.Add("falhou $dir\$($f.Name)") }
                    }
                }
                $r.Add("arquivos nv* de System32/SysWOW64 copiados: $k")
                # 4) sem drivers pelo Windows Update (registro SOFTWARE da VM)
                $hive = 'HKLM\VM_SOFTWARE'
                reg.exe load $hive "$win\Windows\System32\config\SOFTWARE" | Out-Null
                reg.exe add "$hive\Policies\Microsoft\Windows\WindowsUpdate" /v ExcludeWUDriversInQualityUpdate /t REG_DWORD /d 1 /f | Out-Null
                reg.exe add "$hive\Microsoft\Windows\CurrentVersion\DriverSearching" /v SearchOrderConfig /t REG_DWORD /d 0 /f | Out-Null
                $r.Add('Windows Update da VM sem drivers (ExcludeWUDriversInQualityUpdate=1, SearchOrderConfig=0)')
            } finally {
                if ($hive) { [GC]::Collect(); reg.exe unload $hive | Out-Null }
                Dismount-VHD -Path $vhd
            }
            Start-VM -Name $Nome
            $r.Add('VM ligada')
            Nota "GPU-P: driver NVIDIA da VM refeito (drivers do Windows Update guardados em C:\SequoiaMac\originais\drivers-nvidia-vm, driver do PC recopiado para HostDriverStore, nv* de System32/SysWOW64, Windows Update sem drivers). Desfazer: tools\vm-windows-mac.ps1 -Acao gpu-desligar."
        }
        'log-offline' {
            # lê o fim do log do AuthUX Mac com a VM desligada (disco montado no PC) e liga de novo
            if ((Get-VM $Nome).State -ne 'Off') { Stop-VM -Name $Nome -Force }
            $vhd = (Get-VMHardDiskDrive -VMName $Nome | Select-Object -First 1).Path
            $disk = Mount-VHD -Path $vhd -PassThru | Get-Disk
            try {
                foreach ($p in ($disk | Get-Partition)) {
                    if (-not $p.DriveLetter) { $p | Add-PartitionAccessPath -AssignDriveLetter -ErrorAction SilentlyContinue; $p = Get-Partition -DiskNumber $p.DiskNumber -PartitionNumber $p.PartitionNumber }
                    $f = "$($p.DriveLetter):\Users\Public\AuthUX-mac.log"
                    if ($p.DriveLetter -and (Test-Path $f)) {
                        Copy-Item $f (Join-Path $root 'docs\authux-mac-vm.log') -Force
                        $r.AddRange([string[]](Get-Content $f -Tail 40 -Encoding UTF8))
                    }
                }
            } finally { Dismount-VHD -Path $vhd }
            Start-VM -Name $Nome
        }
        { $_ -in 'shell-instalar', 'shell-desfazer' } {
            # SequoiaShell como shell do Windows na VM (no lugar do explorer.exe), com a VM desligada:
            #  - programas em C:\SequoiaShell (bin sem obj/obj32/logs, icones, sons)
            #  - Winlogon\Shell (HKLM e de cada usuário, inclusive o perfil padrão) -> SequoiaShell.exe
            #  - ajustes de cada usuário: explorer=0 (sem Explorador), finder_padrao=1 (pastas no Finder)
            # shell-desfazer devolve os valores guardados em C:\SequoiaMac\originais\shell-*.txt
            $desfazer = $Acao -eq 'shell-desfazer'
            if ((Get-VM $Nome).State -ne 'Off') { Stop-VM -Name $Nome -Force; $r.Add('VM desligada') }
            $vhd = (Get-VMHardDiskDrive -VMName $Nome | Select-Object -First 1).Path
            $disk = Mount-VHD -Path $vhd -PassThru | Get-Disk
            $loaded = @()
            try {
                $win = $null
                foreach ($p in ($disk | Get-Partition)) {
                    if (-not $p.DriveLetter) { $p | Add-PartitionAccessPath -AssignDriveLetter -ErrorAction SilentlyContinue; $p = Get-Partition -DiskNumber $p.DiskNumber -PartitionNumber $p.PartitionNumber }
                    if ($p.DriveLetter -and (Test-Path "$($p.DriveLetter):\Windows\System32")) { $win = "$($p.DriveLetter):" }
                }
                if (-not $win) { throw 'Windows da VM não encontrado' }
                $orig = "$win\SequoiaMac\originais"
                New-Item -ItemType Directory -Force $orig | Out-Null
                $exe = 'C:\SequoiaShell\bin\SequoiaShell.exe'
                if (-not $desfazer) {
                    $dst = "$win\SequoiaShell"
                    New-Item -ItemType Directory -Force "$dst\bin" | Out-Null
                    robocopy (Join-Path $root 'bin') "$dst\bin" /E /XD obj obj32 /XF *.log /NFL /NDL /NJH /NJS /NP | Out-Null
                    robocopy (Join-Path $root 'icones') "$dst\icones" /E /NFL /NDL /NJH /NJS /NP | Out-Null
                    robocopy (Join-Path $root 'sons') "$dst\sons" /E /NFL /NDL /NJH /NJS /NP | Out-Null
                    $r.Add("programas copiados para C:\SequoiaShell ($((Get-ChildItem $dst -Recurse -File).Count) arquivos)")
                }
                # HKLM
                $hive = 'HKLM\VM_SOFT2'
                reg.exe load $hive "$win\Windows\System32\config\SOFTWARE" | Out-Null; $loaded += $hive
                $k = "$hive\Microsoft\Windows NT\CurrentVersion\Winlogon"
                $old = (reg.exe query $k /v Shell 2>$null | Select-String 'REG_SZ\s+(.*)$').Matches.Groups[1].Value
                if ($desfazer) {
                    $val = if (Test-Path "$orig\shell-hklm.txt") { (Get-Content "$orig\shell-hklm.txt" -Raw).Trim() } else { 'explorer.exe' }
                    reg.exe add $k /v Shell /t REG_SZ /d $val /f | Out-Null; $r.Add("HKLM Shell -> $val")
                } else {
                    if (-not (Test-Path "$orig\shell-hklm.txt")) { Set-Content "$orig\shell-hklm.txt" $old -Encoding utf8 }
                    reg.exe add $k /v Shell /t REG_SZ /d $exe /f | Out-Null; $r.Add("HKLM Shell: $old -> $exe")
                }
                # cada usuário (e o perfil padrão)
                foreach ($u in Get-ChildItem "$win\Users" -Directory -Force) {
                    $nt = Join-Path $u.FullName 'NTUSER.DAT'
                    if (-not (Test-Path $nt) -or $u.Name -in 'All Users', 'Public', 'Default User') { continue }
                    $h = "HKU\VM_U_$($u.Name -replace '[^A-Za-z0-9]', '')"
                    reg.exe load $h $nt 2>&1 | Out-Null
                    if ($LASTEXITCODE -ne 0) { $r.Add("perfil $($u.Name): não carregou"); continue }
                    $loaded += $h
                    $uk = "$h\Software\Microsoft\Windows NT\CurrentVersion\Winlogon"
                    $bk = "$orig\shell-user-$($u.Name).txt"
                    if ($desfazer) {
                        if ((Test-Path $bk) -and (Get-Content $bk -Raw).Trim()) { reg.exe add $uk /v Shell /t REG_SZ /d (Get-Content $bk -Raw).Trim() /f | Out-Null }
                        else { reg.exe delete $uk /v Shell /f 2>&1 | Out-Null }
                        $r.Add("perfil $($u.Name): Shell devolvido")
                    } else {
                        $uold = ''
                        $mm = reg.exe query $uk /v Shell 2>$null | Select-String 'REG_SZ\s+(.*)$'
                        if ($mm) { $uold = $mm.Matches[0].Groups[1].Value }
                        if (-not (Test-Path $bk)) { Set-Content $bk "$uold" -Encoding utf8 }
                        reg.exe add $uk /v Shell /t REG_SZ /d $exe /f | Out-Null
                        $cfg = Join-Path $u.FullName 'AppData\Roaming\Shell'
                        New-Item -ItemType Directory -Force $cfg | Out-Null
                        $aj = Join-Path $cfg 'ajustes.txt'
                        if (-not (Test-Path $aj)) { Set-Content $aj "explorer=0`r`nfinder_padrao=1" -Encoding utf8 }
                        $r.Add("perfil $($u.Name): Shell -> SequoiaShell, ajustes $aj")
                    }
                }
                # AuthUX novo (a entrada espera o Dock e a barra de menus do Shell)
                if (-not $desfazer) {
                    Copy-Item (Join-Path (Split-Path $root -Parent) 'AuthUX\x64\Release.GE\AuthUX.dll') "$win\Windows\System32\AuthUX.dll" -Force
                }
            } finally {
                [GC]::Collect(); [GC]::WaitForPendingFinalizers()
                foreach ($h in $loaded) { reg.exe unload $h 2>&1 | Out-Null }
                Dismount-VHD -Path $vhd
            }
            Start-VM -Name $Nome
            $r.Add('VM ligada')
            if ($desfazer) { Nota "SequoiaShell deixou de ser o shell da VM (Winlogon\Shell devolvido; tools\vm-windows-mac.ps1 -Acao shell-desfazer)." }
            else { Nota "SequoiaShell instalado como shell do Windows da VM (C:\SequoiaShell; Winlogon\Shell HKLM e de cada usuário; ajustes explorer=0, finder_padrao=1). Valores antigos em C:\SequoiaMac\originais\shell-*.txt. Desfazer: tools\vm-windows-mac.ps1 -Acao shell-desfazer (VM desligada) ou Ctrl+Alt+Shift+F11 dentro da VM devolve o Explorador na hora." }
        }
        { $_ -in 'barra-instalar', 'barra-desfazer', 'barra-log' } {
            # Barra de tarefas reescrita (barra-nativa\MacBarra.dll, Dock do Mac), com a VM desligada:
            #  - barra-instalar: copia o Taskbar.dll e o explorer.exe da VM para docs\vm-barra, confere se as interfaces
            #    ITrayUI/ITrayUIHost estão na ordem que a MacBarra espera (barra-nativa\conferir-interfaces.ps1) e só então
            #    põe a MacBarra.dll em System32 e troca HKLM\SOFTWARE\Classes\CLSID\{88FC85D3...}\InProcServer32
            #    ("TrayUI Component", que o explorer.exe cria para montar a barra) de Taskbar.dll para MacBarra.dll
            #  - barra-desfazer: devolve o valor original (guardado em C:\SequoiaMac\originais\barra-clsid.txt)
            #  - barra-log: só copia C:\Users\Public\MacBarra.log da VM para docs\vm-barra
            if ((Get-VM $Nome).State -ne 'Off') { Stop-VM -Name $Nome -Force; $r.Add('VM desligada') }
            $vhd = (Get-VMHardDiskDrive -VMName $Nome | Select-Object -First 1).Path
            $disk = Mount-VHD -Path $vhd -PassThru | Get-Disk
            $hive = $null
            try {
                $win = $null
                foreach ($p in ($disk | Get-Partition)) {
                    if (-not $p.DriveLetter) { $p | Add-PartitionAccessPath -AssignDriveLetter -ErrorAction SilentlyContinue; $p = Get-Partition -DiskNumber $p.DiskNumber -PartitionNumber $p.PartitionNumber }
                    if ($p.DriveLetter -and (Test-Path "$($p.DriveLetter):\Windows\System32")) { $win = "$($p.DriveLetter):" }
                }
                if (-not $win) { throw 'Windows da VM não encontrado' }
                $orig = "$win\SequoiaMac\originais"
                New-Item -ItemType Directory -Force $orig | Out-Null
                $bn = Join-Path $root 'barra-nativa'
                $pasta = Join-Path $root 'docs\vm-barra'
                New-Item -ItemType Directory -Force $pasta | Out-Null
                foreach ($f in 'MacBarra.log', 'MacBarra-falhas.txt') {
                    $src = "$win\Users\Public\$f"
                    if (Test-Path $src) { Copy-Item $src (Join-Path $pasta $f) -Force }
                }
                if (Test-Path "$win\Users\Public\MacBarra.log") { $r.Add('--- fim do MacBarra.log da VM:'); $r.AddRange([string[]](Get-Content "$win\Users\Public\MacBarra.log" -Tail 40 -Encoding UTF8)) }
                if ($Acao -ne 'barra-log') {
                    # o reg load (RegLoadKey) deste PC está falhando para qualquer hive ("nome longo demais"); RegLoadAppKey
                    # carrega a hive SOFTWARE da VM de forma privada (fica aberta só enquanto houver handle dela)
                    Add-Type -ErrorAction SilentlyContinue @'
using System; using System.Runtime.InteropServices;
public static class RegApp {
  [DllImport("advapi32.dll", CharSet=CharSet.Unicode)] static extern int RegLoadAppKeyW(string file, out IntPtr key, int sam, int opt, int res);
  [DllImport("advapi32.dll", CharSet=CharSet.Unicode)] static extern int RegCreateKeyExW(IntPtr hKey, string sub, int res, string cls, int opt, int sam, IntPtr sa, out IntPtr result, out int disp);
  [DllImport("advapi32.dll", CharSet=CharSet.Unicode)] static extern int RegOpenKeyExW(IntPtr hKey, string sub, int opt, int sam, out IntPtr result);
  [DllImport("advapi32.dll", CharSet=CharSet.Unicode)] static extern int RegSetValueExW(IntPtr h, string name, int res, int type, byte[] data, int cb);
  [DllImport("advapi32.dll", CharSet=CharSet.Unicode)] static extern int RegQueryValueExW(IntPtr h, string name, IntPtr res, out int type, byte[] data, ref int cb);
  [DllImport("advapi32.dll")] static extern int RegCloseKey(IntPtr h);
  [DllImport("advapi32.dll", SetLastError=true)] static extern bool OpenProcessToken(IntPtr p, int acc, out IntPtr tok);
  [DllImport("advapi32.dll", SetLastError=true, CharSet=CharSet.Unicode)] static extern bool LookupPrivilegeValueW(string sys, string name, out long luid);
  [StructLayout(LayoutKind.Sequential, Pack=4)] struct TP { public int Count; public long Luid; public int Attr; }
  [DllImport("advapi32.dll", SetLastError=true)] static extern bool AdjustTokenPrivileges(IntPtr tok, bool dis, ref TP n, int len, IntPtr p, IntPtr r);
  [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
  public static IntPtr Raiz = IntPtr.Zero;
  public static void Enable(string n) { IntPtr t; OpenProcessToken(GetCurrentProcess(), 0x28, out t); TP tp = new TP(); tp.Count = 1; LookupPrivilegeValueW(null, n, out tp.Luid); tp.Attr = 2; AdjustTokenPrivileges(t, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero); }
  public static int Carregar(string arq) { return RegLoadAppKeyW(arq, out Raiz, 0xF003F, 0, 0); }
  public static void Fechar() { if (Raiz != IntPtr.Zero) { RegCloseKey(Raiz); Raiz = IntPtr.Zero; } }
  public static string Get(string sub, string name) {
    IntPtr h;
    if (RegOpenKeyExW(Raiz, sub, 0, 0x20019, out h) != 0) return null;
    byte[] b = new byte[4096]; int cb = b.Length; int type;
    int r = RegQueryValueExW(h, name, IntPtr.Zero, out type, b, ref cb); RegCloseKey(h);
    if (r != 0) return null; return System.Text.Encoding.Unicode.GetString(b, 0, cb).TrimEnd('\0');
  }
  // abre a chave-pai com backup/restore (passa por cima da ACL do TrustedInstaller) e cria/abre a filha por ela
  public static int SetSub(string pai, string filha, string name, string val, int type) {
    IntPtr hp, h; int d;
    int r = RegCreateKeyExW(Raiz, pai, 0, null, 4, 0, IntPtr.Zero, out hp, out d);
    if (r != 0) { r = RegOpenKeyExW(Raiz, pai, 0, 0xF003F, out hp); if (r != 0) return 1000 + r; }
    r = RegCreateKeyExW(hp, filha, 0, null, 0, 0xF003F, IntPtr.Zero, out h, out d);
    if (r != 0) r = RegCreateKeyExW(hp, filha, 0, null, 4, 0, IntPtr.Zero, out h, out d);
    RegCloseKey(hp); if (r != 0) return 2000 + r;
    byte[] b = System.Text.Encoding.Unicode.GetBytes(val + "\0");
    r = RegSetValueExW(h, name, 0, type, b, b.Length); RegCloseKey(h); return r == 0 ? 0 : 3000 + r;
  }
}
'@
                    [RegApp]::Enable('SeRestorePrivilege'); [RegApp]::Enable('SeBackupPrivilege')
                    $c0 = [RegApp]::Carregar("$win\Windows\System32\config\SOFTWARE")
                    $r.Add("registro SOFTWARE da VM aberto (RegLoadAppKey): código $c0")
                    if ($c0 -ne 0) { throw "não consegui abrir o registro SOFTWARE da VM (código $c0)" }
                    $hive = 'app'
                    $ck = 'Classes\CLSID\{88FC85D3-7090-4F53-8F7A-EB0268162788}\InProcServer32'
                    $atual = [RegApp]::Get($ck, '')
                    $r.Add("TrayUI Component agora: '$atual' (ordem: '$([RegApp]::Get($ck, 'MacBarraOrdem'))')")
                    function GravarClsid([string]$nome, [string]$valor) {
                        $c = [RegApp]::SetSub('Classes\CLSID', '{88FC85D3-7090-4F53-8F7A-EB0268162788}\InProcServer32', $nome, $valor, 1)
                        if ($c -eq 0) { return 'ok' }
                        return "FALHOU (código $c)"
                    }
                    if ($Acao -eq 'barra-desfazer') {
                        $volta = if (Test-Path "$orig\barra-clsid.txt") { (Get-Content "$orig\barra-clsid.txt" -Raw).Trim() } else { '' }
                        if (-not $volta) { $volta = 'C:\Windows\System32\Taskbar.dll' }
                        $r.Add("TrayUI Component -> $volta : " + (GravarClsid '' $volta))
                        Nota "barra de tarefas original devolvida (TrayUI Component -> $volta; tools\vm-windows-mac.ps1 -Acao barra-desfazer)."
                    } else {
                        Copy-Item "$win\Windows\System32\Taskbar.dll", "$win\Windows\explorer.exe" $pasta -Force
                        $conf = & (Join-Path $bn 'conferir-interfaces.ps1') -Pasta $pasta 2>&1
                        $confOk = $LASTEXITCODE -eq 0
                        $r.AddRange([string[]]($conf | ForEach-Object { "$_" }))
                        if (-not $confOk) {
                            $r.Add('MacBarra NÃO ativada: as interfaces do Windows da VM são diferentes das que a barra espera')
                        } else {
                            Copy-Item (Join-Path $bn 'bin\MacBarra.dll') "$win\Windows\System32\MacBarra.dll" -Force
                            $bk = "$orig\barra-clsid.txt"
                            if ($atual -and $atual -notmatch 'MacBarra' -and (-not (Test-Path $bk) -or -not (Get-Content $bk -Raw).Trim())) { Set-Content $bk $atual -Encoding utf8 }
                            $r.Add("MacBarra.dll copiada ($((Get-Item (Join-Path $bn 'bin\MacBarra.dll')).Length) bytes)")
                            $r.Add('TrayUI Component -> C:\Windows\System32\MacBarra.dll : ' + (GravarClsid '' 'C:\Windows\System32\MacBarra.dll'))
                            $r.Add('ThreadingModel Apartment : ' + (GravarClsid 'ThreadingModel' 'Apartment'))
                            $ordem = ($conf | ForEach-Object { "$_" } | Where-Object { $_ -match '^ORDEM=(\d+)' } | Select-Object -First 1) -replace '^ORDEM=', ''
                            $r.Add("MacBarraOrdem = $ordem : " + (GravarClsid 'MacBarraOrdem' $ordem))
                            $r.Add("depois: '$([RegApp]::Get($ck, ''))' ThreadingModel '$([RegApp]::Get($ck, 'ThreadingModel'))' ordem '$([RegApp]::Get($ck, 'MacBarraOrdem'))'")
                            Remove-Item "$win\Users\Public\MacBarra-falhas.txt", "$win\Users\Public\MacBarra-desligada.txt" -ErrorAction SilentlyContinue
                            Nota "barra de tarefas reescrita (Dock do Mac) instalada: MacBarra.dll em System32 e HKLM\SOFTWARE\Classes\CLSID\{88FC85D3-7090-4F53-8F7A-EB0268162788}\InProcServer32 (TrayUI Component, antes $atual) -> MacBarra.dll. Se o explorer cair 3 vezes a DLL volta sozinha para a barra original; C:\Users\Public\MacBarra-desligada.txt também desliga. Desfazer: tools\vm-windows-mac.ps1 -Acao barra-desfazer."
                        }
                    }
                }
            } finally {
                if ($hive) { [RegApp]::Fechar(); [GC]::Collect(); [GC]::WaitForPendingFinalizers(); Start-Sleep -Milliseconds 500 }
                Dismount-VHD -Path $vhd
                Start-VM -Name $Nome
                $r.Add('VM ligada')
            }
        }
        'authux-dll' {
            # só troca a AuthUX.dll (Documents\AuthUX\x64\Release.GE) com a VM desligada; não mexe no registro
            if ((Get-VM $Nome).State -ne 'Off') { Stop-VM -Name $Nome -Force; $r.Add('VM desligada') }
            $vhd = (Get-VMHardDiskDrive -VMName $Nome | Select-Object -First 1).Path
            $disk = Mount-VHD -Path $vhd -PassThru | Get-Disk
            try {
                $win = $null
                foreach ($p in ($disk | Get-Partition)) {
                    if (-not $p.DriveLetter) { $p | Add-PartitionAccessPath -AssignDriveLetter -ErrorAction SilentlyContinue; $p = Get-Partition -DiskNumber $p.DiskNumber -PartitionNumber $p.PartitionNumber }
                    if ($p.DriveLetter -and (Test-Path "$($p.DriveLetter):\Windows\System32")) { $win = "$($p.DriveLetter):" }
                }
                if (-not $win) { throw 'Windows da VM não encontrado' }
                $dll = Join-Path (Split-Path $root -Parent) 'AuthUX\x64\Release.GE\AuthUX.dll'
                Copy-Item $dll "$win\Windows\System32\AuthUX.dll" -Force
                $r.Add("AuthUX.dll trocada ($((Get-Item $dll).Length) bytes)")
                $logVm = "$win\Users\Public\AuthUX-mac.log"
                if (Test-Path $logVm) { Copy-Item $logVm (Join-Path $root 'docs\authux-mac-vm.log') -Force; $r.AddRange([string[]](Get-Content $logVm -Tail 25 -Encoding UTF8)) }
            } finally {
                Dismount-VHD -Path $vhd
                Start-VM -Name $Nome
                $r.Add('VM ligada')
            }
            Nota "AuthUX.dll trocada com a VM desligada (Ctrl+Alt+Del com fade de entrada e Esc = Cancelar)."
        }
        'registro-diag' {
            # só leitura: por que o reg load do registro da VM falha (arquivos de config e tentativa de carregar cada hive)
            if ((Get-VM $Nome).State -ne 'Off') { Stop-VM -Name $Nome -Force; $r.Add('VM desligada') }
            $vhd = (Get-VMHardDiskDrive -VMName $Nome | Select-Object -First 1).Path
            $r.Add("disco: $vhd")
            $disk = Mount-VHD -Path $vhd -PassThru | Get-Disk
            try {
                foreach ($p in ($disk | Get-Partition)) {
                    if (-not $p.DriveLetter) { $p | Add-PartitionAccessPath -AssignDriveLetter -ErrorAction SilentlyContinue; $p = Get-Partition -DiskNumber $p.DiskNumber -PartitionNumber $p.PartitionNumber }
                    $r.Add("partição $($p.PartitionNumber) letra '$($p.DriveLetter)' $([int]($p.Size/1GB)) GB tipo $($p.Type)")
                    if ($p.DriveLetter -and (Test-Path "$($p.DriveLetter):\Windows\System32\config")) {
                        $cfg = "$($p.DriveLetter):\Windows\System32\config"
                        Get-ChildItem $cfg -Force -File | Where-Object { $_.Name -match '^(SOFTWARE|SYSTEM)' } | ForEach-Object { $r.Add("  $($_.Name) $($_.Length) $($_.Attributes) $($_.LastWriteTime)") }
                        foreach ($h in 'SOFTWARE', 'SYSTEM') {
                            $o = reg.exe load "HKLM\VMDIAG_$h" "$cfg\$h" 2>&1
                            $r.Add("  reg load $h -> $LASTEXITCODE $o")
                            if ($LASTEXITCODE -eq 0) {
                                $q = reg.exe query "HKLM\VMDIAG_$h\Classes\CLSID\{88FC85D3-7090-4F53-8F7A-EB0268162788}\InProcServer32" 2>&1
                                if ($h -eq 'SOFTWARE') { $r.AddRange([string[]]@($q | ForEach-Object { "    $_" })) }
                                [GC]::Collect(); reg.exe unload "HKLM\VMDIAG_$h" 2>&1 | Out-Null
                            }
                        }
                    }
                }
            } finally {
                Dismount-VHD -Path $vhd
                Start-VM -Name $Nome
                $r.Add('VM ligada')
            }
        }
        'gpu-desligar' {
            if ((Get-VM $Nome).State -ne 'Off') { Stop-VM -Name $Nome -Force }
            Remove-VMGpuPartitionAdapter -VMName $Nome -ErrorAction SilentlyContinue
            Start-VM -Name $Nome
            $r.Add('placa removida da VM e VM ligada')
            Nota "GPU-P removido da VM (Remove-VMGpuPartitionAdapter)."
        }
        default { $r.Add("ação desconhecida: $Acao") }
    }
    ("ok`r`n" + ($r -join "`r`n")) | Set-Content $out -Encoding utf8
} catch {
    ("erro: $($_.Exception.Message)`r`n" + ($r -join "`r`n")) | Set-Content $out -Encoding utf8
    exit 1
}
