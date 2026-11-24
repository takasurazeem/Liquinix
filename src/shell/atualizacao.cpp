// Aviso de atualização (Ajustes do Sistema > Geral > Atualização de Software > "Avisar quando houver atualização",
// ajuste avisar_atualizacao, ligado por padrão): 90 s depois de entrar e a cada 6 horas o Shell roda
// tools\atualizar-github.ps1 -Verificar numa thread. Se o GitHub tem uma versão mais nova que a instalada
// (bin\versao.txt), mostra uma notificação "Atualizações Disponíveis" uma vez por versão (atualizacao_avisada=).
// Clicar nela abre o Ajustes na página de Atualização de Software (notificacoes.cpp, OpenApp).
//
// A notificação é do Windows (vira o banner do Mac pela Central de Notificações): o remetente é o AUMID
// Liquinix.Atualizacao, registrado em HKCU\Software\Classes\AppUserModelId\Liquinix.Atualizacao (nome e ícone que o
// Windows mostra). O desinstalador apaga essa chave.
#include "shell.h"
#include "../comum/idioma.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>
#include <mutex>
#include <string>
#include <thread>

namespace {

const wchar_t* AUMID = L"Liquinix.Atualizacao";
HWND g_to = nullptr;
UINT g_msg = 0;
std::mutex g_mx;
std::wstring g_atual, g_novo;
int g_mudancas = 0;

std::wstring Root() { return shell::dir.substr(0, shell::dir.find_last_of(L'\\')); }

// Uma checagem (≈ 2 a 10 s, rede): atual= e novo= do script
void Checar() {
    std::wstring script = Root() + L"\\tools\\atualizar-github.ps1";
    if (GetFileAttributesW(script.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    wchar_t tmp[MAX_PATH], f[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    if (!GetTempFileNameW(tmp, L"wau", 0, f)) return;
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script + L"\" -Verificar -Saida \"" + f + L"\" -Bin \"" + shell::dir + L"\"";
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi)) { DeleteFileW(f); return; }
    WaitForSingleObject(pi.hProcess, 60000);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    std::wstring atual, novo, erro;
    int n = 0;
    FILE* fp = nullptr;
    if (!_wfopen_s(&fp, f, L"r, ccs=UTF-8") && fp) {
        wchar_t line[1024];
        while (fgetws(line, 1024, fp)) {
            std::wstring l = line;
            while (!l.empty() && (l.back() == L'\n' || l.back() == L'\r')) l.pop_back();
            if (!l.empty() && l[0] == 0xFEFF) l.erase(0, 1);
            if (l.rfind(L"atual=", 0) == 0) atual = l.substr(6);
            else if (l.rfind(L"novo=", 0) == 0) novo = l.substr(5);
            else if (l.rfind(L"erro=", 0) == 0) erro = l.substr(5);
            else if (l.rfind(L"commit=", 0) == 0) n++;
        }
        fclose(fp);
    }
    DeleteFileW(f);
    if (!erro.empty() || novo.empty() || novo == atual) {
        if (!erro.empty()) Log(L"atualização: não deu para conferir (%ls)", erro.c_str());
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_atual = atual; g_novo = novo; g_mudancas = n;
    }
    PostMessageW(g_to, g_msg, 0, 0);
}

void Registrar() {
    std::wstring key = std::wstring(L"Software\\Classes\\AppUserModelId\\") + AUMID;
    HKEY k = nullptr;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, &disp)) return;
    std::wstring name = Tr(L"Ajustes do Sistema");
    RegSetValueExW(k, L"DisplayName", 0, REG_SZ, (const BYTE*)name.c_str(), (DWORD)((name.size() + 1) * sizeof(wchar_t)));
    std::wstring icon = Root() + L"\\icones\\tahoe\\Settings.png";
    if (GetFileAttributesW(icon.c_str()) != INVALID_FILE_ATTRIBUTES)
        RegSetValueExW(k, L"IconUri", 0, REG_SZ, (const BYTE*)icon.c_str(), (DWORD)((icon.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    if (disp == REG_CREATED_NEW_KEY) Log(L"registro: criada HKCU\\%ls (remetente do aviso de atualização)", key.c_str());
}

std::wstring Xml(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'&') o += L"&amp;"; else if (c == L'<') o += L"&lt;"; else if (c == L'>') o += L"&gt;"; else if (c == L'"') o += L"&quot;"; else o += c;
    }
    return o;
}

}  // namespace

void atualizacao::Start(HWND msgWnd) {
    g_to = msgWnd;
    g_msg = RegisterWindowMessageW(L"ShellAtualizacaoDisponivel");
    std::thread([] {
        Sleep(90 * 1000);  // depois de entrar: não disputa a rede e o disco com o login
        for (;;) {
            try { Checar(); } catch (...) {}
            Sleep(6 * 60 * 60 * 1000);
        }
    }).detach();
}

bool atualizacao::Handle(UINT m) {
    if (!g_msg || m != g_msg) return false;
    std::wstring atual, novo;
    int n = 0;
    { std::lock_guard<std::mutex> lk(g_mx); atual = g_atual; novo = g_novo; n = g_mudancas; }
    if (novo.empty()) return true;
    if (shell::Setting(L"avisar_atualizacao", L"1") == L"0") return true;
    if (shell::Setting(L"atualizacao_avisada", L"") == novo) return true;  // uma vez por versão
    shell::SetSetting(L"atualizacao_avisada", novo);
    Log(L"atualização: %ls disponível (instalada %ls, %d mudanças)", novo.c_str(), atual.c_str(), n);
    Registrar();
    std::wstring corpo = n > 0 ? (L"Liquinix " + novo + L" " + Tr(L"está pronto para instalar") + L" (" + std::to_wstring(n) + (n == 1 ? Tr(L" mudança") : Tr(L" mudanças")) + L").")
                               : (L"Liquinix " + novo + L" " + Tr(L"está pronto para instalar") + L".");
    std::wstring xml = L"<toast launch=\"atualizacao\"><visual><binding template=\"ToastGeneric\"><text>" + Xml(Tr(L"Atualizações Disponíveis")) +
                       L"</text><text>" + Xml(corpo) + L"</text></binding></visual></toast>";
    try {
        winrt::Windows::Data::Xml::Dom::XmlDocument doc;
        doc.LoadXml(xml);
        winrt::Windows::UI::Notifications::ToastNotificationManager::CreateToastNotifier(AUMID).Show(winrt::Windows::UI::Notifications::ToastNotification(doc));
    } catch (winrt::hresult_error const& e) {
        Log(L"atualização: notificação recusada (0x%08X %ls)", (unsigned)e.code(), e.message().c_str());
    } catch (...) {}
    return true;
}
