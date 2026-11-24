// Áudio e vídeo do Quick Look: Media Foundation Media Engine (o motor do app Filmes e TV) no
// modo "frame server". Cada quadro vai para uma textura na GPU, que vira bitmap do Direct2D e é
// desenhada dentro do painel, com cantos e animação, como no Mac.
#include "finder.h"
#include <d3d11_4.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace media {
namespace {

struct Notify : IMFMediaEngineNotify {
    LONG ref = 1;
    HWND hwnd = nullptr;
    UINT msg = 0;
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFMediaEngineNotify)) { *out = this; AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    // chega numa thread do Media Foundation: só repassa para a janela
    STDMETHODIMP EventNotify(DWORD ev, DWORD_PTR, DWORD) override {
        if (hwnd) PostMessageW(hwnd, msg, ev, 0);
        return S_OK;
    }
};

bool started;
HWND g_hwnd;
UINT g_msg;
IMFDXGIDeviceManager* mgr;
IMFMediaEngineClassFactory* factory;
IMFMediaEngine* eng;
Notify* notify;
ID3D11Texture2D* tex;
ID2D1Bitmap* bmp;
DWORD vw, vh;
std::wstring cur;
bool ready, ended, failed;

void ReleaseFrame() {
    if (bmp) { bmp->Release(); bmp = nullptr; }
    if (tex) { tex->Release(); tex = nullptr; }
}

bool Start() {
    if (started) return factory != nullptr;
    started = true;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
    auto* dev = (ID3D11Device*)gfx::Device();
    if (!dev) return false;
    ID3D11Multithread* mt = nullptr;  // o Media Foundation usa o mesmo dispositivo em outras threads
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&mt)))) { mt->SetMultithreadProtected(TRUE); mt->Release(); }
    UINT token = 0;
    if (FAILED(MFCreateDXGIDeviceManager(&token, &mgr)) || FAILED(mgr->ResetDevice(dev, token))) return false;
    return SUCCEEDED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
}

}  // namespace

void Init(HWND hwnd, UINT msg) { g_hwnd = hwnd; g_msg = msg; }

bool Open(const std::wstring& path) {
    Close();
    if (!Start()) { Log(L"mídia: Media Foundation indisponível"); return false; }
    notify = new Notify;
    notify->hwnd = g_hwnd;
    notify->msg = g_msg;
    IMFAttributes* attr = nullptr;
    MFCreateAttributes(&attr, 3);
    attr->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
    attr->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, mgr);
    attr->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    HRESULT hr = factory->CreateInstance(0, attr, &eng);
    attr->Release();
    if (FAILED(hr)) { Log(L"mídia: CreateInstance 0x%08X", (unsigned)hr); Close(); return false; }
    cur = path;
    ready = ended = failed = false;
    vw = vh = 0;
    eng->SetAutoPlay(TRUE);  // o Quick Look do Mac já começa tocando
    BSTR src = SysAllocString(path.c_str());
    hr = eng->SetSource(src);
    SysFreeString(src);
    if (FAILED(hr)) { Log(L"mídia: SetSource 0x%08X", (unsigned)hr); failed = true; }
    return SUCCEEDED(hr);
}

void Close() {
    if (notify) notify->hwnd = nullptr;  // eventos atrasados não chegam mais
    if (eng) { eng->Shutdown(); eng->Release(); eng = nullptr; }
    if (notify) { notify->Release(); notify = nullptr; }
    ReleaseFrame();
    cur.clear();
    vw = vh = 0;
    ready = ended = failed = false;
}

void OnEvent(DWORD ev) {
    if (!eng) return;
    switch (ev) {
    case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
        if (eng->HasVideo()) eng->GetNativeVideoSize(&vw, &vh);
        ready = true;
        break;
    case MF_MEDIA_ENGINE_EVENT_ENDED: ended = true; break;
    case MF_MEDIA_ENGINE_EVENT_PLAYING: ended = false; break;
    case MF_MEDIA_ENGINE_EVENT_ERROR: {
        failed = true;
        IMFMediaError* err = nullptr;
        if (SUCCEEDED(eng->GetError(&err)) && err) { Log(L"mídia: erro %u (0x%08X) em %ls", err->GetErrorCode(), (unsigned)err->GetExtendedErrorCode(), cur.c_str()); err->Release(); }
        break;
    }
    }
}

const std::wstring& Path() { return cur; }
bool Active() { return eng != nullptr; }
bool Ready() { return ready; }
bool Failed() { return failed; }
bool HasVideo() { return vw > 0 && vh > 0; }
UINT VideoW() { return vw; }
UINT VideoH() { return vh; }
bool Paused() { return !eng || eng->IsPaused() || ended; }
bool Ended() { return ended; }
double Duration() { double d = eng ? eng->GetDuration() : 0; return std::isfinite(d) ? d : 0; }
double Time() { return eng ? eng->GetCurrentTime() : 0; }
void Seek(double t) { if (eng) { eng->SetCurrentTime(std::max(0.0, t)); ended = false; } }
bool Muted() { return eng && eng->GetMuted(); }
void SetMuted(bool m) { if (eng) eng->SetMuted(m); }

void TogglePlay() {
    if (!eng) return;
    if (ended) { eng->SetCurrentTime(0); ended = false; eng->Play(); }
    else if (eng->IsPaused()) eng->Play();
    else eng->Pause();
}

// Copia o quadro novo (se houver) para a textura. Chamar fora do BeginDraw.
bool UpdateFrame() {
    if (!eng || !HasVideo()) return false;
    LONGLONG pts;
    if (eng->OnVideoStreamTick(&pts) != S_OK) return false;
    if (!tex) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = vw; d.Height = vh; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(((ID3D11Device*)gfx::Device())->CreateTexture2D(&d, nullptr, &tex))) return false;
        bmp = gfx::BitmapFromTexture(tex);
    }
    RECT dst{ 0, 0, (LONG)vw, (LONG)vh };
    MFARGB black{ 0, 0, 0, 255 };
    return SUCCEEDED(eng->TransferVideoFrame(tex, nullptr, &dst, &black));
}

ID2D1Bitmap* Frame() { return bmp; }

}  // namespace media
