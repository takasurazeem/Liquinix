# Problemas conhecidos e exceções

Lista viva: o que ainda não funciona como no macOS, ou que é limitação do Windows.

## Menu de contexto (src/menu)
- Itens que só existem no menu novo do Windows 11 (`IExplorerCommand` de apps empacotados, ex.: "Abrir no Terminal" da Store) não aparecem.
- Menu do fundo de pasta não tem Colar, Exibir e Classificar por: esses itens dependem da janela do Explorer. No Arquivos, Ctrl+V cola normalmente.
- Os textos vêm do Windows (em inglês neste PC). Tradução/renomeação estilo Mac fica para depois.
- "Enviar para" em pastas preenche aos poucos; o submenu é relido quando abre.

## Arquivos (src/finder)
### Visões
- As quatro visões funcionam: ícones, lista (colunas ordenáveis, grupos), colunas (com prévia do arquivo) e galeria.
- Agrupar vale na lista; nas outras visões só muda a ordem.
- Colunas: a fileira começa na pasta aberta pela barra lateral/caminho; não há rolagem horizontal manual (acompanha a coluna mais à direita).
- Galeria: sem o painel de metadados à direita do Finder.
- "Mostrar Opções de Visualização" (Cmd+J) ainda não existe.

### Vidro, cantos e sombra (limites do Windows)
- A barra lateral usa o acrílico do Windows, que põe uma camada fosca por cima: passa menos cor do fundo que o vidro do Mac. O desfoque sem essa camada (SetWindowCompositionAttribute) foi testado: o DWM o desenha por cima do conteúdo e apaga texto e ícones.
- Cantos da janela: o Windows 11 arredonda com 8 px (no Mac ~10 px). Cantos maiores exigiriam desligar o vidro.
- Apps Chromium/Electron (Chrome, Edge, Discord, VS Code, Obsidian...): os cantos são os do Windows (8 px, a DLL pede o
  arredondamento e troca a borda pelo contorno do Mac). Cantos maiores não dá: o conteúdo deles vem da GPU por
  DirectComposition e o recorte da janela (SetWindowRgn) não o corta (testado no Edge 157, 20/11/2026). Num PC/VM em que o
  Windows não arredonda nada (sem aceleração de vídeo), essas janelas ficam com cantos retos.
- A barra de título do Mac nunca entra em jogos nem em programas com anti-cheat (src/comum/jogos.h). Um jogo não reconhecido
  pode ser posto em %APPDATA%\Shell\jogos.txt ou janelas-excluidas.txt.
- Sombra: além da sombra curta do Windows, uma janela própria desenha a sombra grande e difusa do Mac (src/finder/shadow.cpp).

### Etiquetas e barra lateral
- Etiquetas ficam no próprio arquivo (fluxo NTFS "Arquivos.Etiquetas"); as datas do arquivo são preservadas. Só em discos NTFS locais: em pen drives FAT/exFAT e na rede não dá para marcar.
- O OneDrive não sincroniza fluxos alternativos: num arquivo do OneDrive a etiqueta vale só neste PC.
- A lista de uma etiqueta vem do índice (%APPDATA%\Arquivos\etiquetas.txt): itens marcados fora do Arquivos (ou em outro PC) só aparecem depois de abertos aqui.
- Nomes e cores das etiquetas são fixos (o Finder deixa renomear e criar novas).
- Favoritos ficam em %APPDATA%\Arquivos\favoritos.txt. Só a seção Favoritos é editável; OneDrive, Locais e Etiquetas são fixos.

### Fonte
- O macOS Sequoia usa a SF Pro. O Arquivos (e o menu de contexto) usam a **SF Pro Text se ela estiver instalada no PC**; sem ela, a Inter.
- A SF Pro **não vai junto com o projeto** (licença da Apple). Quem quiser o visual idêntico instala a fonte por conta própria.
- Pesos: com a SF Pro, texto em Regular (igual ao Mac na comparação). Com a Inter, Medium, para compensar o traço mais fino do Windows.

