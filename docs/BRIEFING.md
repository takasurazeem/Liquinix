# Briefing completo do projeto (referência obrigatória para qualquer agente)

Atualizado em 2026-11-11. Leia inteiro antes de mudar qualquer coisa. Quando este documento e o código
divergirem, o código manda; corrija o documento. Documentos de apoio: `docs/dock-e-barra-de-menus.md`,
`docs/sem-explorer.md`, `docs/problemas-conhecidos.md`, `docs/pendencias.md`, `docs/estudo-bandeja.md`,
`docs/registro-mudancas.txt` (tudo que já mudou no sistema).

---

## 1. O que é

Um "macOS por cima do Windows 11": um shell próprio em **C++ Win32 + Direct2D/DirectWrite +
DirectComposition / Windows.UI.Composition**, sem frameworks, que substitui a experiência do Windows
(barra de tarefas, Iniciar, Explorador, menus, diálogos, tela de bloqueio) pelo visual e comportamento do
**macOS Sequoia** e, com o tema ligado, do **macOS 26 Tahoe (Liquid Glass)**. Kernel, drivers e apps do
Windows continuam os mesmos.

Objetivo de qualidade: **ficar idêntico ao Mac** (medidas, curvas, tempos, textos em pt-BR), fluido
(no ritmo da tela, que aqui é 174 Hz) e reversível.

---

## 2. Regras do usuário (inegociáveis)

1. **Sempre responder em português (pt-BR).** Textos da interface em pt-BR, com tradução em inglês
   (seção 7.4).
2. **Reversível sempre.** Toda mudança no sistema tem um "desfazer" e fica anotada em
   `docs/registro-mudancas.txt` (data, o que, valor antigo, como desfazer).
3. **Não editar arquivos do sistema** no PC real (System32, imageres.dll etc.). Só registro (de preferência
   HKCU) e arquivos próprios. Na **VM de testes** é permitido (é para isso que ela existe).
4. **Avisar antes de qualquer ação irreversível** (registro de shell, remover arquivos, políticas) e
   esperar o OK.
5. **Não registrar o Shell como shell do Windows** no PC real nesta etapa (HKLM/HKCU Winlogon Shell).
6. **Não fazer commit** a menos que o usuário peça. Nada de push.
7. **Pode reiniciar o Shell sem perguntar** (seção 4.3), mas **nunca** sem o executável novo existir.
8. **Não fechar apps do usuário sem perguntar.** Matar processo exige OK a cada vez.
   Fechar janelas que o próprio agente abriu para teste (WM_CLOSE) é permitido.
9. **Evitar simulação de entrada** (SendInput, keybd_event, mouse_event) no PC real. Mensagens diretas
   para janelas dos nossos próprios apps (ex.: WM_MOUSEWHEEL para medir) são aceitáveis. Na VM, o teclado
   e o mouse virtuais do Hyper-V podem ser usados (seção 10.3).
10. **Nunca repetir a chave da API do macosicons** (`%APPDATA%\Shell\macosicons.txt`).
11. **Nunca digitar senhas/PINs da conta Microsoft do usuário**; ele digita. Credenciais de teste da VM
    ficam em `docs/vm-authux.txt` (não repetir na conversa).
12. Secure Boot fica **desligado** de propósito (o driver AudioCore usa `testsigning`); TPM fica ligado.
    O Windows está **travado no 25H2 / build 26200** por política (`tools/travar-atualizacoes.ps1`).
13. Coisas que parecem estranhas mas são do usuário: `version.dll` sem assinatura no Ableton (mod próprio),
    agente Insightful/Workpuls (trabalho). Não tratar como malware.

---

## 3. Ambiente

- Windows 11 Pro 25H2, build 26200.9711. Monitor 3440×1440 a **174 Hz** (RTX 3070 Ti).
- Visual Studio Build Tools 2022 (MSVC v143), Windows SDK 10.0.26100.0, C++20.
- **O Claude roda num contêiner MSIX**: HKCU e `%APPDATA%` que ele vê podem ser uma **cópia virtualizada**.
  Para ler/gravar o registro e o AppData **reais**, ou para abrir programas "de verdade", lance pelo WMI:
  `Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = '...' }`.
  Programas abertos diretamente pelo Claude gravam ajustes numa cópia que o Shell não vê.
- `DllImport` no PowerShell com `CharSet = CharSet.Unicode`.
- Scripts .ps1 do Windows PowerShell 5.1 **com BOM UTF-8** (sem BOM os acentos quebram).
  `ajustes.txt` é UTF-8 com BOM.
