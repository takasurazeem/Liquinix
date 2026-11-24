# Shell sem o explorer.exe

O SequoiaShell desenha a mesa (papel de parede + ícones) sozinho (`src/shell/mesa.cpp`), então o
explorer.exe pode ser fechado. O modo é escolhido em `%APPDATA%\Shell\ajustes.txt`:

| ajuste | o que acontece |
|---|---|
| `explorer=1` (padrão) | como antes: o Explorador é a mesa. Se ele cair ou for fechado, a mesa do Shell aparece em 3 s (sem se registrar como shell) e some quando ele volta. |
| `explorer=0` | o Shell pede ao Explorador para sair (mensagem `WM_USER+436` ao `Shell_TrayWnd` dele, o mesmo "Sair do Explorador" do Ctrl+Shift+clique direito na barra de tarefas; se não sair em 8 s, `TerminateProcess` código 0) e assume. Logo depois do login espera o Explorador ter 30 s de vida, para ele abrir os apps de inicialização. Se o Explorador voltar sozinho 3 vezes, o Shell desiste e fica com ele. |

Trocar o ajuste com o Shell aberto vale na hora (o Ajustes avisa com `ShellAjustesMudaram`):
`explorer=1` de novo → a nossa mesa fecha e o explorer.exe é aberto.

**Nunca fica sem mesa:** ao sair do Shell (menu da Apple, Ctrl+Alt+Shift+F12) a mesa dele fecha e o
explorer.exe é aberto de novo. Se o Shell cair, um vigia (`SequoiaShell.exe --mesa-vigia <pid>`, outro
processo) espera 1,5 s e abre o explorer.exe se não houver janela de shell. No fim da sessão do Windows
nada é aberto.

## Inventário: o que o Explorador fazia por nós