### Outros
- Faltam do Sequoia: AirDrop e Rede na barra lateral.
- Documentos sem miniatura são desenhados pelo app (folha com zíper, linhas, nota, extensão); programas (.exe, .msi, atalhos) usam o ícone do próprio Windows, como os apps no Mac.
- "Aplicativos" abre a pasta de atalhos do menu Iniciar de todos os usuários (os atalhos só do seu usuário ficam em outra pasta).
- Nomes de pastas do sistema aparecem como estão no disco ("Documents", "Music"), sem a tradução do desktop.ini.
- Só as pastas especiais conhecidas ganham símbolo gravado.
- Busca usa o índice do Windows Search: "Este Computador" só acha o que está indexado (pastas do usuário, por padrão). Pasta fora do índice cai numa varredura pelo nome (mais lenta). Limite de 3000 resultados. Resultados não se atualizam sozinhos.
- Na etiqueta aberta, a busca só filtra os itens dela.
- Quick Look toca áudio e vídeo pelo Media Foundation (MP4, MOV, MP3, AAC, WAV, FLAC...). MKV/WebM/AV1/HEVC dependem das extensões de vídeo instaladas no Windows; sem codec, mostra o ícone. Sem controle de volume (só mudo) e sem tela cheia.
- Enter renomeia (como no Mac). Para abrir: duplo clique, Ctrl+O ou Ctrl+↓.
- Imagem do arraste mostra só o primeiro item da seleção.
- Barra de rolagem é só indicador (não dá para arrastar).
- Nomes longos são cortados no fim com "…" (o Mac corta no meio).
- Padrão (`--padrao on`): pastas, discos, Win+E e Este Computador abrem no Arquivos. Ainda no Explorador: Lixeira, Painel de Controle, "Mostrar na pasta" de outros apps (navegadores etc. chamam o Explorador direto), Área de Trabalho e barra de tarefas (são o próprio explorer.exe; trocar exige registrar um shell próprio, fase futura).

## Ajustes do Sistema (bin\Ajustes.exe)
- Abre com Win+I, pelo menu Apple, pela Central de Controle e pelo ícone de Ajustes fixado no Dock. Links `ms-settings:` dos nossos menus vão para a página equivalente.
- Muda de verdade: Wi‑Fi (ligar, conectar com senha, desconectar), Bluetooth (ligar, lista de emparelhados), modo de energia e tempos de tela/repouso, brilho (só monitores com DDC/CI), resolução e taxa de atualização, imagem de fundo e ajuste, som (volume, mudo, dispositivo padrão de saída/entrada), tema claro/escuro/automático (7h–19h, o Shell troca), barras de rolagem, protetor de tela, itens de início (HKCU), teclado, mouse, encaixe de janelas, impressora padrão, notificações e tudo do Dock e da barra de menus.
- O resto (acessibilidade, privacidade, contas, atualizações, Windows Hello...) abre a página do Windows correspondente. Apple Intelligence, iCloud e Game Center mostram os equivalentes (Copilot/Fala, OneDrive, Jogos); Carteira e Apple Pay não existe.
- Ícones: SF Symbols reais (SVGs da fonte SF Pro em `icones\simbolos`, uso pessoal). Bluetooth não existe nos SF Symbols e usa o glifo do Windows.
- Tamanho da janela fixo em 715 pt de largura como no Mac (aumenta só se arrastar). Sem animação de troca de página.
- A grade de Imagem de Fundo lista a pasta do fundo atual, as subpastas de Imagens com "wallpaper", "papel", "fundo" ou "background" no nome e as imagens do Windows.

## Mission Control (Win+Tab)
- Substitui a Visão de Tarefas do Windows. Mostra as janelas da área de trabalho virtual atual (miniaturas vivas do DWM), em linhas como no Mac; clique leva a janela para a frente, clique no vazio/Esc/clique direito fecha.
- Mesas = áreas de trabalho virtuais do Windows (nomes do registro). Clicar troca (Win+Ctrl+setas) e "+" cria uma nova (Win+Ctrl+D); o Mac fica no Mission Control depois de criar, aqui ele fecha. Arrastar janela para outra mesa ainda não existe.
- Apps feitos com Chromium (Chrome, Discord, Claude, VS Code...) param de desenhar quando estão totalmente cobertos por outra janela; se já estavam cobertos antes do Mission Control abrir, a miniatura fica preta. Abertos pelo Dock ou pelo Launchpad, eles recebem `--disable-features=CalculateNativeWinOcclusion` (detectados pelo nome do navegador ou pelos arquivos do Electron; Discord pelo `--process-start-args` do Update.exe; apps empacotados pelo ativador do Windows). Nenhum atalho é alterado; desligar com `chromium_sem_oclusao=0` em ajustes.txt. Abertos por outro caminho (início automático, link de outro app com o navegador fechado), continuam sem a opção.
- Sem Ctrl+↑ nem F3 (no Windows esses atalhos já são de outros programas). Só a tela principal.

## Central de Controle (ícone da barra de menus)
- Wi‑Fi e Bluetooth: o círculo liga/desliga o rádio (Windows.Devices.Radios); o nome abre a página no Ajustes. Sem a lista de redes dentro do painel (o Mac mostra).
- AirDrop = Compartilhamento por Proximidade do Windows: mostra o estado e abre os ajustes do Windows ao clicar.
- Foco = silenciar as notificações (o mesmo "Permitir notificações" do Ajustes), não o Não Incomodar com horários do Windows 11.
- No lugar de Stage Manager e Espelhamento: Mission Control e "Espelhar Tela" (Win+K, Transmitir).
- Brilho só em monitores com DDC/CI (senão o controle fica cheio e parado); volume e a saída de som (botão à direita) funcionam; rolar o mouse em cima do painel muda o volume.
- Tocando agora usa o controle de mídia do Windows (capa, tocar/pausar, próxima); só aparece o que os apps informam ao Windows.
- Abre também pela mensagem registrada "ShellAbrirCentral" (para atalhos).

