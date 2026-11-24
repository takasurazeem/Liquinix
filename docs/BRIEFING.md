# Briefing completo do projeto

> **English summary.** This is the most complete technical overview of Liquinix, written in Portuguese like most
> of the code. It covers what each program and shell module does (sections 5 and 5.1), the visual reference for
> each theme (6), settings keys and the registered messages between processes (7), how Liquinix integrates with
> Windows (8), the performance rules (9) and how to test (10, 11). It came from upstream WacOS before the fork and
> was updated for Liquinix: names, paths and the release package follow the fork. The project rules live in
> [`AGENTS.md`](../AGENTS.md) and the skills in `.claude/skills/`; where this document disagrees with them or
> with the code, they win. Work is tracked on the project board, in the order of [`ROADMAP.md`](ROADMAP.md).

Herdado do WacOS (estado de 2026-11-11) e atualizado para o Liquinix. Quando este documento e o código
divergirem, o código manda; corrija o documento. As regras do projeto estão no `AGENTS.md`. Documentos de apoio:
`docs/dock-e-barra-de-menus.md`, `docs/sem-explorer.md`, `docs/problemas-conhecidos.md`, `docs/pendencias.md`,
`docs/estudo-bandeja.md`, `docs/windows-vm.md` (VM de testes) e `docs/linux-dev.md` (o lado Linux).

---

## 1. O que é

O Liquinix (fork GPL-3.0 do WacOS, ver `PROVENANCE.md`) é um "macOS por cima do Windows 11": um shell próprio em **C++ Win32 + Direct2D/DirectWrite +
DirectComposition / Windows.UI.Composition**, sem frameworks, que substitui a experiência do Windows
(barra de tarefas, Iniciar, Explorador, menus, diálogos, tela de bloqueio) pelo visual e comportamento do
**macOS Sequoia** e, com o tema ligado, do **macOS 26 Tahoe (Liquid Glass)**. Kernel, drivers e apps do
Windows continuam os mesmos. Uma versão para Linux está planejada (`docs/linux-dev.md`, skill
`liquinix-platform-layer`).

Objetivo de qualidade: **ficar idêntico ao Mac** (medidas, curvas, tempos e textos), fluido (no ritmo da tela,
medido numa de 174 Hz) e reversível. Sem nenhum arquivo da Apple no repositório (regra 2 do `AGENTS.md`).

---

## 2. Regras de trabalho

As regras do projeto (licença, arquivos da Apple, dados pessoais, git) estão no `AGENTS.md`. Estas são as de
engenharia que valem para qualquer mudança:

1. **Reversível sempre.** Toda mudança no sistema tem um "desfazer" e fica anotada no log de quem a fez
   (o instalador usa `instalacao.log`; os scripts de `tools/` guardam backup antes de mudar).
2. **Não editar arquivos do sistema** num PC real (System32, imageres.dll etc.). Só registro (de preferência
   HKCU) e arquivos próprios. Na **VM de testes** é permitido (é para isso que ela existe).
3. **Avisar antes de qualquer ação irreversível** (registro de shell, remover arquivos, políticas) e esperar o OK.
4. **Não registrar o Shell como shell do Windows** (Winlogon Shell) num PC real; isso só se testa na VM.
5. **Reiniciar o Shell só depois de o executável novo existir** (seção 4.3).
6. **Não fechar apps do usuário sem perguntar.** Fechar janelas que você mesmo abriu para teste é permitido.
7. **Evitar simulação de entrada** (SendInput, keybd_event, mouse_event) num PC real. Mensagens diretas para
   janelas dos nossos próprios apps (ex.: WM_MOUSEWHEEL para medir) são aceitáveis. Na VM pode.
8. **Nunca guardar chaves de API, senhas ou dados de uma conta** no repositório nem nos logs.

---

## 3. Ambiente

- Windows 11, Visual Studio 2022 ou Build Tools 2022 (MSVC v143), Windows SDK 10.0.26100.0, C++20.
- De um PC com Linux: o código Windows se compila e testa numa VM (`docs/windows-vm.md`); o CI compila
  `build.cmd` e `build.cmd janelas` em cada pull request.
