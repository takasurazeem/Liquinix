// Quem pode receber a LiquinixWindow.dll (barra de título do Mac nos outros programas) e quem NUNCA pode.
// Regra dura (pedido de 20/11/2026): a DLL não entra em jogo nem em nada com anti-cheat (risco de banimento).
// Usado nos dois lados:
//   - pelo Shell (src\shell\janelas.cpp), ANTES de pôr o gancho: só olha o caminho do .exe e os arquivos da pasta
//     dele no disco (nada de abrir o processo do jogo além do nome, que é o que o Gerenciador de Tarefas faz);
//   - pela própria DLL (src\janelas\macjanela.cpp), ao ser carregada: se mesmo assim cair num jogo, recusa carregar
//     (DllMain devolve FALSE e o Windows a tira na hora) — segunda trava, para o que a primeira não pegou.
// Como decide (na dúvida, fora):
//   1. nome do .exe de launchers, anti-cheat e jogos conhecidos;
//   2. pastas de jogos das lojas (Steam, Epic, Riot, EA, Ubisoft, GOG, Xbox, Battle.net, Rockstar...) e qualquer
//      caminho com "\games\" / "\jogos\";
//   3. arquivos de motor/loja/anti-cheat ao lado do .exe (UnityPlayer.dll, steam_api64.dll, EOSSDK, pasta
//      EasyAntiCheat/BattlEye...) e o layout do Unreal (\Binaries\Win64\);
//   4. classe da janela de motor de jogo (UnrealWindow, UnityWndClass, SDL, GLFW...);
//   5. no Shell: app em tela cheia exclusiva (Direct3D) ou janela sem borda cobrindo o monitor que não é Chromium.
// Janelas sem a barra do Windows (modo "barra própria") só em apps Chromium/Electron reconhecidos pelos arquivos
// (Chrome, Edge, Brave, Discord, VS Code, Obsidian, Slack...), sem nenhuma marca de jogo.
#pragma once
#include <windows.h>
#include <string>
#include <cwctype>
#include <cwchar>