- Explorer fica rodando "no fundo" (`explorer=fundo`): apps da Loja (UWP) precisam dele. Nunca ligar e
  desligar o Explorer em ciclo.

---

## 4. Compilar, instalar no lugar e reiniciar

### 4.1 Build
`build.cmd [alvo]` na raiz. Alvos: (vazio = tudo), `shell`, `finder`, `ajustes`, `monitor`, `terminal`,
`calculadora`, `notas`, `captura`, `menu` (bancada MenuTeste), `titulos` (WacOSTitleBars.exe; também sai no `janelas`), `janelas` (SequoiaWindow.dll 64/32 +
host 32), `dialogo` (DialogoMac.dll), `bandeja`. O build "tudo" **não** inclui `janelas` nem `dialogo`.
Ícone e descrição de cada exe: `tools/recursos.ps1` (gerado no build).
Flags: `/O2 /W3 /std:c++20 /utf-8 /DUNICODE`. Sem MSBuild, sem projeto .vcxproj.

### 4.2 Antes de compilar
1. Conferir se há `cl.exe`/`link.exe` rodando (outra sessão pode estar compilando; o PDB do shell
   conflita).
2. Executáveis em uso não podem ser sobrescritos: **mova** o exe em uso para
   `%LOCALAPPDATA%\SequoiaBinariosAntigos\<nome>-<data>.exe` antes do build (Windows deixa renomear arquivo
   em uso). Só mova o que está travado. Se mover algo que o alvo não recompila (ex.: o host de 32 bits no
   build "tudo"), devolva a cópia.

### 4.3 Reiniciar o Shell
`PostMessage(FindWindowW(L"ShellMensagens"), RegisterWindowMessageW(L"ShellReiniciar"))`.
**Só depois de conferir que `bin\SequoiaShell.exe` existe.** Se o Shell morreu e não há exe, devolva a
versão anterior de `SequoiaBinariosAntigos` e abra pelo WMI (seção 3).
O Finder mantém uma **reserva** escondida (janela `ArquivosJanela` com título `\u200BArquivosReserva`)
do exe antigo: depois de recompilar o Finder, mande `WM_CLOSE` para ela; o Shell abre outra (`finder_reserva`).

### 4.4 Pacote de instalação
`tools/empacotar.ps1` (rodar pelo WMI, para pegar os ajustes/fontes/cursor reais) gera
`dist/SequoiaShell-AAAA-MM-DD.zip`: `bin` (só os exes/dlls de produção), `icones`, `sons`, `fontes` SF,
`cursor`, `ajustes`, `tools` (ícones do sistema e diálogo) e `instalador/*`.
`instalador/instalar.ps1` instala em `%LOCALAPPDATA%\Programs\SequoiaShell` (sem administrador),
pergunta: cursor do Mac, Abrir/Salvar do Mac, ícones do Mac no Windows (HKCU e, opcional, HKLM/desktop.ini),
início automático (tarefa agendada `SequoiaShell`), abrir agora. `desinstalar.ps1` desfaz tudo pelo
`instalacao.log`. **Fora do pacote:** a troca da tela de logon (AuthUX, seção 12).

---

## 5. Programas (bin\)

| Exe / DLL | Fonte | O que é |
|---|---|---|
| `SequoiaShell.exe` | `src/shell` (46 arquivos, ~27 mil linhas) | barra de menus, Dock, Launchpad, Spotlight, Central de Controle, Notificações, Mission Control, mesa, widgets, bloqueio, Alt+Tab, gênio, etc. |
| `SequoiaFinder.exe` | `src/finder` | Finder: ícones/lista/colunas/galeria, abas, Quick Look, etiquetas, busca, Obter Informações, Abrir/Salvar |
| `SequoiaSettings.exe` | `src/ajustes` | Ajustes do Sistema (Sequoia), muda o Windows de verdade |
| `SequoiaActivityMonitor.exe` | `src/monitor` | Monitor de Atividade |
| `SequoiaTerminal.exe` | `src/terminal` | Terminal (PowerShell e cmd) |
| `SequoiaCalculator.exe` | `src/calculadora` | Calculadora (Básica/Científica/Programador) |
| `SequoiaNotes.exe` | `src/notas` | Notas (texto rico, `%APPDATA%\Shell\Notas`) |
| `SequoiaScreenshot.exe` | `src/captura` | Captura de Tela (Win+Shift+3/4/5) |
| `WacOSWindow.dll` / `WacOSWindow32.dll` | `src/janelas` | barra de título do Mac e controles nas janelas dos outros programas (gancho em 64 bits pelo Shell; 32 bits pela própria DLL dentro do rundll32 de 32 bits do Windows, entrada WacOSHost) |
| `WacOSTitleBars.exe` | `src/titulos` + `src/shell/janelas.cpp` | só as barras de título do Mac, sem o Shell (`--sair` desliga; o Shell fecha e assume). Os apps sem botões próprios (app2d.h, Pré-Visualização) carregam a `WacOSWindow.dll` sozinhos (`src/comum/barra_mac.h`, entrada `WacOSPropria`); o Shell não põe gancho em exe com "wacos" no nome |
| `DialogoMac.dll` / `DialogoMac32.dll` | `src/dialogo` | troca IFileOpenDialog/IFileSaveDialog pelo Finder (HKCU CLSID, `tools/dialogo-ligar.ps1`) |
| `ferramentas\MenuTeste.exe` | `src/tools/menu_teste.cpp` | bancada do menu de contexto (`--dump`, `--demo`, `--demo <pasta> --quente`) |

