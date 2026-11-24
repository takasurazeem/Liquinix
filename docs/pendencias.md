# Pendências (anotado em 2026-11-11)

Da lista de ideias ("o que mais dá pra fazer"). Feitos: 1 (menu do botão verde), 2 (Pilhas no Dock), 3 (Exposé do app: Ctrl+Win+↓), 5 (Win+Q/H/W), 6 (painel Apps do Tahoe: tremer, arrastar, ×, Trazer de Volta) e, fora da lista, Cantos Ativos e o botão direito/"Mostrar Mais" no Spotlight e Launchpad.

## Coisas do Mac que ainda faltam
4. ~~Pilhas na mesa~~ feito em 2026-11-13 (botão direito na mesa > Usar Pilhas).
- Feitos em 2026-11-13: Quick Look com espaço (mesa, Downloads, Ctrl+Y no Spotlight), Organizador Visual (Stage Manager), app Pré-Visualização. A conferir na tela: Quick Look, faixa do Organizador, abrir/fechar pilha.
- Feitos em 2026-11-13 (tarde): Lembretes, Calendário, TextEdit, cor/símbolo nas pastas, Organizador no Tahoe, estilo de ícones no Launchpad/Spotlight, Ctrl+1..4 no Spotlight, menus do Spotify/VS Code, avisos de lembretes/eventos. A conferir com clique: criar lembrete/evento, Personalizar Pasta, Ctrl+1..4.

## Acabamento
7. **Spotlight**: medir se os quadros lentos ao digitar sumiram (precisa de umas buscas digitadas).
8. ~~Animação de abrir nos apps de 32 bits~~ feito em 2026-11-13 (falta conferir na tela).

## Memória como no Mac (2026-11-14)
- Feito: pressão de memória, Soneca de Apps (App Nap), memória descartável, Forçar Encerrar (Ctrl+Alt+Esc) e coluna App Nap no Monitor (detalhes no registro-mudancas).
- A conferir: a limpeza na pressão alta e a janela automática na pressão crítica só acontecem com a memória quase no fim (não testado de verdade); Ctrl+Alt+Esc e o menu da Apple abrindo o Forçar Encerrar; a coluna App Nap.

## Manutenção
9. **Instalador**: gerar o zip de novo (o atual é de 2026-11-10 16:50).
10. ~~Limpar %LOCALAPPDATA%\SequoiaBinariosAntigos~~ feito em 2026-11-13 com autorização (656 apagados, 653 MB; 2 em uso ficaram). Zip refeito às 22:47.

## Outros em aberto
- Ícones: .md e tipos sem programa com a folha do Mac na mesa e nas pilhas (2026-11-13); no Explorador continuam com o ícone genérico do Windows; .txt, imagens, músicas e vídeos ficam com o ícone do app da Store (só mudaria trocando o app padrão).
- Ícones: o desfazer da segunda parte (tools\icones-sistema-extra.ps1 -Desfazer) ainda não foi testado.
- Idiomas: conferir cada app em inglês na tela.
- Janela de copiar/excluir no estilo do Mac: feita em outra sessão (resumo em notas/progresso-copia.md); falta conferir na tela.
- ~~Downloads no Dock: opção "Exibir como Pasta / Pilha"~~ já existia (pilhas.cpp).
- 2026-11-13: conferido no código que já existem a galeria do "Editar Controles", o relógio de vidro do bloqueio, os modos do Spotlight (Ctrl+1..4), a cor/símbolo das pastas e o estilo de ícones no Launchpad/Spotlight; a lista do 9to5Mac abaixo está em dia só no que segue em aberto.
- 2026-11-13: "Tema" do Ajustes ficava cinza com "Reduzir transparência" ligado; agora fica sempre livre.

## Refinamento pelo vídeo do 9to5Mac (macOS 26 Tahoe, walkthrough completo) — 2026-11-11
Feito: estilo de ícones e widgets (Padrão/Escuro/Transparente/Tingido) no Dock; cápsula de vidro no ícone da barra
do painel aberto (Central, Spotlight, relógio, menus); travamento ao digitar no Spotlight (vidro real).
Ainda diferente do vídeo:
- Central de Controle: "Editar Controles" com a galeria de controles (adicionar/remover/arrastar módulos).
- Tela de bloqueio: relógio enorme de vidro (números translúcidos que mostram o papel de parede), data pequena em cima.
- Cor das pastas (Automática + cores) e pastas com emoji/símbolo (Finder e mesa).
- Spotlight num modo (Área de Transferência etc.): nome do modo + atalho "⌘4" na cápsula, círculo do modo destacado; filtros como fichas no campo (Arquivos).
- Estilo dos ícones também no Launchpad/Apps, Spotlight e widgets (hoje só o Dock).
- ~~Tingir janelas com o papel de parede e cantos maiores~~ feitos (barra lateral dos apps novos; cantos de 16 pt no Tahoe).
- Live Activities do iPhone na barra de menus (não se aplica no Windows).
## Rebranding Tahoe (guia macOS 26) — 2026-11-15
Guia: `Documents\macOS26-Theme-Reference\...\LIQUID_GLASS_MACOS26.md` (medidas do macOS 26 real). O que foi aplicado
está no `registro-mudancas.txt` (2026-11-15 22:20). Conferido no código que já batiam com o guia: barra de menus
transparente com o nome do app em negrito, menus abrindo sem animação e sumindo em fade de 160 ms, ícones ao lado
dos itens de menu, cantos de 16 (só título) e 26 (com barra de ferramentas), cores do semáforo, coluna de 344 da
Central de Notificações, banner de 5 s, cor de destaque azul `#007AFF`, texto de 13 pt, Reduzir Transparência
deixando o vidro quase opaco, estilos de ícone do Dock (Padrão/Escuro/Transparente/Tingido), ⌘-Tab sem animação.

Itens que tinham ficado de fora: todos implementados na segunda leva (registro-mudancas.txt, 2026-11-15 22:55).
Diferenças conscientes: nenhuma (alerta centralizado, vibrancy na barra e a mola da luz feitos em 2026-11-15 23:08).
A conferir com o mouse (não simulei clique): módulos expandidos da Central, menu do botão verde, hover dos vidros,
apertar (escala 1,06), gota do segmentado do Monitor, Reduzir Movimento, Apps com 7 colunas, barra do Finder, alerta centralizado, sacudir o ponteiro,
Spotlight: arrastar um resultado para fora (copia) e "Mostrar no Finder" no botão direito (2026-11-16).
