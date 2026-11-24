// Mesas (áreas de trabalho virtuais do Windows) sem passar por atalhos de teclado.
// O Windows não tem API pública para remover/criar/trocar mesas sem mudar de mesa; esta é a interface
// interna do Explorer (IVirtualDesktopManagerInternal, Windows 11 24H2/25H2), a mesma que ferramentas
// como o VirtualDesktop usam. Antes de usar, ela é conferida: tem de devolver o mesmo número de mesas
// e os mesmos IDs gravados no registro. Se o IID não existir nesta versão ou algo não bater, as funções
// devolvem false e quem chamou usa os atalhos (Win+Ctrl+setas/D/F4).
#include "shell.h"
#include <shobjidl.h>
#include <objectarray.h>
#include <servprov.h>

namespace {

const CLSID CLSID_ImmersiveShell = { 0xC2F03A33, 0x21F5, 0x47FA, { 0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39 } };
const CLSID CLSID_VDMInternal = { 0xC5E0CDCA, 0x7B6E, 0x41B2, { 0x9F, 0xC4, 0xD9, 0x39, 0x75, 0xCC, 0x46, 0x7B } };
// Windows 11 24H2 / 25H2
const IID IID_IVD = { 0x3F07F4BE, 0xB107, 0x441A, { 0xAF, 0x0F, 0x39, 0xD8, 0x25, 0x29, 0x07, 0x2C } };
const IID IID_IVDMI = { 0x53F5CA0B, 0x158F, 0x4124, { 0x90, 0x0C, 0x05, 0x71, 0x58, 0x06, 0x0B, 0x27 } };

struct IVD : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsViewVisible(IUnknown* view, BOOL* visible) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetID(GUID* id) = 0;
};
struct IVDMI : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCount(UINT* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveViewToDesktop(IUnknown* view, IVD* d) = 0;
    virtual HRESULT STDMETHODCALLTYPE CanViewMoveDesktops(IUnknown* view, BOOL* can) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentDesktop(IVD** d) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDesktops(IObjectArray** arr) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAdjacentDesktop(IVD* from, int dir, IVD** d) = 0;
    virtual HRESULT STDMETHODCALLTYPE SwitchDesktop(IVD* d) = 0;
    virtual HRESULT STDMETHODCALLTYPE SwitchDesktopAndMoveForegroundView(IVD* d) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateDesktop(IVD** d) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveDesktop(IVD* d, int index) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveDesktop(IVD* d, IVD* fallback) = 0;
};

std::vector<GUID> RegistryIds() {
    std::vector<GUID> out;
    BYTE ids[16 * 64]; DWORD cb = sizeof(ids);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops", L"VirtualDesktopIDs",
                     RRF_RT_REG_BINARY, nullptr, ids, &cb) == ERROR_SUCCESS)
        for (DWORD i = 0; i + 16 <= cb; i += 16) { GUID g; memcpy(&g, ids + i, 16); out.push_back(g); }
    return out;
}

// a interface interna, conferida contra o registro; nullptr se não der para confiar nela
IVDMI* Open(std::vector<IVD*>& desks) {
    desks.clear();
    if (mesa::Standalone()) return nullptr;  // a interface é do Explorador: sem ele não há mesas virtuais
    IServiceProvider* sp = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sp))) || !sp) return nullptr;
    IVDMI* m = nullptr;
    HRESULT hr = sp->QueryService(CLSID_VDMInternal, IID_IVDMI, (void**)&m);
    sp->Release();
    if (FAILED(hr) || !m) { Log(L"mesas: interface interna indisponível (0x%08X); usando atalhos", (unsigned)hr); return nullptr; }
    auto reg = RegistryIds();
    UINT n = 0;
    IObjectArray* arr = nullptr;
    bool ok = SUCCEEDED(m->GetCount(&n)) && n == reg.size() && n > 0 && SUCCEEDED(m->GetDesktops(&arr)) && arr;
    if (ok) {
        UINT c = 0; arr->GetCount(&c);
        ok = c == n;
        for (UINT i = 0; ok && i < c; i++) {
            IVD* d = nullptr;
            if (FAILED(arr->GetAt(i, IID_IVD, (void**)&d)) || !d) { ok = false; break; }
            GUID g{};
            if (FAILED(d->GetID(&g)) || memcmp(&g, &reg[i], sizeof(GUID))) ok = false;
            desks.push_back(d);
        }
    }
    if (arr) arr->Release();
    if (!ok) {
        Log(L"mesas: interface interna não confere com o registro; usando atalhos");
        for (auto* d : desks) d->Release();
        desks.clear();
        m->Release();
        return nullptr;
    }
    return m;
}