Nomes antigos (Shell.exe, Arquivos.exe, Ajustes.exe, MacJanela.dll...) só aparecem em `src/comum/nomes.h`
para reconhecer processos velhos. Use sempre as macros `EXE_*`.

### 5.1 Módulos do Shell (src/shell)
abrir (animação de abrir janelas, 180 ms, miniatura DWM), alertas (confirmações do Explorer no alerta do Mac),
ampliar (zoom do botão verde e fechar animado), anim (relógio de alta precisão, curvas e molas do macOS),
apps (Dock: fixos em dock.txt, abertos, recentes), atalhos (tarefas do app no menu do Dock),
backdrop (vidro: Composition + **vidro real** por captura de janelas), bloqueio (tela de bloqueio própria),
boot (tela de inicialização), cantos (Cantos Ativos), central (Central de Controle + Editar Controles),
dialogs (Abrir/Salvar trocado por fora), dock, foto/fotogpu (fotos de janelas), genio (efeito gênio),
indice (índice de arquivos do Spotlight), instalados (lista de apps), jogo (Modo Jogo: jogo na frente desliga vidro real, ganchos de posição e de mouse, Cantos Ativos e o índice; `modo_jogo`, `jogos.txt`), janelas (aplica a SequoiaWindow.dll),
launchpad (Launchpad/Apps do Tahoe), limites (barra e Dock são limites das janelas), liquid (desenho do Tahoe),
menubar, menus (menus Win32 dos apps no visual do Mac, sem injetar), mesa (papel de parede e ícones, sem
Explorer), mesas (áreas virtuais), missioncontrol (Mission Control e Exposé), notarapida, notificacoes,
pastas (troca janelas de pasta do Explorer pelo Finder), pilhas (Pilhas do Dock), ponteiro (sacudir para
localizar), progresso (cópia/exclusão estilo Mac), revelar (clique no papel de parede mostra a mesa),
seletor (Cmd+Tab no Alt+Tab), sobre (Sobre Este Mac), som (menu de som), sombras (sombra do Mac em apps
Chromium/Store), spotlight, tray (bandeja própria), ui (desenho comum + FrameClock), verde (menu do botão
verde), widgets (widgets da mesa).

### 5.2 Código compartilhado
- `src/menu/context_menu.cpp/.h`: **menu de contexto próprio** sobre as extensões reais do Windows
  (`IContextMenu` → lê o HMENU → desenha no visual do Mac, vidro por Composition no Tahoe). Usado por todos
  os apps. API: `ctxmenu::Init`, `Show`, `ShowItems`, `Warmup`, `WarmupAsync`, `Close`, `IsOpen`.
- `src/comum/`: `nomes.h` (exes), `idioma.h` + `idioma_en.inc` (tradução), `vidro.h` (borda do Liquid Glass:
  `vidro::Borda`, `vidro::Gosma`), `sombra.h` (sombra grande do Mac), `ampliacao.h` (abrir/fechar animado
  pelo Shell para qualquer app nosso), `simbolos.h` (SF Symbols em Direct2D).
- `src/finder/gfx.cpp, shadow.cpp, textfield.cpp, tahoe.cpp` são reaproveitados por Ajustes, Monitor e Notas.

---

## 6. Referência visual (não fuja disso)

- **Fonte:** SF Pro Text/Display/Rounded/Mono se instalada; senão Inter; senão Segoe UI Variable. Texto
  em Regular com SF Pro; Medium com Inter. A fonte não vai no repositório (licença Apple; o pacote leva
  para uso pessoal).