- `DllImport` no PowerShell com `CharSet = CharSet.Unicode`.
- Scripts .ps1 do Windows PowerShell 5.1 **com BOM UTF-8** (sem BOM os acentos quebram).
  `ajustes.txt` é UTF-8 com BOM.
- Explorer fica rodando "no fundo" (`explorer=fundo`): apps da Loja (UWP) precisam dele. Nunca ligar e
  desligar o Explorer em ciclo.

---

## 4. Compilar, instalar no lugar e reiniciar

### 4.1 Build
`build.cmd [alvo]` na raiz. Alvos: (vazio = tudo), `shell`, `finder`, `ajustes`, `monitor`, `terminal`,
`calculadora`, `notas`, `captura`, `menu` (bancada MenuTeste), `titulos` (LiquinixTitleBars.exe; também sai no `janelas`), `janelas` (LiquinixWindow.dll 64/32 +
host 32), `dialogo` (DialogoMac.dll), `bandeja`. O build "tudo" **não** inclui `janelas` nem `dialogo`.
Ícone e descrição de cada exe: `tools/recursos.ps1` (gerado no build).
Flags: `/O2 /W3 /std:c++20 /utf-8 /DUNICODE`. Sem MSBuild, sem projeto .vcxproj.

### 4.2 Antes de compilar
1. Conferir se há `cl.exe`/`link.exe` rodando (outra sessão pode estar compilando; o PDB do shell
   conflita).