## Barra de título do Mac nos outros programas (MacJanela.dll)
- Só em janelas com a barra de título padrão do Windows (Ableton, programas Win32/Qt/Java comuns). Programas que desenham a própria barra (Chrome, Discord, Claude, VS Code, Office, apps da loja, FL Studio) ficam como estão.
- O Shell põe um gancho só na thread de cada janela elegível (nunca global): jogos, launchers e anti-cheat (lista em `src\shell\janelas.cpp` + `%APPDATA%\Shell\janelas-excluidas.txt`) nunca recebem a DLL. Programas abertos como administrador também ficam de fora (o Windows não deixa).
- Sem os botões do Windows, o Windows maximizaria por cima da barra de menus e do Dock e ignoraria Win+↑/↓: a DLL corrige o tamanho maximizado (área de trabalho) e o Shell trata Win+↑ (maximizar) e Win+↓ (restaurar/minimizar) nessas janelas.
- Barra com a cor do próprio app (lida do topo da área dele), cantos arredondados forçados e a sombra grande do Mac (dentro do programa, sem atraso). As demais janelas (Chrome, Discord...) recebem a mesma sombra pelo Shell (`sombras_mac=0` desliga), que pode atrasar um pouco ao arrastar.
- Na janela: título, ícone e botões do Windows somem; a barra fica com a cor da barra do Mac; por cima, os três botões (fechar/minimizar/maximizar, com os símbolos ao passar o mouse) e o título centralizado. A barra de menus da janela vai para a barra de menus do Shell. Duplo clique na barra maximiza. Clique direito na barra não abre mais o menu do sistema.
- A barra é a do Windows com a cor trocada: a altura é a do Windows (~31 px), não a de 28 pt do Mac, e não fica "unificada" com a barra de ferramentas do app como em apps do Mac.
- Ao sair do Shell, tudo volta ao visual do Windows. A DLL fica carregada no programa até ele fechar (para não travar se o Shell cair); recompilar a DLL com esses programas abertos exige renomear o arquivo antigo.
- Desligar: `janelas_mac=0` em ajustes.txt.

## Spotlight (Win+Espaço ou a lupa da barra de menus)
- Campo de vidro no alto da tela; digitando, aparecem Melhor Resultado, Aplicativos, Ajustes do Sistema, Documentos, Pastas, Imagens, Músicas e Filmes, Outros e "Pesquisar na Web" (Google), com a prévia à direita. Contas simples (+ − × ÷ ^ %, parênteses) viram o Melhor Resultado; Enter copia o valor.
- ↑/↓ escolhem, Enter abre, Ctrl+Enter mostra o arquivo no Arquivos, Esc apaga e depois fecha, Ctrl+A/Ctrl+V/Ctrl+Backspace no campo. Reabre com a última busca selecionada, como no Mac.
- Segurando o Win, cada Espaço abre/fecha de novo (nenhum espaço vai para o app de baixo).
- Win+Espaço é a troca de idioma do teclado no Windows; com o Shell aberto vira o Spotlight (a troca continua no Alt+Shift e no ícone da bandeja).
- Arquivos: índice próprio (`src\shell\indice.cpp`), só pelo nome. Cobre a pasta do usuário (sem a AppData) e todos os discos fixos, inclusive o externo (sem Windows, Arquivos de Programas, ProgramData, Users, `node_modules`, pastas começando com ponto). Disco ligado ou desligado com o Shell aberto refaz o índice na hora; o log mostra quantos itens cada disco tem. Fica em memória e em `%APPDATA%\Shell\indice.db` (~70 bytes por item; ~66 MB com 900 mil itens). Ao abrir o Shell, o arquivo salvo é lido e uma varredura nova roda em segundo plano (segundos); mudanças nas pastas refazem o índice depois de 10 s de calma, no máximo a cada 2 minutos. Enquanto o índice não fica pronto na primeira vez, a busca usa o Windows Search.
- Sem busca no conteúdo dos arquivos, sem filtros ("tipo:pdf") e sem definições de dicionário.
- Campo sem cursor móvel (o texto é editado no fim) e sem IME.

