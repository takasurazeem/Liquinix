# Estudo: como funciona a bandeja (área de notificação)

Fontes lidas: ReactOS `base/shell/explorer/syspager.cpp` (GPL, só como referência) e
ManagedShell `src/ManagedShell.WindowsTray` (Apache-2.0, base do Cairo Desktop). Nenhum código foi copiado;
o protótipo (`src/tools/bandeja_teste.cpp`) é nosso.

## O protocolo

1. Um app chama `Shell_NotifyIcon(NIM_ADD, &nid)`. O `shell32` procura a janela de classe
   **`Shell_TrayWnd`** (`FindWindow`, a primeira na ordem Z) e manda um **`WM_COPYDATA`**:
   - `dwData = 1` → `SHELLTRAYDATA { DWORD dwSignature (0x34753423); DWORD dwMessage (NIM_*); NOTIFYICONDATA32 nid }`
   - `dwData = 0` → mensagens de AppBar (`SHAppBarMessage`: barras encaixadas, tela cheia...)
   - `dwData = 3` → `Shell_NotifyIconGetRect` (onde o ícone está na tela)
2. A `NOTIFYICONDATA` que chega tem layout **32 bits fixo** (HWND e HICON como DWORD), mesmo vindo
   de apps 64 bits. HICON e HWND são objetos globais: dá para usar o ícone no nosso processo
   (copiamos com `CopyIcon`, porque o app pode destruir o dele).
3. Mensagens: `NIM_ADD`, `NIM_MODIFY` (troca ícone/dica/estado), `NIM_DELETE`, `NIM_SETVERSION`
   (3 = antigo, 4 = moderno), `NIM_SETFOCUS`. Ícone identificado por (hWnd, uID) ou por `guidItem`.
4. Quando a bandeja nasce (ou renasce), ela manda **`TaskbarCreated`** (mensagem registrada) por
   broadcast. Todo app bem comportado responde refazendo seus `NIM_ADD`. É assim que a gente
   "pega" os ícones de quem já estava aberto.

## Cliques: o que mandar de volta ao app

O app escolheu uma mensagem (`uCallbackMessage`). Mandamos para `hWnd` do ícone:

| versão | wParam | lParam |
|---|---|---|
| ≤ 3 | uID | evento (WM_LBUTTONUP...) |
| 4 | x,y do clique (MAKEWPARAM) | MAKELPARAM(evento, uID) |

Sequência que o Explorador usa (ManagedShell confirma): botão esquerdo → `WM_LBUTTONDOWN`,
`WM_LBUTTONUP`, depois **`NIN_SELECT`**; direito → `WM_RBUTTONDOWN`, `WM_RBUTTONUP`, depois
**`WM_CONTEXTMENU`** (o Explorador manda esses dois "da versão 4" também para a versão 3).
Antes, **`AllowSetForegroundWindow(pid do app)`**: sem isso o menu do app abre atrás ou não fecha
ao clicar fora.

## Conviver com o Explorador (antes de virar o shell)

O ManagedShell cria o próprio `Shell_TrayWnd` como **janela topmost invisível** e a reafirma no
topo a cada 100 ms: assim o `FindWindow` dos apps encontra a nossa primeiro. Tudo que não tratamos
(AppBar, GetRect, mensagens WM_USER+) é **repassado** para o `Shell_TrayWnd` do Explorador.

Diferença nossa: também repassamos os `NIM_*` ao Explorador, para a barra de tarefas do Windows
continuar com os ícones enquanto testamos. Ao fechar, mandamos `TaskbarCreated` de novo para os apps
se registrarem só no Explorador. Ou seja: fechar o protótipo desfaz tudo.

## Limites conhecidos

- No Windows 11, **Wi-Fi, som e bateria não são ícones de bandeja**: fazem parte da barra XAML do
  Explorador. Teremos que fazer os nossos (APIs de rede, áudio `IMMDevice`, energia
  `GetSystemPowerStatus`).
- Ícones de apps que não respondem a `TaskbarCreated` só aparecem quando o app atualizar o ícone
  ou reabrir. (O ManagedShell também lê a lista de dentro do Explorador com `ReadProcessMemory`;
  fica para depois.)
- Balões/notificações (`NIF_INFO`) do Windows 10+ viram notificações do sistema; ignoramos.