// a "vista" de uma janela (o que a interface das mesas move); IID estável do Windows 10 ao 11 25H2
const IID IID_IAVC = { 0x1841C6D7, 0x4F9D, 0x42C0, { 0xAF, 0x41, 0x87, 0x47, 0x53, 0x8F, 0x10, 0xE5 } };
struct IAVC : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetViews(IObjectArray** a) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByZOrder(IObjectArray** a) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByAppUserModelId(PCWSTR id, IObjectArray** a) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewForHwnd(HWND h, IUnknown** view) = 0;
};

void Free(IVDMI* m, std::vector<IVD*>& desks) {
    for (auto* d : desks) d->Release();
    desks.clear();
    if (m) m->Release();
}

}  // namespace

bool mesas::Switch(int index) {
    std::vector<IVD*> d;
    IVDMI* m = Open(d);
    if (!m) return false;
    bool ok = index >= 0 && index < (int)d.size() && SUCCEEDED(m->SwitchDesktop(d[index]));
    Free(m, d);
    return ok;
}

bool mesas::Create() {
    std::vector<IVD*> d;
    IVDMI* m = Open(d);
    if (!m) return false;
    IVD* nd = nullptr;
    bool ok = SUCCEEDED(m->CreateDesktop(&nd)) && nd;
    if (nd) nd->Release();
    Free(m, d);
    return ok;
}

bool mesas::MoveWindow(HWND h, int index) {
    std::vector<IVD*> d;
    IVDMI* m = Open(d);
    bool ok = false;
    if (m && index >= 0 && index < (int)d.size()) {
        IServiceProvider* sp = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_ImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sp))) && sp) {
            IAVC* c = nullptr;
            if (SUCCEEDED(sp->QueryService(IID_IAVC, IID_IAVC, (void**)&c)) && c) {
                IUnknown* view = nullptr;
                HRESULT hr = c->GetViewForHwnd(h, &view);
                if (SUCCEEDED(hr) && view) {
                    hr = m->MoveViewToDesktop(view, d[index]);
                    ok = SUCCEEDED(hr);
                    view->Release();
                }
                if (!ok) Log(L"mesas: mover janela falhou (0x%08X)", (unsigned)hr);
                c->Release();
            }
            sp->Release();
        }
    }
    Free(m, d);
    if (!ok) {  // a API pública (só funciona com janelas do próprio Shell, mas não custa tentar)
        auto ids = RegistryIds();
        IVirtualDesktopManager* vdm = nullptr;
        if (index >= 0 && index < (int)ids.size() && SUCCEEDED(CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&vdm))) && vdm) {
            ok = SUCCEEDED(vdm->MoveWindowToDesktop(h, ids[index]));
            vdm->Release();
        }
    }
    return ok;
}

bool mesas::Remove(int index) {
    std::vector<IVD*> d;
    IVDMI* m = Open(d);
    if (!m) return false;
    bool ok = false;
    if (index >= 0 && index < (int)d.size() && d.size() > 1) {
        IVD* fallback = d[index > 0 ? index - 1 : 1];  // as janelas vão para a mesa vizinha, como no Mac
        ok = SUCCEEDED(m->RemoveDesktop(d[index], fallback));
    }
    Free(m, d);
    return ok;
}