## Diálogos e alertas no visual do Mac
- Diálogos dos programas que recebem a barra do Mac (Propriedades do Mouse, Som etc.): fundo e texto do Mac (claro/escuro), fonte SF Pro (encolhe até 85% quando não cabe), botões do Mac com o padrão azul, caixas de seleção e rádios azuis, abas como controle segmentado sobre uma caixa, grupos como caixas arredondadas, menus suspensos com as setinhas no quadrado azul, campos com borda arredondada, listas no tema escuro e controles deslizantes com trilho azul e bolinha branca (`src\janelas\controles.cpp`). Desliga com `HKCU\Software\ShellProprio\MacJanela\Controles = 0`.
- Ficam como no Windows: imagens dos próprios diálogos (fundo branco), controles desenhados pelo programa, barras de progresso, a lista aberta dos menus suspensos e as partes "DirectUI" de diálogos novos.
- Caixas de mensagem (erros, confirmações) desses programas viram o alerta do Mac: sem barra de título (arrasta pelo fundo), ícone do app (ou o triângulo amarelo com o app no canto, em avisos e erros), título em negrito, texto informativo e botões na largura toda, o padrão azul à direita.
- Confirmações de excluir do Explorer (e de qualquer programa: "Delete File", "Delete Multiple Items"...): o Shell lê a pergunta pela UI Automation, tira a janela do Windows da tela e mostra o alerta do Mac com a Lixeira, em português ("Tem certeza de que deseja apagar “x” permanentemente? Você não pode desfazer esta ação."). Cancelar/Esc e Apagar/Enter respondem na janela original (`src\shell\alertas.cpp`).
- Senha do administrador (UAC) e telas de login ficam no visual do Windows: rodam numa área de trabalho protegida, onde nenhum programa pode desenhar.

## Sons do macOS
- Os sons vêm do repositório github.com/ThisIsNoahEvans/BigSurSounds (tirados do macOS; os de sistema e os 14 alertas clássicos são os mesmos no Sequoia), convertidos para WAV em `sons\mac` (originais em `sons\mac\original`). Uso pessoal: a pasta fica fora do git.
- Esquema de sons "macOS Sequoia" no registro (`HKCU\AppEvents\Schemes`, chave `macsequoia`, já ativo): alerta padrão/informação/pergunta = Tink, aviso = Funk, erro = Basso, dispositivo conectado/removido = Volume_Mount/Volume_Unmount, e-mail = new-mail, notificação e lembrete = Glass, mensagens = ReceivedMessage, bateria fraca = Sosumi, esvaziar a Lixeira = empty_trash. Entrar, sair e desbloquear o Windows ficam em silêncio, como no Mac.
- O Shell toca o "puf" ao arrastar um ícone para fora do Dock e o estalo (Pop) ao soltar o volume na Central de Controle.
- Faltam: os alertas novos do Sonoma/Sequoia (Boop, Breeze, Bubble...), que não foram achados para baixar, o som de inicialização e os toques de alarme/chamada (continuam os do Windows).
- Desfazer: `reg import "%APPDATA%\Shell\sons-windows-backup.reg"` (cópia de `HKCU\AppEvents` de antes da troca) ou escolher o esquema "Padrão do Windows" no Painel de Som.

## Tecla Windows
- Sozinha abre o Launchpad; com outra tecla vai para o Windows (Win+E, Win+D...), exceto Win+I (nosso Ajustes) e Win+Tab (Mission Control). Com Ctrl/Shift/Alt já seguros vai direto para o Windows; segurada mais de 1 s sem outra tecla não abre nada.

## Fora do alcance (decisão do briefing)
- Diálogos de abrir/salvar de outros apps, UAC, tela de login: ficam com o visual do Windows.

## Clique no vazio mostra a mesa, sugestões do Google e Mesas (Mission Control)

- **Clique no vazio da área de trabalho** (inclusive ao lado do Dock) empurra as janelas para as bordas,
  como no macOS Sonoma/Sequoia; outro clique no vazio ou ativar uma janela traz todas de volta.
  Janelas maximizadas são minimizadas e voltam maximizadas. Desligar: `clique_mostra_mesa=0` em
  `%APPDATA%\Shell\ajustes.txt`. Ao sair do Shell, as janelas voltam ao lugar na hora.
- **Spotlight com sugestões do Google**: o texto digitado vai para `suggestqueries.google.com` (as
  sugestões aparecem na seção "Sugestões do Google"). Desligar: `spotlight_google=0`.
- **Mesas**: o X (passar o mouse na miniatura com a barra aberta) fecha a Mesa; as janelas dela vão
  para a vizinha. Fechar/criar/trocar usam a interface interna do Explorer (Windows 11 24H2/25H2),
  conferida contra o registro antes de cada uso; se não bater, o Shell usa Win+Ctrl+setas/D/F4.

## Monitor de Atividade (MonitorAtividade.exe)