namespace jogos {

inline std::wstring Minusculo(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

// launchers, anti-cheat, overlays de jogo e jogos conhecidos (nome do .exe, minúsculo)
inline bool ExeProibido(const std::wstring& exe) {
    static const wchar_t* L_[] = {
        L"steam.exe", L"steamwebhelper.exe", L"steamservice.exe", L"gameoverlayui.exe", L"gameoverlayui64.exe",
        L"battle.net.exe", L"agent.exe", L"blizzard error.exe", L"eadesktop.exe", L"eabackgroundservice.exe", L"ealauncher.exe",
        L"origin.exe", L"originwebhelperservice.exe", L"epicgameslauncher.exe", L"epicwebhelper.exe", L"epiconlineservices.exe",
        L"riotclientservices.exe", L"riotclientux.exe", L"riotclientuxrender.exe", L"riot client.exe",
        L"leagueclient.exe", L"leagueclientux.exe", L"leagueclientuxrender.exe", L"league of legends.exe",
        L"valorant.exe", L"valorant-win64-shipping.exe", L"vgc.exe", L"vgtray.exe",
        L"faceit.exe", L"faceitclient.exe", L"faceitservice.exe", L"esportal.exe", L"gamersclub.exe", L"gcclient.exe",
        L"easyanticheat.exe", L"easyanticheat_eos.exe", L"easyanticheat_setup.exe", L"beservice.exe", L"beservice_x64.exe",
        L"belauncher.exe", L"xigncode.exe", L"nprotect.exe", L"gameguard.exe", L"ucldr_battlegrounds_gl.exe", L"tslgame.exe",
        L"ubisoftconnect.exe", L"upc.exe", L"uplaywebcore.exe", L"galaxyclient.exe", L"gogclient.exe",
        L"xboxpcapp.exe", L"gamebar.exe", L"gamebarftserver.exe", L"xboxappservices.exe",
        L"robloxplayerbeta.exe", L"robloxplayerlauncher.exe", L"robloxstudiobeta.exe", L"minecraftlauncher.exe", L"minecraft.windows.exe",
        L"javaw.exe", L"rockstarlauncher.exe", L"launcher.exe", L"socialclubhelper.exe", L"playgtav.exe", L"gta5.exe",
        L"overwolf.exe", L"overwolfbrowser.exe", L"medal.exe", L"hoyoplay.exe", L"launcherhoyoplay.exe", L"genshinimpact.exe",
        L"yuanshen.exe", L"starrail.exe", L"zenlesszonezero.exe", L"wargaminggamecenter.exe", L"wgc.exe",
        L"amazongames.exe", L"amazon games ui.exe", L"itch.exe", L"playnite.desktopapp.exe", L"playnite.fullscreenapp.exe",
        L"bsglauncher.exe", L"escapefromtarkov.exe", L"fortniteclient-win64-shipping.exe", L"fortnitelauncher.exe",
        L"cs2.exe", L"csgo.exe", L"dota2.exe", L"r5apex.exe", L"r5apex_dx12.exe", L"overwatch.exe", L"rainbowsix.exe",
        L"rainbowsix_be.exe", L"cod.exe", L"modernwarfare.exe", L"blackops6.exe", L"destiny2.exe", L"rustclient.exe",
        L"pubg.exe", L"eldenring.exe", L"start_protected_game.exe", L"osu!.exe", L"lutris.exe",
    };
    for (auto e : L_) if (exe == e) return true;
    return false;
}

// pastas de jogos (caminho completo em minúsculas)
inline bool CaminhoDeJogo(const std::wstring& caminho) {
    static const wchar_t* P[] = {
        L"\\steamapps\\", L"\\steam\\", L"\\riot games\\", L"\\epic games\\", L"\\ea games\\", L"\\ea app\\", L"\\electronic arts\\",
        L"\\origin games\\", L"\\battle.net\\", L"\\blizzard", L"\\ubisoft", L"\\gog galaxy\\", L"\\gog games\\", L"\\xboxgames\\",
        L"\\rockstar games\\", L"\\easyanticheat", L"\\battleye", L"\\games\\", L"\\jogos\\", L"\\hoyoplay\\", L"\\mihoyo\\",
        L"\\genshin impact", L"\\wargaming", L"\\amazon games\\", L"\\itch\\apps\\", L"\\riot vanguard\\", L"\\faceit",
        L"\\battlestate games\\", L"\\call of duty\\", L"\\overwatch\\", L"\\roblox\\", L"\\.minecraft\\", L"\\minecraft",
        L"\\microsoft.gaming", L"\\windowsapps\\microsoft.xbox", L"\\playnite\\", L"\\heroic\\",
    };
    for (auto p : P) if (caminho.find(p) != std::wstring::npos) return true;
    return false;
}

// motores de jogo (classe da janela principal)
inline bool ClasseDeJogo(const wchar_t* cls) {
    static const wchar_t* C[] = { L"UnrealWindow", L"UnityWndClass", L"SDL_app", L"GLFW30", L"Valve001", L"CryENGINE",
                                  L"RiotWindowClass", L"LWJGL", L"SFML_Window", L"OgreD3D11Wnd", L"TankWindowClass", L"Godot_Engine",
                                  L"GodotWindow", L"raylib", L"GameMaker", L"YYGameMakerYY", L"OsuWindow", L"Respawn001", L"Tarkov" };
    for (auto c : C) if (!wcscmp(cls, c)) return true;
    return false;
}

inline bool Existe(const std::wstring& f) { return GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES; }

// arquivos de motor de jogo, de loja de jogos ou de anti-cheat na pasta do .exe (pasta SEM a barra no fim)
inline bool MarcasDeJogo(const std::wstring& pasta) {
    static const wchar_t* F[] = {
        L"UnityPlayer.dll", L"GameAssembly.dll", L"steam_api64.dll", L"steam_api.dll", L"steamclient64.dll",
        L"EOSSDK-Win64-Shipping.dll", L"EOSSDK-Win32-Shipping.dll", L"galaxy64.dll", L"Galaxy.dll", L"BEClient_x64.dll",
        L"BEClient.dll", L"EasyAntiCheat", L"BattlEye", L"EasyAntiCheat_EOS_Setup.exe", L"start_protected_game.exe",
        L"discord_game_sdk.dll", L"bink2w64.dll", L"PhysX3_x64.dll", L"PhysX_64.dll", L"fmodstudio.dll", L"fmod.dll",
        L"amd_ags_x64.dll", L"nvngx_dlss.dll", L"sl.interposer.dll", L"xess.dll", L"libxess.dll",
    };
    for (auto f : F) if (Existe(pasta + L"\\" + f)) return true;
    return false;
}

// O .exe é de jogo / launcher / anti-cheat? (caminho completo; não abre o processo)
inline bool ProgramaDeJogo(const std::wstring& caminhoOriginal) {
    std::wstring c = Minusculo(caminhoOriginal);
    size_t barra = c.find_last_of(L'\\');
    if (barra == std::wstring::npos) return false;
    std::wstring exe = c.substr(barra + 1);
    if (ExeProibido(exe) || CaminhoDeJogo(c)) return true;
    if (c.find(L"\\binaries\\win64\\") != std::wstring::npos || c.find(L"\\binaries\\win32\\") != std::wstring::npos) return true;  // Unreal
    if (c.find(L"-shipping.exe") != std::wstring::npos || c.find(L"_be.exe") != std::wstring::npos || c.find(L"_eac.exe") != std::wstring::npos) return true;
    return MarcasDeJogo(caminhoOriginal.substr(0, barra));
}

// App Chromium/Electron reconhecido pelos arquivos da pasta do .exe: os navegadores conhecidos pelo nome; os apps
// Electron/CEF pelos pacotes do Chromium ao lado do .exe (resources.pak, icudtl.dat, v8_context_snapshot.bin).
inline bool ProgramaChromium(const std::wstring& caminhoOriginal) {
    std::wstring c = Minusculo(caminhoOriginal);
    size_t barra = c.find_last_of(L'\\');
    if (barra == std::wstring::npos) return false;
    std::wstring exe = c.substr(barra + 1), pasta = caminhoOriginal.substr(0, barra);
    static const wchar_t* NAVEGADORES[] = { L"chrome.exe", L"msedge.exe", L"brave.exe", L"vivaldi.exe", L"opera.exe", L"opera_gx.exe",
                                            L"chromium.exe", L"thorium.exe", L"arc.exe", L"yandex.exe", L"browser.exe" };
    for (auto n : NAVEGADORES) if (exe == n) return true;
    int marcas = 0;
    for (auto f : { L"resources.pak", L"chrome_100_percent.pak", L"icudtl.dat", L"v8_context_snapshot.bin", L"snapshot_blob.bin" })
        if (Existe(pasta + L"\\" + f)) marcas++;
    return marcas >= 2;
}

}  // namespace jogos