- **Ícones:** `icones/sequoia`, `icones/tahoe`, `icones/tahoe-macosicons`, `icones/simbolos` (SF Symbols em
  SVG), `icones/bloqueio`, `icones/extra`. Ícones do sistema do Windows trocados por `tools/icones-sistema*.ps1`
  (.ico em `bin/icones-sistema`, gerados por `tools/icones-sistema.py`).
- **Barra de menus:** 24 pt, SF Pro 13 pt, nome do app em negrito, destaque arredondado (raio 4 pt);
  no Tahoe, cápsula de vidro no item do painel aberto.
- **Dock:** ícone 48 pt, ampliação até 128 pt (desligada por padrão), ponto de 4 pt, etiqueta ao passar o
  mouse, pulo ao abrir. Ordem: Finder · fixos · | · abertos/recentes · | · Downloads · Lixeira.
- **Tema Tahoe (Liquid Glass)** (`liquid_glass=1`): vidro claro com lente (refração por Lei de Snell),
  borda especular (`vidro::Borda`), metaballs no Spotlight (`vidro::Gosma`). Menus de contexto: vidro
  **tingido e bem menos transparente**. Central de Controle: só desfoque (sem véu escuro), módulos de vidro
  claro.
- **Tema Mac OS X Leopard** (`leopard=1` sem `liquid_glass`): o Sequoia por baixo e, por cima, o Aqua de 2007 —
  barra de menus de 22 pt clara e translúcida (Lucida, maçã grafite) e destaque azul; menus brancos com destaque azul de ponta a ponta (`menubar.cpp`), Dock em prateleira de vidro
  em perspectiva com reflexo e a luz azul do app aberto (`dock.cpp`, sem o vidro de fundo), botões de gel
  (`semaforo::DrawGel`) e o metal cinza das janelas "unified" (`comum/leopard.h`: DLL das janelas, Finder, Ajustes).
  Finder e Ajustes ficam sempre claros (o 10.5 não tinha modo escuro). Ícones em `icones\leopard`
  (`tools/icones-leopard.py`, do pacote "Leopard Icons" do usuário; o que faltar vem do Sequoia). O papel de parede
  Aurora é só sugestão (botão em Aparência); trocar de tema nunca muda o papel de parede.
  Cursor: ao entrar no Leopard o Shell aplica o esquema "Mac OS X Leopard" em HKCU (seta clássica e o cata-vento
  colorido; `cursor\Leopard`, `tools/cursor-leopard.py`) e guarda o anterior em `%APPDATA%\Shell\cursor-antes.txt`,
  que volta ao sair do tema (`ponteiro::TemaLeopard`). Finder: barra de 55 px (título centralizado na linha de 22,
  botões com moldura metálica), pastas azuis do pacote, seleção azul em degradê e barra de rolagem Aqua. Ajustes:
  caixas de seleção, pop-ups e botões em gel Aqua, fundo cinza 232. Medidas tiradas de capturas do 10.5 a 1x.
  Regra do usuário: o Leopard tem desenho PRÓPRIO (nada reaproveitado do visual dos outros temas). Peças:
  Ajustes = Preferências do Sistema do 10.5 (`RenderLeopard` em `ajustes/main.cpp`: sem barra lateral, "Mostrar Todos"
  em faixas por categoria com os ícones `icones\leopard\pref <id>.png`, painéis em formulário com rótulo à direita);
  Aparência própria (`AparenciaLeopard`: Azul/Grafite, Cor de realce, barra de menus translúcida); Finder = barra de
  55 px com ◀▶/arrumar/modos/engrenagem/busca, barra lateral FAVORITOS/DISPOSITIVOS/COMPARTILHADO/BUSCAR POR
  (`search::Smart` no índice), Cover Flow (4ª visão: lista + capas em perspectiva 3D com reflexo, `DrawCoverFlow`),
  Quick Look em HUD escuro; Shell = Spotlight preso na lupa (`spotlight.cpp PaintLeopard`), Exposé sem a faixa de
  Mesas, alertas do 10.5 (`alertas.cpp`), Sobre Este Mac do 10.5 (dados reais do PC), Pilha em Grade escura e a pasta
  Aplicativos à direita da Dock abrindo como pilha (`pilhas::ShowApps`, atalhos do Menu Iniciar), Quick Look da mesa
  em HUD. Ícones dos apps sem equivalente no pacote: o ícone moderno "aquificado" (`tools/aqua_icone.py`).
  Teste sem clicar: `ShellAbrirPainel` 2 Exposé, 7 Spotlight, 8 Sobre, 11 Quick Look, 12 pilha Aplicativos.