- O gerenciador de tarefas no visual do Mac: abas CPU, Memória, Energia, Disco, Rede e GPU, tabela ordenável,
  busca, Encerrar / Forçar Encerramento (X), Informações (i) e o menu ⋯. Compilar: `build.cmd monitor`.
- **Ctrl+Shift+Esc** abre o Monitor no lugar do Gerenciador de Tarefas enquanto o Shell roda (o gancho de
  teclado do Shell); sem o Shell, volta a ser o do Windows. "Forçar Encerrar…" do menu da Apple também.
- **Atalho no Menu Iniciar** (para o Launchpad e o Spotlight acharem): 
  `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Monitor de Atividade.lnk`. Para desfazer, apague o atalho.
- Processos do sistema e de outros usuários só podem ser encerrados com o Monitor aberto como administrador.
- A aba Rede mostra as conexões abertas por processo (o Windows não dá os bytes por processo sem ETW);
  os totais de baixo (pacotes e dados) são das placas de rede de verdade.
- **GPU**: % e tempo de GPU, memória da GPU e o motor em uso (3D, vídeo, cópia, computação) por processo, dos
  mesmos contadores do Gerenciador de Tarefas ("GPU Engine", "GPU Process Memory"); o nome e a memória total
  da placa vêm do DXGI. "Trocas de Contexto" (aba CPU) é a soma das threads por segundo — o equivalente
  mais próximo no Windows das "Ativações Ociosas" do Mac.

## Terminal (Terminal.exe)

- O Terminal do Mac (perfil "Básico" escuro) rodando o PowerShell (`Terminal.exe`) ou o Prompt de Comando
  (`Terminal.exe --cmd`) num pseudoconsole (ConPTY, o mesmo do Windows Terminal). Compilar: `build.cmd terminal`.
- O título mostra "Pasta — shell — colunas×linhas". A pasta vem de uma marca invisível no prompt (OSC 9;9),
  posta só nas janelas deste Terminal: no PowerShell, um `prompt` que embrulha o seu (o perfil continua valendo);
  no cmd, a variável PROMPT só deste processo. Nada é gravado no seu perfil nem no registro.
- Atalhos: Ctrl+C copia se houver seleção (senão interrompe); Ctrl+V cola; Ctrl+Shift+C/V/A; Ctrl+K limpa a
  rolagem; Ctrl+Shift+N nova janela; Shift+PgUp/PgDn e a roda do mouse rolam o histórico (10.000 linhas).
- Atalhos no Menu Iniciar (Launchpad/Spotlight): `Terminal.lnk` e `Terminal - Prompt de Comando.lnk` em
  `%APPDATA%\Microsoft\Windows\Start Menu\Programs`. Para desfazer, apague-os.
- O cmd/PowerShell abertos pelo Executar (Win+R) ou por outros programas continuam no console do Windows:
  trocar o "terminal padrão" do Windows exige uma integração COM que o Terminal ainda não tem.
- **Fonte SF Mono** (fornecida pelo usuário, uso pessoal): instalada só para o usuário em
  `%LOCALAPPDATA%\Microsoft\Windows\Fonts\SFMono*.otf`, registrada em
  `HKCU\Software\Microsoft\Windows NT\CurrentVersion\Fonts` ("SF Mono ... (OpenType)"). Para desfazer, apague
  os valores "SF Mono*" dessa chave e os arquivos.

## Notas (Notas.exe)

- O Notas do Sequoia: barra lateral translúcida (Notas Matemáticas, iCloud, No Meu Mac, Nova Pasta), lista
  agrupada por data (Fixadas, Hoje, Ontem, 7/30 Dias Anteriores, meses) ou galeria, e o editor. Compilar: `build.cmd notas`.
