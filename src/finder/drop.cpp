// Soltar arquivos. Sobre uma pasta (no conteúdo ou na barra lateral) repassamos tudo para o
// IDropTarget do próprio Windows, então copiar/mover/atalho, conflitos e progresso funcionam
// como no Explorer. Entre dois favoritos, a pasta solta vira favorito; sobre uma etiqueta da
// barra lateral, os itens ganham a etiqueta. O IDropTargetHelper desenha a imagem do arraste.
#include "finder.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include "../menu/context_menu.h"

namespace {
std::vector<std::wstring> PathsOf(IDataObject* d) {
    std::vector<std::wstring> out;
    FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM m{};
    if (FAILED(d->GetData(&fe, &m))) return out;
    if (auto drop = (HDROP)GlobalLock(m.hGlobal)) {
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            wchar_t p[MAX_PATH * 2];
            if (DragQueryFileW(drop, i, p, MAX_PATH * 2)) out.push_back(p);
        }
        GlobalUnlock(m.hGlobal);
    }
    ReleaseStgMedium(&m);
    return out;
}

bool AnyFolder(const std::vector<std::wstring>& paths) {
    for (auto& p : paths) {
        DWORD a = GetFileAttributesW(p.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return true;
    }
    return false;
}

class DropTarget : public IDropTarget {
    LONG ref_ = 1;
    HWND hwnd_;
    IDataObject* data_ = nullptr;
    std::vector<std::wstring> paths_;  // arquivos arrastados (para favoritos e etiquetas)
    IDropTarget* inner_ = nullptr;
    std::wstring innerPath_;
    IDropTargetHelper* helper_ = nullptr;
    // pastas com mola (Mac): parado em cima de uma pasta segurando o arraste, ela pisca e abre
    std::wstring springPath_;
    DWORD springSince_ = 0;

    // Troca o alvo interno quando o cursor passa para outra pasta.
    void SwitchTo(const std::wstring& path, DWORD keys, POINTL pt, DWORD allowed) {
        if (path == innerPath_ && (inner_ || path.empty())) return;
        if (inner_) { inner_->DragLeave(); inner_->Release(); inner_ = nullptr; }
        innerPath_ = path;
        if (path.empty()) return;
        if (SUCCEEDED(ctxmenu::GetUIObject(hwnd_, { path }, IID_IDropTarget, (void**)&inner_))) {
            DWORD e = allowed;
            if (FAILED(inner_->DragEnter(data_, keys, pt, &e))) { inner_->Release(); inner_ = nullptr; }
        }
    }

    DropSpot SpotAt(POINTL pt) {
        POINT p{ pt.x, pt.y };
        ScreenToClient(hwnd_, &p);
        DropSpot s = DropSpotAt(p);
        SetDropFeedback(s);
        return s;
    }

    // Efeito para os destinos tratados por nós (favorito / etiqueta).
    DWORD CustomEffect(const DropSpot& s) {
        if (s.kind == DropSpot::SIDEBAR_INSERT) return AnyFolder(paths_) ? DROPEFFECT_LINK : DROPEFFECT_NONE;
        if (s.kind == DropSpot::TAG) return paths_.empty() ? DROPEFFECT_NONE : DROPEFFECT_LINK;
        return DROPEFFECT_NONE;
    }

    void Reset() {
        springPath_.clear();
        if (inner_) { inner_->Release(); inner_ = nullptr; }
        if (data_) { data_->Release(); data_ = nullptr; }
        innerPath_.clear();
        paths_.clear();
        SetDropFeedback(DropSpot{});
    }

public:
    explicit DropTarget(HWND h) : hwnd_(h) {
        CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper_));
    }
    ~DropTarget() { if (helper_) helper_->Release(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&ref_); if (!r) delete this; return r; }

    STDMETHODIMP DragEnter(IDataObject* d, DWORD keys, POINTL pt, DWORD* eff) override {
        data_ = d; d->AddRef();
        paths_ = PathsOf(d);
        innerPath_.clear();
        if (helper_) { POINT p{ pt.x, pt.y }; helper_->DragEnter(hwnd_, d, &p, *eff); }
        return DragOver(keys, pt, eff);
    }

    STDMETHODIMP DragOver(DWORD keys, POINTL pt, DWORD* eff) override {
        DWORD allowed = *eff;
        DropSpot s = SpotAt(pt);
        bool overFolder = s.kind == DropSpot::FOLDER && (s.item >= 0 || s.row >= 0) && !s.path.empty();
        if (!overFolder) springPath_.clear();
        else if (s.path != springPath_) { springPath_ = s.path; springSince_ = GetTickCount(); }
        else if (GetTickCount() - springSince_ > 900) {
            for (int k = 0; k < 2; k++) {  // pisca duas vezes, como no Mac
                SetDropFeedback(DropSpot{}); RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW); Sleep(70);
                SetDropFeedback(s); RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW); Sleep(70);
            }
            std::wstring path = springPath_;
            springPath_.clear();
            SwitchTo(L"", keys, pt, allowed);  // o alvo antigo some junto com a pasta de antes
            SpringLoad(path);
            s = SpotAt(pt);
        }
        SwitchTo(s.kind == DropSpot::FOLDER ? s.path : L"", keys, pt, allowed);
        if (s.kind == DropSpot::FOLDER && inner_) inner_->DragOver(keys, pt, eff);
        else *eff = CustomEffect(s) & allowed;
        if (helper_) { POINT p{ pt.x, pt.y }; helper_->DragOver(&p, *eff); }
        return S_OK;
    }

    STDMETHODIMP DragLeave() override {
        if (helper_) helper_->DragLeave();
        if (inner_) inner_->DragLeave();
        Reset();
        return S_OK;
    }

    STDMETHODIMP Drop(IDataObject* d, DWORD keys, POINTL pt, DWORD* eff) override {
        if (helper_) { POINT p{ pt.x, pt.y }; helper_->Drop(d, &p, *eff); }
        DropSpot s = SpotAt(pt);
        if (s.kind == DropSpot::FOLDER) {
            SwitchTo(s.path, keys, pt, *eff);
            if (inner_) inner_->Drop(d, keys, pt, eff);
            else *eff = DROPEFFECT_NONE;
        } else if (DWORD e = CustomEffect(s) & *eff) {
            DropCustom(s, paths_);
            *eff = e;  // "atalho": quem arrastou não apaga o original
        } else {
            *eff = DROPEFFECT_NONE;
        }
        Reset();
        return S_OK;
    }
};
}  // namespace