- **Vidro real** (`vidro_real=1`): o fundo do vidro é montado das janelas que estão atrás (captura de
  janelas do Windows), então as peças aparecem nas gravações. Desenha só as áreas das peças e só remonta
  quando a **região alterada** de um quadro (Windows 11 24H2, `DirtyRegionMode::ReportOnly`) passa atrás
  de uma peça.
- **Animações (tempos de referência):** abrir janela 180 ms (cresce de 94% a 100% com fade);
  fade de saída do menu 160 ms; piscada do item escolhido 2×50 ms; miniaturas entram com fade de 150 ms;
  Launchpad 220 ms com mola; Spotlight com mola ágil (96%→100%). Curvas e molas em `src/shell/anim.cpp`.
  O menu de contexto **abre instantâneo** (como no Mac) e só some com fade.
- **Textos:** os nomes e a ordem do macOS em pt-BR (ex.: "Ajustes do Sistema", "Obter Informações",
  "Mover para a Lixeira").

---

## 7. Configuração e comunicação entre processos

### 7.1 Ajustes
Arquivo `%APPDATA%\Shell\ajustes.txt` (UTF-8 com BOM, `chave=valor`). Leitura: `shell::Setting(L"chave",
L"padrão")`. Depois de gravar, avisar com a mensagem `ShellAjustesMudaram` (broadcast). Para gravar de
fora do contêiner: `bin/icones-sistema/ajuste.ps1` lançado pelo WMI.