- **Dados**: `%APPDATA%\Shell\Notas\` — um `<id>.nota` por nota (texto UTF-8), `pastas.txt` e `estado.txt`
  (janela, larguras, pasta e nota abertas). Grava sozinho 0,6 s depois de cada mudança e ao fechar. Não há iCloud
  de verdade: "iCloud" e "No Meu Mac" são só duas seções locais.
- Texto rico: Título/Cabeçalho/Subcabeçalho/Corpo/Monoespaçado (Ctrl+Shift+T/H/J/B/M), negrito/itálico/sublinhado
  (Ctrl+B/I/U), tachado (Ctrl+Shift+X), listas com marcadores/traços/números (Ctrl+Shift+7/8/9, ou digitar
  "* ", "- ", "1. "), lista de verificação (Ctrl+Shift+L, marcar com Ctrl+Shift+U), recuo (Tab/Shift+Tab,
  Ctrl+]/[), realces roxo/rosa/laranja/menta/azul (painel Aa), seções recolhíveis (seta à esquerda dos títulos).
- Notas Matemáticas em todas as notas: `nome = expressão` cria a variável (destacada em amarelo); linha que
  termina em `=` mostra o resultado (+ − × ÷ ^ %, parênteses, √, sqrt/sen/cos/tan/ln/log). Números em pt-BR
  ("1.500", "12,50").
- Apagar manda para Apagadas Recentemente (somem depois de 30 dias); lá dentro, recuperar ou apagar de vez
  (com confirmação). Arrastar uma nota para uma pasta da barra lateral a move. Notas vazias somem ao sair delas.
- Busca (lupa ou Ctrl+F) em todas as notas, com os trechos destacados. Ctrl+N nova nota, Ctrl+Shift+N nova pasta,
  Ctrl+1/2 lista/galeria, Ctrl+Alt+S barra lateral, Ctrl+Shift+P fixar. Botão direito: menus da nota/pasta
  (fixar, mover, duplicar, renomear, ordenar por edição/criação/título, agrupar por data).
- Ainda não fazem nada (mostram um menu dizendo isso): tabela, gravar áudio, fotos, colaborar e bloquear.
- **Nota Rápida** (canto ativo, como no Mac): encostar o ponteiro no canto inferior direito da tela faz um quadradinho
  com uma nota sair do canto; clicar abre `Notas.exe --nota-rapida`, uma janelinha flutuante (sempre por cima) com uma
  nota nova na pasta "Notas Rápidas". Vazia, some ao fechar. O canto fica no Shell (`src/shell/notarapida.cpp`); não
  dispara com botão do mouse apertado nem com app em tela cheia na frente. Desligar: `canto_nota_rapida=0` em
  `%APPDATA%\Shell\ajustes.txt`. O Notas aberto traz as notas rápidas novas quando volta a ficar ativo.
- **Atalho no Menu Iniciar** (Launchpad/Spotlight): `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Notas.lnk`,
  com o ícone `bin\Notas.ico` (feito do Notes.png do pacote do Sequoia, uso pessoal). Para desfazer, apague os dois.
- Testes sem mexer nos seus dados: `Notas.exe --dados=<pasta>`; `--captura=arquivo.png [--claro|--escuro]
  [--tamanho=1100x680] [--pasta=id] [--nota=n] [--galeria] [--aa] [--busca=texto]` desenha um quadro num PNG.

## Efeito gênio (minimizar)

- Minimizar uma janela faz o gênio do Mac até o lugar dela no Dock; clicar na miniatura no Dock faz o caminho
  de volta. É feito com faixas horizontais da miniatura viva da janela (DWM), uma a cada ~4 px.
- Para não ter duas animações, a animação de minimizar do Windows é desligada **só enquanto o Shell roda**
  (`SPI_SETANIMATION` sem gravar no perfil: nada vai para o registro). Ao sair do Shell ela volta como estava;
  se o Shell fechar de forma inesperada, ela volta no próximo login. Desligar o gênio: `genio=0` em
  `%APPDATA%\Shell\ajustes.txt`.

## Tela de inicialização (src/shell/boot.cpp)

- Tela preta com a maçã e a barrinha de progresso do Mac (sem o texto "About N minutes remaining", sem som),
  em todos os monitores (maçã e barra só no principal). Medidas tiradas de um vídeo de boot do Sonoma e da
  foto de um MacBook: maçã ~96 pt, barra 220 × 6 pt com cantos redondos, trilho cinza-escuro, branco por cima.
- **Não é o boot do Windows**: o logo do firmware e as bolinhas girando continuam (trocá-los exigiria mexer em
  arquivos de boot). A tela aparece quando o Shell abre **até 2 min depois do login** (conta do início do
  `sihost.exe` da sessão); reabrir o Shell à mão não mostra nada.
- Progresso real: partes do Shell criadas → Explorador pronto → apps de inicialização (espera a CPU ficar
  abaixo de 35% por 1,5 s, no máximo 12 s). Mínimo 3 s, máximo 25 s. No fim, maçã e barra somem, a tela
  fica preta um instante e esmaece revelando a mesa. Clique encerra na hora. O ponteiro some por cima dela.
- **Desligada por padrão** (desde 09/11/2026): o usuário quer a maçã só no boot do Windows (HackBGRT, abaixo),
  não no login nem depois. Ligar: `boot=1` em `%APPDATA%\Shell\ajustes.txt` (só logo depois do login) ou
  `boot=sempre` (até quando o Shell é reaberto). Um `boot=0` gravado pelo terminal do Claude caiu na cópia
  virtualizada do `%APPDATA%` e o Shell não viu; por isso virou padrão no código.
- **Prévia** sem reiniciar: `bin\SequoiaShell.exe --testar-boot` (só a tela, ~6 s; não mexe no Shell que está rodando).
- **Maçã no boot do Windows (aplicado em 07/11/2026, a pedido do usuário)**: HackBGRT 2.6.0
  (github.com/Metabolix/HackBGRT) troca o logo do firmware (BGRT) pela maçã branca centralizada no preto, com a barra de progresso
  embaixo (imagem `maca.bmp` 231×314: maçã 84×100 no centro, barra 229×6 a 151 px abaixo, ~12% cheia). A barra
  é **parada**: a imagem do boot do Windows é estática e nada além das bolinhas pode animar ali. O firmware não
  informa a resolução (GOP), então o HackBGRT calcula para 1024×768 e o Windows centraliza.
  Instalado com `setup.exe batch install enable-bcdedit`: entrada de boot "HackBGRT" na frente do Windows Boot
  Manager, arquivos em `\EFI\HackBGRT` da partição EFI; o carregador do Windows não foi sobrescrito.
  Secure Boot e BitLocker estavam desligados. Cópia do instalador e da imagem: `%APPDATA%\Shell\HackBGRT`.
  - As bolinhas girando foram escondidas com `bcdedit /set {globalsettings} custom:16000069 true`
    (aparece como `nobootuxprogress`). O `bootuxdisabled` usado antes foi retirado, porque esconderia a maçã.
  - O logo da ASUS ainda pisca um instante antes (vem do firmware). Atualização do Windows pode restaurar o
    carregador: é só reinstalar. Pode afetar PIN (TPM) e anti-cheat (Vanguard/EAC); se o PIN falhar, entrar com a
    senha e "Esqueci meu PIN".
  - **Desfazer** (Prompt como administrador): `%APPDATA%\Shell\HackBGRT\setup.exe batch uninstall` e
    `bcdedit /deletevalue {globalsettings} custom:16000069`. Se o PC não ligar, escolha "Windows Boot Manager" no
    menu de boot da placa (F8 na ASUS) e desinstale.
  - Cópia da configuração de boot antes de tudo: `%APPDATA%\Shell\bcd-backup-antes-bootux`.

## Tela de bloqueio (src/shell/bloqueio.cpp)

- Win+L, "Bloquear Tela" no menu da Apple e a tela apagando/suspensão mostram a tela de bloqueio do Sequoia
  (medida no vídeo de referência): papel de parede da mesa em todos os monitores, data e relógio grandes com
  vibração (o texto é pintado com o próprio papel de parede desfocado e clareado), foto e nome da conta do
  Windows, campo "Digite a Senha" que chacoalha na senha errada, fonte de entrada e botão de energia
  (Suspender / Reiniciar… / Desligar…) no canto. Enter com o campo vazio usa o Windows Hello (PIN/rosto/digital).
- **Senha própria da tela** (opcional): `Shell.exe --senha-bloqueio` define uma senha só desta tela (a da conta
  do Windows não muda). Só o resumo PBKDF2-SHA256 com sal vai para `%APPDATA%\Shellloqueio-senha.txt`;
  apagar esse arquivo (ou "Usar a do Windows" na mesma janela) volta para a senha da conta.
- Atalhos: Win+L (com a política abaixo) e Ctrl+Win+Q (o Control+Command+Q do Mac, sempre funciona).
- Sem senha própria, a senha é conferida com `LogonUserW` (logon interativo, conta local ou Microsoft) numa thread; nunca é
  gravada e a cópia é zerada. Erros que não são "senha errada" (ex.: senha expirada, tipo de logon negado)
  **aceitam** e ficam no `bin\shell.log`, para nunca trancar o dono para fora. Depois de 3 erros a tela espera
  1 min (4: 5 min; 5+: 15 min), para não disparar o bloqueio de conta do Windows (10 tentativas).
- **Saídas de emergência** (nenhuma deixa a sessão aberta):
  - Ctrl+Alt+Del → Bloquear → desbloqueie com o PIN/senha do Windows: ao voltar, a nossa tela some sozinha.
  - Ctrl+Alt+Shift+F12 (sair do Shell) ou o Shell cair com a tela travada: o Windows é bloqueado de verdade
    (`LockWorkStation`). Um vigia em outro processo (`Shell.exe --vigia <pid>`) faz o mesmo se o Shell for
    encerrado à força (ex.: Gerenciador de Tarefas pelo Ctrl+Alt+Del).
- Com a tela travada o gancho do teclado barra Win, Alt+Tab, Alt+Esc, Alt+F4, Ctrl+Esc e Ctrl+Shift+Esc.
  Ctrl+Alt+Del não dá para barrar no modo usuário (é a saída acima). Nada de políticas, registro ou da tela de
  bloqueio real do Windows é alterado.
- **Win+L**: o Windows trata o atalho antes de qualquer gancho de teclado, então ele só chega ao Shell com a
  política `DisableLockWorkstation=1` em `HKCU\Software\Microsoft\Windows\CurrentVersion\Policies\System`.
  O Shell tenta ligá-la ao abrir e apagá-la ao sair (também pelo vigia), mas essa chave só aceita escrita de
  administrador: numa conta padrão a escrita falha (`bloqueio: ... ERRO ao ligar` no shell.log) e o Win+L
  continua com a tela do Windows. Neste PC ela foi ligada pelo administrador em 07/11/2026 (pedido do usuário): fica permanente (o Shell não consegue
  apagar): sem o Shell o Win+L não faz nada, o "Bloquear" some do Ctrl+Alt+Del, e o bloqueio de segurança passa
  a desconectar a sessão (`WTSDisconnectSession`: tela de login do Windows, volta com o PIN). Desfazer (admin):
  `reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Policies\System" /v DisableLockWorkstation /f`.
  `bloqueio_winl=0` faz o Shell nem tentar. Precisa do gancho da tecla Windows (`tecla_windows_launchpad=1`).