IDropTarget* CreateDropTarget(HWND hwnd) { return new DropTarget(hwnd); }

// Colar = soltar o conteúdo da área de transferência na pasta, com o efeito pedido por
// quem copiou ("Preferred DropEffect": copiar ou recortar).
HRESULT PasteInto(HWND hwnd, const std::wstring& folder) {
    IDataObject* d = nullptr;
    HRESULT hr = OleGetClipboard(&d);
    if (FAILED(hr)) return hr;
    DWORD eff = DROPEFFECT_COPY;
    FORMATETC fe{ (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM m{};
    if (SUCCEEDED(d->GetData(&fe, &m))) {
        if (auto* p = (DWORD*)GlobalLock(m.hGlobal)) { eff = *p; GlobalUnlock(m.hGlobal); }
        ReleaseStgMedium(&m);
    }
    if (eff & DROPEFFECT_MOVE) eff = DROPEFFECT_MOVE;
    else eff = DROPEFFECT_COPY;
    IDropTarget* t = nullptr;
    hr = ctxmenu::GetUIObject(hwnd, { folder }, IID_IDropTarget, (void**)&t);
    if (SUCCEEDED(hr)) {
        POINTL pt{ 0, 0 };
        DWORD e = eff;
        hr = t->DragEnter(d, MK_LBUTTON, pt, &e);
        if (SUCCEEDED(hr) && e != DROPEFFECT_NONE) { e = eff; hr = t->Drop(d, MK_LBUTTON, pt, &e); }
        else { t->DragLeave(); hr = E_FAIL; }
        t->Release();
    }
    d->Release();
    return hr;
}
