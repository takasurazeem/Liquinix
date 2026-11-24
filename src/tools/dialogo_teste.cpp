// DialogoTeste: abre a janela de Abrir ou Salvar do Windows (IFileOpenDialog/IFileSaveDialog),
// como qualquer programa faz, e grava o resultado em dialogo-teste.txt. Serve para testar a
// DialogoMac.dll sem mexer em programas de verdade.
//   DialogoTeste.exe abrir | salvar | pasta | multi
#include <windows.h>
#include <shobjidl.h>
#include <cstdio>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmd, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring mode = cmd && *cmd ? cmd : L"abrir";
    while (!mode.empty() && (mode[0] == L' ' || mode[0] == L'"')) mode.erase(0, 1);
    while (!mode.empty() && (mode.back() == L' ' || mode.back() == L'"')) mode.pop_back();
    bool save = mode == L"salvar";
    IFileDialog* d = nullptr;
    HRESULT hr = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  save ? IID_IFileSaveDialog : IID_IFileOpenDialog, (void**)&d);
    std::wstring out = L"criar: 0x" + std::to_wstring((unsigned)hr) + L"\n";
    if (SUCCEEDED(hr)) {
        COMDLG_FILTERSPEC types[] = { { L"Documentos de texto", L"*.txt;*.md" }, { L"Imagens", L"*.png;*.jpg" }, { L"Todos os arquivos", L"*.*" } };
        d->SetFileTypes(3, types);
        d->SetFileTypeIndex(1);
        FILEOPENDIALOGOPTIONS o = 0;
        d->GetOptions(&o);
        if (mode == L"pasta") o |= FOS_PICKFOLDERS;
        if (mode == L"multi") o |= FOS_ALLOWMULTISELECT;
        d->SetOptions(o);
        if (save) { d->SetFileName(L"Sem título"); d->SetDefaultExtension(L"txt"); }
        HWND owner = CreateWindowExW(0, L"STATIC", L"DialogoTeste", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 200, 400, 200, nullptr, nullptr, nullptr, nullptr);
        hr = d->Show(owner);
        out += L"show: 0x" + std::to_wstring((unsigned)hr) + L"\n";
        if (SUCCEEDED(hr)) {
            if (mode == L"multi") {
                IFileOpenDialog* od = nullptr; IShellItemArray* arr = nullptr;
                if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&od))) && SUCCEEDED(od->GetResults(&arr))) {
                    DWORD n = 0; arr->GetCount(&n);
                    for (DWORD i = 0; i < n; i++) {
                        IShellItem* it = nullptr; PWSTR p = nullptr;
                        if (SUCCEEDED(arr->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out += L"resultado: " + std::wstring(p) + L"\n"; CoTaskMemFree(p); }
                        if (it) it->Release();
                    }
                    arr->Release();
                }
                if (od) od->Release();
            } else {
                IShellItem* it = nullptr; PWSTR p = nullptr;
                if (SUCCEEDED(d->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out += L"resultado: " + std::wstring(p) + L"\n"; CoTaskMemFree(p); }
                if (it) it->Release();
            }
            UINT ti = 0; d->GetFileTypeIndex(&ti);
            out += L"tipo: " + std::to_wstring(ti) + L"\n";
        }
        DestroyWindow(owner);
        d->Release();
    }
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path = exe; path = path.substr(0, path.find_last_of(L'\\')) + L"\\dialogo-teste.txt";
    FILE* f = nullptr;
    if (!_wfopen_s(&f, path.c_str(), L"w, ccs=UTF-8") && f) { fputws(out.c_str(), f); fclose(f); }
    CoUninitialize();
    return 0;
}