- Ajustes em `%APPDATA%\Shell\ajustes.txt`:
  - `bloqueio=0` desliga: Win+L e "Bloquear Tela" voltam a usar o bloqueio do Windows.
  - `bloqueio_ao_apagar=0`: não trava quando a tela apaga ou o PC suspende (padrão: trava, como o Mac).
    Se o Windows também pedir login ao acordar, desbloquear lá já tira a nossa.
  - `bloqueio_desfoque=1`: papel de parede desfocado (o vídeo do Sequoia mostra nítido, que é o padrão).
- Testes sem travar nada: `Shell.exe --testar-bloqueio` (a tela por ~3,5 s, chacoalha sozinha, não confere
  senha) e `Shell.exe --bloqueio-png saida.png 1920 1080 <fase 1-3> <ms> [chacoalho_ms] [n_bolinhas]` (imagem).
- Limites: a foto vem de `HKLM\...\AccountPicture\Users\<SID>` (sem foto: monograma); sem Wi-Fi no canto
  (o vídeo só mostra fonte de entrada e energia; bateria aparece em notebooks). A janela do Windows Hello é do
  sistema, não no visual do Mac. Se a sua conta só entra por PIN e a senha da conta Microsoft nunca foi digitada
  neste PC, o `LogonUserW` pode recusar a senha: use o Windows Hello (Enter) ou a saída do Ctrl+Alt+Del.

