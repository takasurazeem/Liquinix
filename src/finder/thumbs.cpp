// Miniaturas e ícones em duas threads de fundo. Os pedidos mais recentes saem primeiro
// (o que acabou de aparecer na tela tem prioridade).
#include "finder.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cwctype>
#include <algorithm>

namespace {
struct Req { UINT gen; std::wstring path, key; int px; };

std::mutex mu;
std::condition_variable cv;
std::deque<Req> queue;
std::atomic<UINT> curGen{ 0 };
bool stopping = false;
std::vector<std::thread> workers;
HWND notifyWnd;
UINT notifyMsg;

std::wstring Ext(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.'), slash = path.find_last_of(L'\\');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return L"";
    std::wstring e = path.substr(dot + 1);
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e;
}

bool LinkToFolder(const std::wstring& lnk) {
    IShellLinkW* sl = nullptr;
    bool dir = false;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
        wchar_t t[MAX_PATH] = {};
        if (SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ)) && SUCCEEDED(sl->GetPath(t, MAX_PATH, nullptr, 0)) && t[0]) {
            DWORD a = GetFileAttributesW(t);
            dir = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
        }
        pf->Release();
    }
    sl->Release();
    return dir;
}

// Programas mostram o próprio ícone (como os apps no Mac); o resto só usa miniatura de verdade
// (foto, vídeo...). Sem miniatura, quem desenha é o app: folha branca no estilo do Mac.
thumbs::Result* Render(const Req& r) {
    std::wstring ext = Ext(r.path);
    if (ext == L"lnk" && LinkToFolder(r.path)) return new thumbs::Result{ r.gen, r.key, 0, 0, {}, thumbs::KIND_FOLDER };
    static const wchar_t* apps[] = { L"exe", L"msi", L"lnk", L"url", L"appref-ms", L"scr", L"com" };
    bool app = false;
    for (auto a : apps) if (ext == a) app = true;

    IShellItemImageFactory* f = nullptr;
    if (FAILED(SHCreateItemFromParsingName(r.path.c_str(), nullptr, IID_PPV_ARGS(&f)))) return nullptr;
    HBITMAP hb = nullptr;
    HRESULT hr = f->GetImage(SIZE{ r.px, r.px }, SIIGBF_RESIZETOFIT | (app ? SIIGBF_ICONONLY : SIIGBF_THUMBNAILONLY), &hb);
    f->Release();
    if (FAILED(hr) || !hb) return new thumbs::Result{ r.gen, r.key, 0, 0, {}, thumbs::KIND_DOCUMENT };

    BITMAP bm{}; GetObjectW(hb, sizeof(bm), &bm);
    auto* res = new thumbs::Result{ r.gen, r.key, (UINT)bm.bmWidth, (UINT)std::abs(bm.bmHeight), {}, thumbs::KIND_IMAGE };
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), (LONG)res->w, -(LONG)res->h, 1, 32, BI_RGB };
    res->bgra.resize((size_t)res->w * res->h * 4);
    HDC screen = GetDC(nullptr);
    GetDIBits(screen, hb, 0, res->h, res->bgra.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    DeleteObject(hb);
    // Miniaturas de foto às vezes vêm com alfa zerado: nesse caso a imagem é opaca.
    bool anyAlpha = false;
    for (size_t i = 3; i < res->bgra.size(); i += 4) if (res->bgra[i]) { anyAlpha = true; break; }
    if (!anyAlpha) for (size_t i = 3; i < res->bgra.size(); i += 4) res->bgra[i] = 255;
    return res;
}

void Work() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (;;) {
        Req r;
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [] { return stopping || !queue.empty(); });
            if (stopping) break;
            r = std::move(queue.back());
            queue.pop_back();
        }
        if (r.gen != curGen) continue;
        auto* res = Render(r);
        if (res && !PostMessageW(notifyWnd, notifyMsg, 0, (LPARAM)res)) delete res;
    }
    CoUninitialize();
}
}  // namespace

void thumbs::Start(HWND notify, UINT msg) {
    notifyWnd = notify;
    notifyMsg = msg;
    // 2 a 4 threads (metade dos núcleos): pastas de fotos e vídeos se enchem mais rápido
    unsigned n = std::clamp(std::thread::hardware_concurrency() / 2, 2u, 4u);
    for (unsigned i = 0; i < n; i++) workers.emplace_back(Work);
    // dentro de outro programa (MacFinder/MacDialogo): soltas. Presas, o destrutor delas na saída do programa
    // (threads já mortas, mas ainda "joinable") chamava terminate() e o programa caía ao fechar (0xc0000409).
    if (g_finderNativo || g_finderEmbutido) for (auto& t : workers) t.detach();
}

void thumbs::SetGeneration(UINT gen) {
    std::lock_guard<std::mutex> lk(mu);
    curGen = gen;
    queue.clear();
}

void thumbs::Request(UINT gen, const std::wstring& path, const std::wstring& key, int px) {
    {
        std::lock_guard<std::mutex> lk(mu);
        queue.push_back({ gen, path, key, px });
    }
    cv.notify_one();
}

void thumbs::Stop() {
    {
        std::lock_guard<std::mutex> lk(mu);
        stopping = true;
    }
    cv.notify_all();
    for (auto& t : workers) if (t.joinable()) t.join();
}
