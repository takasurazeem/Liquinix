# Versão nativa encerrada (2026-11-12): o que ficou de bom

O usuário desistiu da versão nativa (Shell dentro do explorer.exe via MacBarra.dll) em 2026-11-12: no PC dele o
ponteiro do mouse sumiu com ela instalada (causa não investigada). Ele desinstalou pelo Desfazer.cmd e usa só a
versão que não mexe no Windows (`bin\SequoiaShell.exe`). **Não retomar a versão nativa sem ele pedir.**

Foram apagados: `barra-nativa\`, `dist\SequoiaNativo-2026-11-12\`, `tools\vm-barra-dentro.ps1`,
`tools\vm-apps-dentro.ps1` e a VM de testes. Os fontes nativos (sem binários) estão em
`docs\nativo-fontes-2026-11-12.zip`. O histórico completo está em `docs\registro-mudancas.txt`.

## Mudanças que continuam no código e servem à versão normal

Estão em `src\`, não dependem do modo nativo. **Conferir se já estão compiladas no `bin\` atual** antes de dizer ao
usuário que estão valendo: nesta sessão elas só foram compiladas dentro da MacBarra.dll e testadas na VM.

- **Alertas do Mac** (`src\shell\alertas.cpp`): caixas de mensagem comuns de qualquer programa (MessageBox com 1, 2 ou
  3 botões) viram o alerta do Mac com o ícone do programa; a resposta volta pelo id do botão original. No tema Tahoe o
  alerta tem o desenho novo (ícone em cima à esquerda, texto alinhado à esquerda, botões em cápsula empilhados, raio
  26) e sombra própria. Ajuste `alertas_mac`.
- **Janela de progresso** (`src\shell\progresso.cpp`): ignora os botões Minimizar/Maximizar/Fechar da barra ao ler a
  janela de cópia (sem isso toda cópia parecia uma "pergunta").
- **Vidro real sem captura** (`src\shell\backdrop.cpp`): onde o Windows recusa a captura de janelas (`rg::semCaptura`),
  cai no vidro fosco com o raio e a cor da peça, em vez de ficar tudo preto.
- **Fundo da tela bloqueada nos Ajustes** (`src\ajustes\paginas.cpp`, `lockbg`): escolher imagem ou vídeo (valida H.264
  pelo Media Foundation); grava em `%APPDATA%\Shell\bloqueio\`. A parte que publica em `C:\Windows\Web\Screen\macOS`
  era para o AuthUX e pede administrador.
- **Ajustes abrindo ms-settings** (`src\ajustes\main.cpp`, `OpenUri`): usa ActivateForProtocol.
- **Restaurar janela de administrador pelo Dock** (`shell::Restaurar` em `src\shell\shell.h`, usado em dock.cpp,
  genio.cpp, apps.cpp, menubar.cpp): quando o ShowWindow é recusado, pede SC_RESTORE por mensagem.
- **Registro de tela cheia** (`src\shell\main.cpp`, `CheckFullscreen`): anota no log qual janela fez o Dock e a barra
  de menus sumirem.
- **Miniaturas do Finder** (`src\finder\thumbs.cpp`): threads soltas (detach) para não derrubar o programa hospedeiro
  ao sair.
- **Cantos por tema** (`src\janelas\macjanela.cpp`): 16 pt no Tahoe, 12 pt no Sequoia (`TemaTahoe`).
- **Fontes** (`tools\vm-windows-mac-dentro.ps1`): não substitui mais a Segoe UI Variable (derrubava Terminal e apps XAML).

## Só existe no modo nativo (desligado na versão normal)

Os trechos `shell::nativo` em `src\shell` (main, menus, mesa, pastas, dialogs, tray...) e o `src\shell\barras.cpp`
inteiro ficaram no código, inertes. `barras.cpp` ficou no meio de uma mudança nos símbolos das bolinhas (compila, não
foi testada). Ideias dele que valeria levar para a SequoiaWindow.dll, se o usuário pedir:

- bolinhas **na linha das abas** do Chrome/Edge/Electron, com as abas começando depois delas (o usuário gostou muito
  do resultado visual; a implementação por cópia deslocada trouxe os problemas de clique, arrasto e ponteiro);
- nunca mover o ponteiro do usuário por código: ele reclamou duas vezes.

## Lições

- O usuário testa no PC real; o que só existe lá (Chrome sem o botão "buscar abas", mouse de verdade) não aparece na VM.
- Perguntas de confirmação têm de aparecer por cima de tudo: um Read-Host num console de fundo passou despercebido.