2. Executáveis em uso não podem ser sobrescritos: **mova** o exe em uso para fora de `bin\` antes do build
   (o Windows deixa renomear arquivo em uso). Só mova o que está travado. Se mover algo que o alvo não
   recompila (ex.: o host de 32 bits no build "tudo"), devolva a cópia. Detalhes: skill `liquinix-build-verify`.

### 4.3 Reiniciar o Shell
`PostMessage(FindWindowW(L"ShellMensagens"), RegisterWindowMessageW(L"ShellReiniciar"))`.
**Só depois de conferir que `bin\LiquinixShell.exe` existe.** Se o Shell morreu e não há exe, devolva a
versão anterior que você tirou de `bin\` e abra de novo.
O Finder mantém uma **reserva** escondida (janela `ArquivosJanela` com título `\u200BArquivosReserva`)
do exe antigo: depois de recompilar o Finder, mande `WM_CLOSE` para ela; o Shell abre outra (`finder_reserva`).

### 4.4 Pacote de instalação
`tools/empacotar.ps1 -SemSistema -SemAjustes` gera `dist/Liquinix-AAAA-MM-DD.zip` (é o que `tools/publicar.ps1`
publica nas releases): `bin` (só os exes/dlls de produção), `icones` (só os rastreados pelo git), `tools` e
`instalador/*`. **Nunca** entram fontes, sons, cursores nem nada da Apple: o instalador oferece baixar isso no
PC do usuário (`tools/baixar-apple.ps1`), e o `empacotar.ps1` recusa o pacote se encontrar algum.
O `LiquinixSetup.exe` (`src/setup`) e o `instalador/instalar.ps1` instalam em `%LOCALAPPDATA%\Programs\Liquinix`
(sem administrador) e perguntam: só os apps, só as barras de título, ou tudo; cursor e fontes do Mac; Abrir/Salvar
do Mac; ícones do Mac no Windows; início automático (tarefa agendada `Liquinix`). `desinstalar.ps1` desfaz tudo
pelo `instalacao.log`. Uma instalação antiga com o nome "Sequoia Shell" é migrada (`instalar.ps1`, passo 1b).

---

## 5. Programas (bin\)

| Exe / DLL | Fonte | O que é |
|---|---|---|
| `LiquinixShell.exe` | `src/shell` (46 arquivos, ~27 mil linhas) | barra de menus, Dock, Launchpad, Spotlight, Central de Controle, Notificações, Mission Control, mesa, widgets, bloqueio, Alt+Tab, gênio, etc. |
| `LiquinixFinder.exe` | `src/finder` | Finder: ícones/lista/colunas/galeria, abas, Quick Look, etiquetas, busca, Obter Informações, Abrir/Salvar |
| `LiquinixSettings.exe` | `src/ajustes` | Ajustes do Sistema (Sequoia), muda o Windows de verdade |
| `LiquinixActivityMonitor.exe` | `src/monitor` | Monitor de Atividade |
| `LiquinixTerminal.exe` | `src/terminal` | Terminal (PowerShell e cmd) |
| `LiquinixCalculator.exe` | `src/calculadora` | Calculadora (Básica/Científica/Programador) |
| `LiquinixNotes.exe` | `src/notas` | Notas (texto rico, `%APPDATA%\Shell\Notas`) |
| `LiquinixScreenshot.exe` | `src/captura` | Captura de Tela (Win+Shift+3/4/5) |
| `LiquinixWindow.dll` / `LiquinixWindow32.dll` | `src/janelas` | barra de título do Mac e controles nas janelas dos outros programas (gancho em 64 bits pelo Shell; 32 bits pela própria DLL dentro do rundll32 de 32 bits do Windows, entrada LiquinixHost) |
| `LiquinixTitleBars.exe` | `src/titulos` + `src/shell/janelas.cpp` | só as barras de título do Mac, sem o Shell (`--sair` desliga; o Shell fecha e assume). Os apps sem botões próprios (app2d.h, Pré-Visualização) carregam a `LiquinixWindow.dll` sozinhos (`src/comum/barra_mac.h`, entrada `LiquinixPropria`); o Shell não põe gancho em exe com "liquinix" no nome |
| `DialogoMac.dll` / `DialogoMac32.dll` | `src/dialogo` | troca IFileOpenDialog/IFileSaveDialog pelo Finder (HKCU CLSID, `tools/dialogo-ligar.ps1`) |
| `LiquinixPreview.exe`, `LiquinixReminders.exe`, `LiquinixCalendar.exe`, `LiquinixTextEdit.exe`, `LiquinixClock.exe`, `LiquinixWeather.exe`, `LiquinixContacts.exe`, `LiquinixPhotos.exe`, `LiquinixTimeMachine.exe`, `LiquinixMusic.exe`, `LiquinixQuickTime.exe`, `LiquinixForceQuit.exe` | `src/preview`, `src/lembretes`, `src/calendario`, `src/textedit`, `src/relogio`, `src/clima`, `src/contatos`, `src/fotos`, `src/timemachine`, `src/musica`, `src/quicktime`, `src/forcar` | os outros apps (a maioria sobre `src/comum/app2d.h`) |
| `LiquinixSetup.exe` | `src/setup` | instalar, atualizar, mudar e desinstalar |
| `ferramentas\MenuTeste.exe` | `src/tools/menu_teste.cpp` | bancada do menu de contexto (`--dump`, `--demo`, `--demo <pasta> --quente`) |

Nomes antigos (Shell.exe, Arquivos.exe, Ajustes.exe, MacJanela.dll, Sequoia*.exe...) só aparecem em `src/comum/nomes.h`
para reconhecer processos velhos. Use sempre as macros `EXE_*`.

### 5.1 Módulos do Shell (src/shell)
abrir (animação de abrir janelas, 180 ms, miniatura DWM), alertas (confirmações do Explorer no alerta do Mac),
ampliar (zoom do botão verde e fechar animado), anim (relógio de alta precisão, curvas e molas do macOS),
apps (Dock: fixos em dock.txt, abertos, recentes), atalhos (tarefas do app no menu do Dock),
backdrop (vidro: Composition + **vidro real** por captura de janelas), bloqueio (tela de bloqueio própria),
boot (tela de inicialização), cantos (Cantos Ativos), central (Central de Controle + Editar Controles),
dialogs (Abrir/Salvar trocado por fora), dock, foto/fotogpu (fotos de janelas), genio (efeito gênio),
indice (índice de arquivos do Spotlight), instalados (lista de apps), jogo (Modo Jogo: jogo na frente desliga vidro real, ganchos de posição e de mouse, Cantos Ativos e o índice; `modo_jogo`, `jogos.txt`), janelas (aplica a LiquinixWindow.dll),
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
  em Regular com SF Pro; Medium com Inter. A SF não vai no repositório nem no pacote (licença da Apple): o
  instalador oferece baixá-la do site da Apple no PC do usuário.
- **Ícones:** no repositório só `icones/tahoe` (tema MacTahoe, GPL-3.0). O código também procura
  `icones/sequoia`, `icones/simbolos`, `icones/bloqueio`, `icones/extra`, `icones/leopard`... e, sem eles (o normal
  aqui), cai nos ícones do Windows e nos desenhos próprios. Ícones do sistema do Windows trocados por
  `tools/icones-sistema*.ps1` (.ico em `bin/icones-sistema`, gerados por `tools/icones-sistema.py`).
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
  quando existem localmente (`tools/icones-leopard.py`; não vêm no repositório). O papel de parede
  Aurora é só sugestão (botão em Aparência); trocar de tema nunca muda o papel de parede.
  Cursor: ao entrar no Leopard o Shell aplica o esquema "Mac OS X Leopard" em HKCU (seta clássica e o cata-vento
  colorido; `cursor\Leopard`, gerado localmente por `tools/cursor-leopard.py`, fora do repositório) e guarda o anterior em `%APPDATA%\Shell\cursor-antes.txt`,
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
- **Textos:** os nomes e a ordem do macOS, escritos em pt-BR no código (ex.: "Ajustes do Sistema",
  "Obter Informações", "Mover para a Lixeira") e traduzidos pelo `Tr()` (seção 7.4).

---

## 7. Configuração e comunicação entre processos

### 7.1 Ajustes
Arquivo `%APPDATA%\Shell\ajustes.txt` (UTF-8 com BOM, `chave=valor`). Leitura: `shell::Setting(L"chave",
L"padrão")`. Depois de gravar, avisar com a mensagem `ShellAjustesMudaram` (broadcast).

Chaves (padrão): `ajustes_pagina=geral` `ampliacao=0` `ampliacao_tamanho=128` `animar_abertura=1`
`animar_janelas=1` `animar_paineis=1` `aparencia_auto` `apps_ordem` `atalhos_mac=1` `banners_mac=1`
`barra_bateria=1` `barra_bluetooth=1` `barra_fundo=1` `barra_icones` `barra_icones_todos=0` `barra_som=1`
`barra_spotlight=1` `barra_teclado=1` `barra_wifi=1` `bloqueio=1` `bloqueio_ao_apagar=1` `bloqueio_desfoque=0`
`bloqueio_winl=1` `boot=0` `canto_nota_rapida=1` `central_veu=1` `chromium_sem_oclusao=1`
`clique_mostra_mesa=1` `destaque=azul` `dialogo_por_fora=1` `dock_clique_oculta=1` `estilo_icones=padrao`
(padrao/escuro/transparente/tingido) `explorer` (1 / 0 / fundo) `finder_padrao=1` `finder_reserva=1`
`genio=1` `genio_motor=composicao` `idioma=en` (en, pt, es, fr, de, it, ja, zh, ko, ru, pl, tr) `indicadores=1` `janelas_mac=1`
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
(`{ L"pt", L"en" },`). Todo texto novo precisa do par em inglês (skill `liquinix-translations`).
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
- **Barra de título do Mac nos outros apps**: `LiquinixWindow.dll` por gancho (64 bits) e pelo host de
  32 bits; apps Chromium/Store recebem só a sombra (`sombras.cpp`).
- **Menus dos apps** (`menus.cpp`): os menus Win32 reais desenhados no visual do Mac, sem injetar DLL.
- **Ícones do Windows trocados**: `tools/icones-sistema.ps1` (HKCU; backup `docs/backup-icones-sistema.json`,
  gerado na hora e fora do git; `-Desfazer`) e `tools/icones-sistema-extra.ps1` (desktop.ini das pastas do usuário + HKLM Shell Icons com
  administrador; backups em `docs/`).
- **Tela de bloqueio própria** (`bloqueio.cpp`): Win+L do Windows não pode ser desligado sem administrador
  (a chave `DisableLockWorkstation` é só leitura para o usuário), então a do Windows ainda aparece em
  alguns casos. A troca completa da tela de logon/bloqueio do Windows é o trabalho do AuthUX (seção 12).
- **Atualizações do Windows**: `tools/travar-atualizacoes.ps1` (opcional, como administrador) trava o Windows
  Update numa versão; backup em `docs/backup-atualizacoes.json` (gerado na hora, fora do git) e `-Desfazer`.

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
- Medir abertura do Finder: iniciar `LiquinixFinder.exe "<pasta>"` e medir até a janela `ArquivosJanela`
  nova ficar visível e sem cloak (`DWMWA_CLOAKED`).
- "Mostrar na pasta": `explorer.exe /select,"<arquivo>"` e registrar a janela da frente a cada 15 ms.
- Diagnóstico do vidro: mensagem `ShellVidroFoto` grava `bin\vidro-*.bmp`.

### 10.3 VM de testes
Ver `docs/windows-vm.md`: uma VM do Windows 11 (KVM/QEMU) para compilar, rodar e conferir, com instantâneos para
voltar atrás. Trocar o shell do Windows, mexer na tela de logon e o que mais for arriscado só se testa nela.
Os `tools/vm-*.ps1` e `tools/criar-vm-authux.ps1` são do WacOS: dirigem uma VM Hyper-V (`AuthUX-Teste`) a partir de
um Windows e não servem num host Linux.

---

## 11. Checklist para cada mudança

1. Ler o arquivo e o comentário do topo dele (cada .cpp explica o porquê das decisões; respeite).
2. Medir antes, se for desempenho.
3. Mudar no estilo do código existente: comentários em pt-BR explicando o **porquê**, nomes em pt-BR,
   `Log(L"...")` nos pontos importantes, `Tr()` em todo texto visível + par em `idioma_en.inc`.
4. Conferir `cl/link` rodando; mover os exes travados; compilar o alvo; ver `error`/`fatal`.
5. Conferir que o exe novo existe; reiniciar o Shell (ou fechar a reserva do Finder).
6. Verificar de verdade (log, foto, gravação, medida). Se não deu para verificar, dizer isso claramente.
7. Mudou algo no sistema? Deixar o desfazer pronto e anotado (log ou backup do script).
8. Fechar as janelas de teste que abriu. Commits, branches e pull requests como diz o `AGENTS.md`.
9. Relatar: o que mudou, números antes/depois, o que não foi testado (skill `liquinix-build-verify`).

---

## 12. Tela de logon do Windows (AuthUX) — só na VM

O WacOS experimentou trocar a tela de logon e de bloqueio do Windows por uma versão Mac, com um fork do
[AuthUX](https://github.com/explorer7-team/AuthUX) fora deste repositório. A troca mexe em dois valores protegidos
de `HKLM\SOFTWARE\Microsoft\WindowsRuntime\ActivatableClassId\...` (`LogonUX` e `LockScreenHost`), então
**só na VM**, nunca num PC real sem o OK explícito do usuário. Nada disso faz parte do Liquinix hoje; os
`tools/vm-authux*.ps1` ficaram do WacOS. Antes de retomar, conferir a licença do AuthUX e o `AGENTS.md`.

---

## 13. Pendências

O que falta fazer está no quadro do projeto, na ordem do [`ROADMAP.md`](ROADMAP.md). As listas antigas do WacOS
continuam em `docs/pendencias.md`, e as limitações conhecidas em `docs/problemas-conhecidos.md`.

Herdadas do WacOS (estado de 2026-11-11), ainda sem cartão no quadro:

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