## Inicialização automática do Shell

O Shell inicia sozinho ao entrar no Windows pela entrada `ShellProprio` em `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` (valor: caminho do `bin\Shell.exe`), criada em 07/11/2026.
Para desfazer: `Remove-ItemProperty HKCU:\Software\Microsoft\Windows\CurrentVersion\Run -Name ShellProprio`.


### Tela de bloqueio do Windows com cara de Sequoia (mod do Windhawk)

- Pasta `docs/windhawk/bloqueio-sequoia/`: mod local `sequoia-lockscreen` do Windhawk para o `LockApp.exe` (a
  tela do relógio do Windows, que roda na conta do usuário). É o Windows 11 Settings Styler (m417z, GPLv3) com o
  alvo trocado para o LockApp e sem o contador de estatísticas (nenhum acesso à rede). O campo de senha/PIN
  (`LogonUI.exe`, do sistema) continua o do Windows.
- Estilos (`estilos.py` → `sequoia-lockscreen.reg`): relógio e data em SF Pro Display Semibold (133/28), data
  acima do relógio, widgets/clima/"Saiba mais"/busca/ícones de status escondidos, sem o escurecimento do fundo.
  Os nomes dos elementos (`TimeText`, `DateText`, `TimeAndDatePanel`...) foram lidos do `MainPage.xbf` dentro do
  `resources.pri` do LockApp; o tipo de alguns é chutado (uma regra por tipo provável). Posições finas
  (o quanto a data sobe/o relógio desce) precisam de um teste com a tela travada e ajuste.
- Fundo: a imagem da tela de bloqueio do Windows foi trocada (API `LockScreen.SetImageFileAsync`, a mesma das
  Configurações) para `icones/bloqueio/sequoia-floresta.jpg`. Antes era `C:\Windows\Web\Screen\img100.jpg`
  (com o Windows em Destaque ligado). Voltar: Configurações > Personalização > Tela de bloqueio.
- Instalar/atualizar (administrador): `python estilos.py <nome>.dll` (sem admin) e depois
  `instalar.ps1` como administrador. Desligar: `desligar.ps1` como administrador, ou desligar o mod na janela do
  Windhawk. Recompilar a dll: `compilar.cmd` (clang++ do próprio Windhawk, alguns minutos).
