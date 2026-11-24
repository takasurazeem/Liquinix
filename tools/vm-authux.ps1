# Instala/desfaz o AuthUX DENTRO da VM AuthUX-Teste via PowerShell Direct (não toca no PC real).
# Uso: tools\vm-authux.ps1 -Acao checkpoint|instalar|desfazer|estado   (pede administrador)
# Credencial de teste da VM: docs\vm-authux.txt
param([switch]$Admin, [string]$Acao = 'estado', [string]$Nome = 'AuthUX-Teste')
$root = Split-Path $PSScriptRoot -Parent
$out  = Join-Path $root 'docs\ultima-execucao-vm.txt'
if (-not $Admin) {
    Remove-Item $out -ErrorAction SilentlyContinue
    $p = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -WindowStyle Hidden -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Admin -Acao $Acao -Nome `"$Nome`""
    exit $p.ExitCode
}
$r = @()
try {
    if ($Acao -eq 'rede') {
        Connect-VMNetworkAdapter -VMName $Nome -SwitchName 'Default Switch'
        $r += 'rede da VM religada (Default Switch)'
    }
    if ($Acao -eq 'contas') {
        $sec  = ConvertTo-SecureString 'teste123' -AsPlainText -Force
        $cred = New-Object System.Management.Automation.PSCredential('.\Teste', $sec)
        $r += Invoke-Command -VMName $Nome -Credential $cred -ScriptBlock {
            Get-LocalUser | Where-Object Enabled | ForEach-Object { "$($_.Name) | origem: $($_.PrincipalSource) | admin: $([bool](Get-LocalGroupMember Administrators -EA SilentlyContinue | Where-Object Name -like "*\$($_.Name)"))" }
        }
    } elseif ($Acao -eq 'rede') {
    } elseif ($Acao -eq 'checkpoint') {
        Checkpoint-VM -Name $Nome -SnapshotName 'antes do AuthUX'
        $r += 'ponto de verificação "antes do AuthUX" criado'
    } else {
        $sec  = ConvertTo-SecureString 'teste123' -AsPlainText -Force
        $cred = New-Object System.Management.Automation.PSCredential('.\Teste', $sec)
        $r += Invoke-Command -VMName $Nome -Credential $cred -ArgumentList $Acao -ScriptBlock {
            param($Acao)
            $base = 'SOFTWARE\Microsoft\WindowsRuntime\ActivatableClassId\Windows.Internal.UI.Logon.Controller.'
            $padrao = @{ LogonUX = '%SystemRoot%\system32\Windows.UI.Logon.dll'; LockScreenHost = '%SystemRoot%\system32\logoncontroller.dll' }
            $o = @()
            $v = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
            $o += "VM build $($v.CurrentBuild).$($v.UBR)"
            if ($Acao -ne 'estado') {
                Add-Type @'
using System; using System.Runtime.InteropServices;
public static class RegBR {
  [DllImport("advapi32.dll", CharSet=CharSet.Unicode)] static extern int RegCreateKeyExW(IntPtr hKey, string sub, int res, string cls, int opt, int sam, IntPtr sa, out IntPtr result, out int disp);
  [DllImport("advapi32.dll", CharSet=CharSet.Unicode)] static extern int RegSetValueExW(IntPtr h, string name, int res, int type, byte[] data, int cb);
  [DllImport("advapi32.dll")] static extern int RegCloseKey(IntPtr h);
  [DllImport("advapi32.dll", SetLastError=true)] static extern bool OpenProcessToken(IntPtr p, int acc, out IntPtr tok);
  [DllImport("advapi32.dll", SetLastError=true, CharSet=CharSet.Unicode)] static extern bool LookupPrivilegeValueW(string sys, string name, out long luid);
  [StructLayout(LayoutKind.Sequential, Pack=4)] struct TP { public int Count; public long Luid; public int Attr; }
  [DllImport("advapi32.dll", SetLastError=true)] static extern bool AdjustTokenPrivileges(IntPtr tok, bool dis, ref TP n, int len, IntPtr p, IntPtr r);
  [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
  public static void Enable(string n) { IntPtr t; OpenProcessToken(GetCurrentProcess(), 0x28, out t); TP tp = new TP(); tp.Count = 1; LookupPrivilegeValueW(null, n, out tp.Luid); tp.Attr = 2; AdjustTokenPrivileges(t, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero); }
  public static int Set(string sub, string val) {
    IntPtr h; int d; IntPtr hklm = new IntPtr(unchecked((int)0x80000002));
    int r = RegCreateKeyExW(hklm, sub, 0, null, 4, 0x20006, IntPtr.Zero, out h, out d); if (r != 0) return r;
    byte[] b = System.Text.Encoding.Unicode.GetBytes(val + "\0");
    r = RegSetValueExW(h, "DllPath", 0, 2, b, b.Length); RegCloseKey(h); return r;
  }
}
'@
                [RegBR]::Enable('SeRestorePrivilege'); [RegBR]::Enable('SeBackupPrivilege')
                if ($Acao -eq 'instalar') {
                    $dll = Get-Volume | Where-Object FileSystemLabel -eq 'PACOTES' | ForEach-Object { "$($_.DriveLetter):\authux\AuthUX.dll" } | Select-Object -First 1
                    Copy-Item $dll "$env:SystemRoot\System32\AuthUX.dll" -Force
                    $o += "AuthUX.dll copiado para System32 da VM"
                    foreach ($k in 'LogonUX', 'LockScreenHost') { $o += "$k -> " + [RegBR]::Set($base + $k, '%SystemRoot%\System32\AuthUX.dll') }
                } else {
                    foreach ($k in 'LogonUX', 'LockScreenHost') { $o += "$k restaurado -> " + [RegBR]::Set($base + $k, $padrao[$k]) }
                }
            }
            foreach ($k in 'LogonUX', 'LockScreenHost') {
                $key = Get-Item ("HKLM:\" + $base + $k)
                $o += "$k DllPath = " + $key.GetValue('DllPath', $null, 'DoNotExpandEnvironmentNames') + " ($($key.GetValueKind('DllPath')))"
            }
            $o
        }
    }
    ("ok`r`n" + ($r -join "`r`n")) | Set-Content $out -Encoding utf8
} catch {
    ("erro: $($_.Exception.Message)`r`n" + ($r -join "`r`n")) | Set-Content $out -Encoding utf8
    exit 1
}