| dependência | onde | substituto |
|---|---|---|
| Papel de parede e ícones da mesa (Progman/WorkerW/SHELLDLL_DefView) | — | `mesa.cpp`: janela `SequoiaMesa` por monitor, sempre no fundo (`WM_WINDOWPOSCHANGING` → `HWND_BOTTOM`). Papel de parede do registro (`HKCU\Control Panel\Desktop` WallPaper/WallpaperStyle/TileWallpaper; se o arquivo sumiu, `Themes\TranscodedWallpaper`), com centralizar/lado a lado/esticar/ajustar/preencher (estender = preencher em cada monitor). Recarrega com `WM_SETTINGCHANGE SPI_SETDESKWALLPAPER`. |
| Ícones da Área de Trabalho | — | Pasta do usuário + pública, só arquivos de verdade (sem ocultos/sistema). Grade do Mac a partir do canto superior direito, sem ficar embaixo dos widgets nem da barra/Dock. Posições em `%APPDATA%\Shell\mesa.txt`. Ícones/miniaturas por `IShellItemImageFactory` numa thread. |
| Mexer nos ícones | — | clique, Ctrl+clique, retângulo de seleção, duplo clique/Enter abre (pastas e atalhos de pasta no Finder), arrastar com `SHDoDragDrop` (para o Finder, para uma pasta da mesa, ou para outra casa da grade), soltar de fora (alvo da pasta Área de Trabalho via `IDropTarget` da própria pasta), F2 renomeia (`IFileOperation::RenameItem`), Delete → Lixeira, Shift+Delete apaga (com confirmação), Ctrl+A/C/X/V, Ctrl+I, Ctrl+Shift+N, F5. Atualiza sozinha (`FindFirstChangeNotification` nas duas pastas + `SHChangeNotifyRegister`). |
| Menu de contexto | — | Itens: o nosso menu (`ctxmenu::Show`, extensões reais do Windows). Fundo: Nova Pasta, Obter Informações, Colar, Alterar Fundo da Mesa… (Ajustes → `fundo`), Limpar, Limpar Por (Nome/Tipo/Data/Tamanho), Mais Opções do Windows…. "Usar Pilhas" ainda não. |
| Widgets logo acima da mesa | `widgets.cpp` `DesktopTop` | `mesa::IsDeskClass` reconhece `SequoiaMesa`; `widgets::Areas()` diz à mesa onde eles estão. |
| Clique no vazio mostra a mesa | `revelar.cpp` (UIA nos ícones) | na nossa mesa pergunta `mesa::IconAt`; sem Explorador não cria `IVirtualDesktopManager`. |
| Gênio, tela cheia, área de trabalho, sombras, Nota Rápida, barra de menus ("Finder" ativo na mesa), Dock | `genio`, `main`, `sombras`, `notarapida`, `menubar`, `apps`, `missioncontrol` | todas as checagens de `Progman`/`WorkerW` passaram para `mesa::IsDeskClass`. |
| Mesas virtuais | `mesas.cpp` (`IVirtualDesktopManagerInternal`) | a interface é do Explorador: sem ele não há mesas virtuais. `mesas::*` devolvem false; Mission Control mostra só a mesa atual. |
| Janela de shell / Taskman / mensagens `HSHELL_*` | `main.cpp` `RegisterShellHookWindow` | sem Explorador: `SetShellWindow(mesa)` e `SetTaskmanWindow(ShellMensagens)` (user32). Ctrl+Esc (`SC_TASKLIST`) abre o Launchpad. O timer de 1 s do Shell continua relendo as janelas mesmo se algum `HSHELL_*` não vier. |
| Minimizadas viram barrinhas no canto | — | `SPI_SETMINIMIZEDMETRICS` com `ARW_HIDE`, só na sessão (sem gravar no registro); volta ao sair. |
| Bandeja (área de notificação) | `tray.cpp` | já era nossa (`Shell_TrayWnd` próprio). Sem Explorador não há para onde repassar; ao assumir, `TaskbarCreated` é enviado para os apps reenviarem os ícones. `SHAppBarMessage` (appbars de terceiros) fica sem resposta. |
| Barra de tarefas, Iniciar | — | já substituídos (Dock, Launchpad). |
| Win+E, Win+R, Win+D | atalhos do Explorador | gancho do teclado (`main.cpp`): Finder, Executar (shell32 #61), minimizar/devolver as janelas. Outros Win+… do Explorador (Win+X, Win+número, Win+Ctrl+setas) não fazem nada. |
| Abrir pasta / "Mostrar no Explorador" | `spotlight.cpp` `explorer /select`, pastas especiais no Finder | abre um explorer.exe como **janela de pasta** (com a janela de shell nossa ele não vira mesa). Fica assim por enquanto. |
| Imagem de fundo no Ajustes | `src/ajustes/sistema.cpp`, `paginas.cpp` (`IDesktopWallpaper`, do Explorador) | sem ele: `SystemParametersInfo(SPI_SETDESKWALLPAPER, SPIF_UPDATEINIFILE \| SPIF_SENDCHANGE)` e `WallpaperStyle`/`TileWallpaper` no registro. |
| Menus de contexto, cópia, lixeira, Propriedades | — | rodam dentro do nosso processo (`IContextMenu`, `IFileOperation`), não precisam do Explorador. |
| Apps de inicialização (Run, pasta Inicializar) | Explorador no login | no modo `explorer=0` o Explorador ainda abre no login e roda isso (o Shell espera 30 s). Se um dia o Shell for o shell do Windows (abaixo), isso terá de ser feito pelo Shell. |
| Notificações (banners do Windows), Reprodução Automática, mesas de toque | Explorador / ShellExperienceHost | **a testar** sem o Explorador. |

## Mudanças no registro

- `HKCU\Control Panel\Desktop` `WallPaper`, `WallpaperStyle`, `TileWallpaper`: só quando o fundo é trocado
  pelo Ajustes **sem** o Explorador (são os mesmos valores que o Windows grava ao trocar o fundo).
- Nada mais: `SetShellWindow`, `SetTaskmanWindow` e `ARW_HIDE` são só da sessão.

## Desfazer

- Voltar ao normal: `explorer=1` em `%APPDATA%\Shell\ajustes.txt` (ou apagar a linha). O Explorador volta na hora.
- Explorador sumiu e o Shell não está rodando: Ctrl+Shift+Esc → Executar nova tarefa → `explorer.exe`.

## O Shell como shell do Windows (feito em 08/11/2026, a pedido do usuário)

Registro alterado (antes não existia em HKCU; o HKLM continua `explorer.exe`, intocado):

```
HKCU\Software\Microsoft\Windows NT\CurrentVersion\Winlogon  Shell = %USERPROFILE%\Documents\shell-proprio\bin\SequoiaShell.exe
```

Desfazer (o Explorador volta a ser o shell no próximo login):

```
reg delete "HKCU\Software\Microsoft\Windows NT\CurrentVersion\Winlogon" /v Shell /f
```

Se o Shell não abrir no login (tela preta): Ctrl+Shift+Esc → Executar nova tarefa → rodar o `reg delete` acima
(ou `cmd`), depois sair e entrar de novo.

O que muda com o registro:
- No login o Windows abre o SequoiaShell no lugar do explorer.exe. A tarefa agendada `SequoiaShell` continua
  (a segunda cópia sai sozinha: mutex `ShellProprioUmaInstancia`).
- Com o registro, o Shell fica sempre sem o Explorador (o ajuste `explorer=1` deixa de valer).
- Apps de inicialização: o Shell roda `Run` (HKCU, HKLM 64 e 32 bits) e as pastas Inicializar (usuário e todos),
  pulando os desligados em `StartupApproved` (Gerenciador de Tarefas → Inicialização). Uma vez por sessão
  (átomo global `SequoiaShellInicializacao`) e só se o Explorador não estava aberto quando o Shell abriu.
  `RunOnce` **não** é rodado.
- Sair do Shell: como o explorer.exe só vira mesa quando é o shell configurado, o Shell troca o valor
  para `explorer.exe` por ~2 s, abre o Explorador e devolve o valor (testado: "Explorador é a mesa"). Se o
  Shell cair depois de 30 s rodando, o vigia abre o Shell de novo; se cair antes, abre o Explorador.

Ajuste `explorer` com o registro: `explorer=1` → o Explorador continua sendo a mesa (no login o Shell abre o
explorer.exe e não roda os apps de inicialização); `explorer=0` ou sem a linha → sem Explorador.
Hoje (08/11) o arquivo está com `explorer=1`.

Ainda não testado: um login de verdade com o registro (só a troca ao vivo foi testada).

## Travamento de 08/11 (12:18–12:22) e redes de segurança

O PC "travou" (apps rodando, nada clicável) com o Explorador aberto: o `explorer=0` ainda não tinha chegado
ao Shell. Causa: o relógio de quadros (`ui::FrameClock`) desligava relógios de janelas "escondidas"; a do
"clique na mesa mostra a mesa" (`revelar.cpp`) é só de mensagens, então a animação parou no meio e a camada
dela (`ShellRevelarCamada`, por cima de tudo, do tamanho do monitor) ficou na tela engolindo os cliques
(log: "relógio de quadros: ShellRevelar ... estava ligado com a janela escondida; desligado"). O corte foi
tirado do `ui.cpp` às 12:24.

Redes de segurança (sempre ligadas, com ou sem Explorador):
- `revelar.cpp`: animação parada há 2,5 s termina à força e a camada some.
- Vigia (`SequoiaShell.exe --mesa-vigia <pid>`, outro processo): **Ctrl+Alt+Shift+F11** devolve o Explorador
  (o Shell sai direito; se não responder, é encerrado e o Explorador é aberto). Se o Shell não responder por
  5 s enquanto alguém usa o mouse/teclado, o mesmo acontece sozinho. O vigia escreve no `shell.log`.

Nota para outras sessões: as sessões do Claude rodam isoladas e o que gravam em `%APPDATA%` não chega ao
arquivo de verdade (o `explorer=0`/`explorer=1` gravados por elas não valeram). Para mudar `ajustes.txt` ou o
registro, use o terminal do app (fora do isolamento) ou o Ajustes do Sistema.
