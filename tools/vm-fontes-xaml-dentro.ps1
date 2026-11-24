# Roda DENTRO da VM AuthUX-Teste, como administrador. Devolve a fonte Segoe UI Variable, que o tema nativo
# (vm-windows-mac-dentro.ps1) tinha zerado em HKLM\...\Fonts e mandado para a SF Pro em FontSubstitutes. A interface
# moderna do Windows (XAML: Terminal, busca, Configurações, ShellExperienceHost) depende dela e caía com 0xc000027b.
# Só a Segoe UI Variable volta; a Segoe UI comum continua trocada pela SF Pro. Os valores voltam ao padrão do
# Windows 11 (SegUIVar.ttf; sem substituições). Resultado em C:\Users\Public\fontes-xaml.txt
#   aplicar | desfazer (zera e substitui de novo, como o tema deixou)
param([ValidateSet('aplicar', 'desfazer')][string]$Acao = 'aplicar')
$res = 'C:\Users\Public\fontes-xaml.txt'
$r = @("$(Get-Date -Format s) $Acao")
$fk = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts'
$sk = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\FontSubstitutes'
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class Fnt {
  [DllImport("gdi32.dll", CharSet=CharSet.Unicode)] public static extern int AddFontResourceW(string f);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeoutW(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint t, out IntPtr r);
  public static void Avisar() { IntPtr r; SendMessageTimeoutW((IntPtr)0xffff, 0x001D, IntPtr.Zero, IntPtr.Zero, 2, 2000, out r); }
}
'@
try {
    if ($Acao -eq 'aplicar') {
        # 1) Fonts: os valores "Segoe UI Variable*" voltam ao nome do arquivo
        $props = (Get-Item $fk).Property | Where-Object { $_ -like 'Segoe UI Variable*' }
        if (-not $props) { $props = @('Segoe UI Variable (TrueType)') }
        foreach ($n in $props) {
            $atual = (Get-ItemProperty $fk).$n
            $old = 'SegUIVar.ttf'   # o arquivo da Segoe UI Variable no Windows 11 (o valor padrão desta chave)
            if (-not (Test-Path "C:\Windows\Fonts\$old")) { $r += "Fonts '$n': arquivo $old não existe em C:\Windows\Fonts; não mexi"; continue }
            Set-ItemProperty $fk -Name $n -Value $old
            $c = [Fnt]::AddFontResourceW("C:\Windows\Fonts\$old")
            $r += "Fonts '$n': '$atual' -> '$old' (carregada agora: $c)"
        }
        # 2) FontSubstitutes: as trocas da Segoe UI Variable saem (ou voltam ao que eram)
        foreach ($n in ((Get-Item $sk).Property | Where-Object { $_ -like 'Segoe UI Variable*' })) {
            $atual = (Get-ItemProperty $sk).$n
            Remove-ItemProperty $sk -Name $n; $r += "FontSubstitutes '$n': '$atual' removida (o Windows não tem essa troca de fábrica)"
        }
    } else {
        foreach ($n in ((Get-Item $fk).Property | Where-Object { $_ -like 'Segoe UI Variable*' })) { Set-ItemProperty $fk -Name $n -Value ''; $r += "Fonts '$n' zerada" }
        foreach ($p in @(@('Segoe UI Variable', 'SF Pro Text'), @('Segoe UI Variable Text', 'SF Pro Text'), @('Segoe UI Variable Small', 'SF Pro Text'), @('Segoe UI Variable Display', 'SF Pro Display'))) {
            Set-ItemProperty $sk -Name $p[0] -Value $p[1]; $r += "FontSubstitutes '$($p[0])' -> '$($p[1])'"
        }
    }
    [Fnt]::Avisar()
    $r += 'aviso de mudança de fontes enviado (WM_FONTCHANGE). Programas já abertos só veem depois de reabrir; se as quedas continuarem, é preciso reiniciar a VM.'
} catch { $r += "erro: $($_.Exception.Message)" }
$r | Set-Content $res -Encoding utf8
