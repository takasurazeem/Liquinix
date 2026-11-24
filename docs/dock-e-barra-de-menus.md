# Dock e barra de menus (Sequoia) — especificação

Programa: `bin\Shell.exe` (`src\shell\`). Roda por cima do Explorador nesta fase: esconde a barra de
tarefas do Windows enquanto está aberto e devolve tudo ao fechar (menu do logo → "Sair do Shell").

Medidas em pontos (pt = px a 100%). Onde não há número oficial, vale o que se mede no sistema e
fica configurável em `%APPDATA%\Shell\ajustes.txt`.

## Barra de menus (topo)

| | Sequoia | Aqui |
|---|---|---|
| Altura | 24 pt (telas sem entalhe) | 24 pt |
| Fundo | translúcido, tingido pelo papel de parede | acrílico do Windows, tinta escura/clara conforme o tema |
| Fonte | SF Pro 13 pt; nome do app em negrito | SF Pro Text (se instalada) / Inter / Segoe UI, 13 pt |
| Destaque do item aberto | retângulo arredondado translúcido | idem (raio 4 pt) |

Esquerda → direita: **logo** (próprio, não o da Apple) · **nome do app ativo** (negrito) · **menus do app**.
Direita ← esquerda: **relógio** ("sáb. 7 de nov. 10:45") · **Central de Controle** · **Spotlight** ·
**ícones de status** (a bandeja dos apps, ver `estudo-bandeja.md`).

Menu do logo (equivalente ao menu Apple):

| Mac | Windows |
|---|---|
| Sobre Este Mac | Sobre Este Computador → `ms-settings:about` |
| Ajustes do Sistema… | `ms-settings:` |
| App Store… | Microsoft Store |
| Itens Recentes ▸ | últimos itens de `shell:Recent` |
| Forçar Encerrar… | Gerenciador de Tarefas |
| Repousar / Reiniciar… / Desligar… | suspender / reiniciar / desligar (com confirmação) |
| Bloquear Tela | `LockWorkStation` |
| Encerrar Sessão de *usuário*… | logoff (com confirmação) |

Menus do app ativo: se a janela tem menu Win32 (`GetMenu`), os **menus reais** do programa aparecem
na barra, no nosso visual, e o item escolhido vai para o programa (`WM_COMMAND`). Programas sem menu
(a maioria dos modernos) ganham o menu do app (Sobre, Ocultar, Ocultar Outros, Mostrar Tudo, Encerrar)
e o menu **Janela** (Minimizar, Zoom, Trazer Tudo para a Frente, lista de janelas), como no Mac.

## Dock

| | Sequoia | Aqui |
|---|---|---|
| Tamanho do ícone | 48 pt (padrão) | 48 pt |
| Ampliação | desligada por padrão; até 128 pt | idem (`ampliacao=1` liga) |
| Fundo | vidro translúcido arredondado, borda fina clara | acrílico + borda desenhada |
| Indicador de app aberto | ponto pequeno embaixo do ícone | ponto de 4 pt |
| Nome | etiqueta acima do ícone ao passar o mouse | idem |
| Abrir app | ícone pula até o app abrir | idem (para quando a janela aparece) |

Seções: **Arquivos** (o Finder, sempre o primeiro) · apps fixos · (separador) · apps abertos não fixos
e recentes · (separador) · **Downloads** · **Lixeira**.

- Clique: abre o app; se já está aberto, traz todas as janelas dele para frente (restaura as minimizadas).
- Botão direito (app aberto): lista de janelas · Opções ▸ (Manter no Dock, Abrir ao Iniciar Sessão,
  Mostrar no Finder) · Mostrar Todas as Janelas · Ocultar · Encerrar. App fechado: Opções ▸ · Abrir.
- Arrastar para reordenar; arrastar para fora e soltar remove.
- Downloads: clique mostra os itens mais recentes; "Abrir no Finder".
- Lixeira: ícone vazio/cheio; clique abre; botão direito → Esvaziar Lixeira.
- Primeira vez: os apps fixados na barra de tarefas do Windows viram os apps fixos do Dock.

## Exceções conhecidas (nesta fase)

- Janelas minimizadas voltam pelo ícone do app (opção "Minimizar janelas no ícone do aplicativo"
  do Mac); ainda sem miniaturas à direita do Dock nem efeito "gênio".
- Só o monitor principal.
- A Lixeira abre no Explorador até o Arquivos ganhar a visão de Lixeira.
- Wi-Fi/som/bateria da Central de Controle abrem as páginas do Windows; controles deslizantes depois.