Chaves (padrão): `ajustes_pagina=geral` `ampliacao=0` `ampliacao_tamanho=128` `animar_abertura=1`
`animar_janelas=1` `animar_paineis=1` `aparencia_auto` `apps_ordem` `atalhos_mac=1` `banners_mac=1`
`barra_bateria=1` `barra_bluetooth=1` `barra_fundo=1` `barra_icones` `barra_icones_todos=0` `barra_som=1`
`barra_spotlight=1` `barra_teclado=1` `barra_wifi=1` `bloqueio=1` `bloqueio_ao_apagar=1` `bloqueio_desfoque=0`
`bloqueio_winl=1` `boot=0` `canto_nota_rapida=1` `central_veu=1` `chromium_sem_oclusao=1`
`clique_mostra_mesa=1` `destaque=azul` `dialogo_por_fora=1` `dock_clique_oculta=1` `estilo_icones=padrao`
(padrao/escuro/transparente/tingido) `explorer` (1 / 0 / fundo) `finder_padrao=1` `finder_reserva=1`
`genio=1` `genio_motor=composicao` `idioma=pt` (pt/en) `indicadores=1` `janelas_mac=1`
`launchpad_motor=composicao` `launchpad_ocultos` `leopard` (tema Mac OS X Leopard; só vale com `liquid_glass=0`) `limitar_janelas=1` `liquid_estilo=nitido`
`liquid_glass` `liquid_refracao=0.6` `mc_motor=composicao` `menus_do_windows=1` `menus_fade=1` `ocultar=0`
`pilha_ordem=adicao` `pilha_ver=auto` `pilhas=1` `progresso_mac=1` `pulo_notificacao=1`
`reduzir_transparencia=0` `relogio_curto=0` `revelar_motor=composicao` `sacudir_ponteiro=1` `sombras_mac=1`
`spotlight_google=1` `tamanho=48` `tecla_windows_launchpad=1` `tema=auto` `vidro_real=1` `vidro_tinta=0.5`
`volume_mac=1` `cc_layout` (ordem da Central) e `widget.*`.
Personalização (vazio = padrão do Mac): `dock_espaco` `dock_margem` `dock_raio` (0-100) `dock_cor` (auto/clara/escura/destaque)
`dock_opacidade` (0-100) `dock_posicao=baixo` (esquerda/direita; Leopard sempre embaixo) `dock_launchpad=1` `dock_separador=1` `dock_rotulos=1` `dock_indicador=ponto` (ou traco)
`mesa_icone_tamanho=64` `mesa_espaco=0` `mesa_texto=12` `notif_duracao=5`; ícones próprios do Dock em `%APPDATA%\Shell\icones-dock`
(PNG com o nome do .exe ou do app). Quem recebe a DLL das janelas (e quem nunca recebe: jogos/anti-cheat): `src/comum/jogos.h`.
Outros arquivos em `%APPDATA%\Shell`: `dock.txt`, `mesa.txt` (posições), `dialogo.txt` (programas que usam o
Abrir/Salvar do Mac; `*` = todos), `janelas-excluidas.txt`, `Notas\`. Finder: `%APPDATA%\Arquivos\`
(`favoritos.txt`, `etiquetas.txt`, `padrao-backup.txt`).

### 7.2 Mensagens registradas (RegisterWindowMessageW)
`ShellReiniciar`, `ShellAjustesMudaram`, `ShellAbrirCentral`, `ShellAbrirNotificacoes`,
`ShellAbrirSpotlight`, `ShellBloquearTela`, `ShellMenuVerde`, `ShellJanelaAbrir`, `ShellJanelaFechar`,
`ShellAmpliarFoto`, `ShellAmpliarPronto`, `ShellCapturaVisivel`, `ShellVidroFoto` (diagnóstico do vidro),
`MacJanelaAbriu`, `MacJanelaAmpliar`, `MacJanelaDesfazer`, `MacJanelaMostrar`, `MacJanelaMover`,
`SequoiaCaptura`, além de `SHELLHOOK` e `TaskbarCreated` do Windows. Janela de mensagens do Shell: classe
`ShellMensagens`.

### 7.3 Atalhos globais (gancho de teclado do Shell)
Win → Launchpad; Win+Espaço → Spotlight; Win+Tab / Ctrl+Win+↑ → Mission Control; Ctrl+Win+↓ → Exposé do
app; Win+Q/H/W (atalhos_mac); Win+E → Finder; Win+L → nossa tela de bloqueio quando possível; Alt+Tab →
seletor de apps; Win+Shift+3/4/5 → Captura; sair do Shell Ctrl+Alt+Shift+F12; emergência (devolve o
Explorador) Ctrl+Alt+Shift+F11.

### 7.4 Tradução
`idioma::Tr(L"texto em pt")` em todo texto visível; pares em `src/comum/idioma_en.inc`
(`{ L"pt", L"en" },`). Todo texto novo precisa do par em inglês.
Idiomas (`idioma=<código>` no ajustes.txt, Ajustes > Geral > Idioma): pt, en (padrão), es, fr, de, it, ja,
zh (chinês simplificado), ko, ru, pl, tr. Um dicionário por idioma, `idioma_<código>.inc`, com as mesmas
chaves em português do `idioma_en.inc`; o que faltar num idioma sai em inglês. `idioma::Code()`/`Locale()`
dão o código e o locale (para APIs e DirectWrite); `idioma::En()`/`Pt()` continuam. Datas e nomes de
mês/dia: `idioma::Date(st, D_...)`, `DateTime`, `MonthName`, `DayName` (formato do Windows no locale
escolhido), nunca `"%d de %ls"` nem tabelas fixas de meses. `python tools/idioma-conferir.py` lista as
chaves que faltam em cada idioma (texto novo: pôr no en e, se der, nos outros).

---

## 8. Integração com o Windows (o que já existe e como)

- **Sem Explorer / mesa própria**: ver `docs/sem-explorer.md` (modos `explorer=1/0/fundo`).
- **Pastas abertas por outros apps** (`pastas.cpp`): o Shell pega a janela `CabinetWClass` do Explorer
  **ao nascer** (EVENT_OBJECT_CREATE), deixa-a invisível (camada alfa 0) e fora da tela, lê pasta e seleção
  (IShellWindows), abre o Finder no lugar e **só fecha o Explorer depois que o Finder pegou a frente**
  (senão o foco volta para quem pediu e o Finder fica atrás). O Finder ainda se defende 3 s (`T_FRENTE`)
  se outra janela tomar a frente sem clique/tecla.
- **Finder como padrão** (`finder/padrao.cpp`): só `.zip` (verbo em CompressedFolder) e Win+E (HKCU),
  backup em `%APPDATA%\Arquivos\padrao-backup.txt`. Pastas comuns vão pelo `pastas.cpp`.
- **Abrir/Salvar do Mac**: `DialogoMac.dll` registrada em HKCU (64 e 32 bits) + fallback "por fora"
  (`dialogs.cpp`, `dialogo_por_fora`).
- **Barra de título do Mac nos outros apps**: `SequoiaWindow.dll` por gancho (64 bits) e pelo host de
  32 bits; apps Chromium/Store recebem só a sombra (`sombras.cpp`).
- **Menus dos apps** (`menus.cpp`): os menus Win32 reais desenhados no visual do Mac, sem injetar DLL.
- **Ícones do Windows trocados**: `tools/icones-sistema.ps1` (HKCU; backup `docs/backup-icones-sistema.json`;
  `-Desfazer`) e `tools/icones-sistema-extra.ps1` (desktop.ini das pastas do usuário + HKLM Shell Icons com
  administrador; backups em `docs/`).
- **Tela de bloqueio própria** (`bloqueio.cpp`): Win+L do Windows não pode ser desligado sem administrador
  (a chave `DisableLockWorkstation` é só leitura para o usuário), então a do Windows ainda aparece em
  alguns casos. A troca completa da tela de logon/bloqueio do Windows é o trabalho do AuthUX (seção 12).
- **Atualizações**: travadas no 25H2 (`tools/travar-atualizacoes.ps1`, backup `docs/backup-atualizacoes.json`).

---

## 9. Regras de desempenho e fluidez (aprendidas e medidas)

1. **Animação anda no ritmo da tela, nunca por SetTimer.** `SetTimer` tem resolução de ~15,6 ms (64 Hz)
   e dá trancos numa tela de 174 Hz. Use: `ui::FrameClock` (thread com `DwmFlush` + `ui::FrameAck` para
   não acumular quadros) no Shell; no Finder, o passo da animação no `WM_PAINT` com `Present(1, 0)` e
   `Invalidate()` enquanto houver movimento; ou animações de Composition (rodam no DWM). Passos
   **por tempo real** (`QueryPerformanceCounter`), nunca "x% por tique".
   Medido: rolagem do Finder de ~64 para 186 quadros/s.
2. **Nada pesado na thread da interface.** Carregar extensões de menu (`ctxmenu::WarmupAsync`), miniaturas
   (2 a 4 threads, pedidos mais recentes primeiro), índice e busca vão para threads.
3. **Pré-carregar o que o primeiro clique usa.** Menus: o Shell pré-carrega 3 s depois de abrir, o Finder
   300 ms depois; o vidro do menu (D3D/Composition) também é criado aí. Menu frio: 500-750 ms; pré-carregado:
   ~70 ms.
4. **Nunca bloquear esperando animação de saída.** O fade do menu roda sozinho (`FadeOutAsync`), a ação
   escolhida acontece na hora e cliques passam pelo menu que some.
5. **Finder reserva**: um processo escondido já pronto; o pedido chega por `WM_COPYDATA`; janela na tela em
   ~40-60 ms e revelada em ~270 ms. O Finder avisa o Shell para animar assim que desenha
   (`ampliacao::Revelar`), sem esperar a primeira pintura.
6. **Ocioso tem que ser ocioso.** Temporizadores só existem enquanto há trabalho pendente; nada de reler
   listas 10×/s se o Windows já avisa por eventos. Medido: Shell parado de ~6% para ~2% de um núcleo.
7. **Meça antes e depois.** Os logs já trazem: `menu na tela em X ms`, `QueryContextMenu: X ms`,
   `abertura (reserva): ... X ms`, `pasta: ... (N itens, X ms)`, `rolagem: N quadros em X ms (F quadros/s)`,
   `vidro real: 10 s: ... quadros ... montagens`.

---

## 10. Testar e verificar

### 10.1 Logs (bin\)
`shell.log` (+ `shell-anterior.log`), `arquivos.log` (Finder), `dialogo.log`, `calculadora.log`,
`captura.log`, `monitor.log`, `notas*.log`, `terminal.log`, `ferramentas\menu-teste.log`.

### 10.2 Ferramentas
- Menu sem clicar: `bin\ferramentas\MenuTeste.exe --demo <pasta> [--quente]` (abre o menu da 1ª linha,
  mede e fecha).
- Gravar a tela para ver piscadas: `ffmpeg -f gdigrab -framerate 30 -t 5 -i desktop ...` e extrair quadros
  em mosaico (`-vf "select=...,tile=8x8"`).
- Medir abertura do Finder: iniciar `SequoiaFinder.exe "<pasta>"` e medir até a janela `ArquivosJanela`
  nova ficar visível e sem cloak (`DWMWA_CLOAKED`).
- "Mostrar na pasta": `explorer.exe /select,"<arquivo>"` e registrar a janela da frente a cada 15 ms.
- Diagnóstico do vidro: mensagem `ShellVidroFoto` grava `bin\vidro-*.bmp`.

### 10.3 VM de testes (Hyper-V `AuthUX-Teste`)
Windows 11 Pro en-US, Geração 2, vTPM, Secure Boot. Pontos de verificação: "antes do AuthUX",
"antes do Windows Mac". Ferramentas (pedem UAC; lançar pelo WMI):
`tools/vm-foto.ps1` (tela → `docs/vm-tela.png`), `tools/vm-digitar.ps1` (`-Texto`, `-Enter`, `-Teclas "9,13"`,
`-Combo "91,82"`, `-Clique "x,y"`, `-Texto CTRLALTDEL`), `tools/vm-enviar.ps1` (arquivo do PC → VM, sem
senha), `tools/vm-authux.ps1`, `tools/vm-windows-mac*.ps1`, `tools/vm-resolucao.ps1`, `tools/vm-gpu-estado.ps1`.
A conta Teste foi vinculada à conta Microsoft: PowerShell Direct com a senha local **não funciona** mais;
use `vm-enviar.ps1` + Win+R. Não reinicie nem faça logoff da VM sem avisar o usuário.

---

## 11. Checklist para cada mudança

1. Ler o arquivo e o comentário do topo dele (cada .cpp explica o porquê das decisões; respeite).
2. Medir antes, se for desempenho.
3. Mudar no estilo do código existente: comentários em pt-BR explicando o **porquê**, nomes em pt-BR,
   `Log(L"...")` nos pontos importantes, `Tr()` em todo texto visível + par em `idioma_en.inc`.
4. Conferir `cl/link` rodando; mover os exes travados; compilar o alvo; ver `error`/`fatal`.
5. Conferir que o exe novo existe; reiniciar o Shell (ou fechar a reserva do Finder).
6. Verificar de verdade (log, foto, gravação, medida). Se não deu para verificar, dizer isso claramente.
7. Mudou algo no sistema? Anotar em `docs/registro-mudancas.txt` com o desfazer.
8. Fechar as janelas de teste que abriu. Não commitar.
9. Relatar em pt-BR: o que mudou, números antes/depois, o que não foi testado.

---

## 12. Tela de logon do Windows (AuthUX) — só na VM

- Projeto `%USERPROFILE%\Documents\AuthUX` (fork de explorer7-team/AuthUX), `sdk/inc/version.h` com
  `CONSOLELOGON_FOR_GE` (builds 26100/26120/26200). Build: `MSBuild AuthUX.sln -p:Configuration=Release.GE
  -p:Platform=x64`.
- Instala trocando 2 valores protegidos: `HKLM\SOFTWARE\Microsoft\WindowsRuntime\ActivatableClassId\
  Windows.Internal.UI.Logon.Controller.LogonUX\DllPath` e `...LockScreenHost\DllPath` (padrões:
  `%SystemRoot%\system32\Windows.UI.Logon.dll` e `...\logoncontroller.dll`). Teste seguro: Ctrl+Alt+Del.
- Redesenho Mac: `AuthUX\mac\layout_mac.py`, `mac\gerar_recursos.py`, relógio/desfoque/avatar redondo em
  `logonframe.cpp`/`usertileelement.cpp`; versão com Windows.UI.Composition (pílula de vidro, animações com
  relógio próprio a 60 Hz). DirectUI rejeita BMP totalmente transparente e pré-multiplica o alfa sozinho
  (grave alfa direto). Erros em `C:\Users\Public\AuthUX-mac.log` na VM.
- Na VM funcionam: Ctrl+Alt+Del, bloquear, desbloquear com senha e com PIN da conta Microsoft.
  Em aberto: "Switch User" volta para a conta bloqueada; correções do "Welcome"/fade ao reiniciar ainda
  sendo testadas. **Nada disso vai para o PC real sem o OK explícito do usuário.**

---

## 13. Pendências (estado em 2026-11-11)

- Pilhas na mesa (agrupar arquivos da área de trabalho por tipo).
- Animação de abrir nos apps de 32 bits.
- Ícones: .md e tipos sem programa ainda com o ícone genérico; testar o `-Desfazer` da parte HKLM.
- Conferir cada app em inglês (`idioma=en`) na tela.
- Downloads no Dock: "Exibir como Pasta / Pilha".
- Do vídeo do Tahoe: cor e emoji nas pastas; Spotlight com nome do modo + "⌘4" e filtros como fichas;
  estilo de ícones também no Launchpad/Spotlight/widgets; tingir janelas com o papel de parede; cantos
  maiores.
- Barra de menus (sessão "substituir o Windows"): Central de Controle ainda abre as configurações rápidas do
  Windows no canto de baixo; o relógio ainda não abre as notificações.
- Gerar o pacote de novo (o `dist\SequoiaShell-2026-11-11.zip` é anterior às melhorias de desempenho).
- Limpar `%LOCALAPPDATA%\SequoiaBinariosAntigos` (só com autorização: apaga arquivos).
- Limitações conhecidas: `docs/problemas-conhecidos.md`.
