// O que as páginas do Ajustes leem e mudam no Windows. Só APIs públicas, exceto IPolicyConfig
// (trocar o dispositivo de som padrão), que é a mesma interface usada pelo próprio Windows.
#include "ajustes.h"
#include "../comum/dll.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shlwapi.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wlanapi.h>
#include <iphlpapi.h>
#include <bluetoothapis.h>
#include <powrprof.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <lm.h>
#include <winspool.h>
#include <sddl.h>
#define SECURITY_WIN32
#include <security.h>
#include <atomic>
#include <mutex>
#include <thread>

#pragma comment(lib, "wlanapi.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "bthprops.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "winspool.lib")
#pragma comment(lib, "secur32.lib")

namespace {

template <class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::wstring RegStr(HKEY root, const wchar_t* key, const wchar_t* name) {
    wchar_t buf[512]; DWORD cb = sizeof(buf);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &cb) != ERROR_SUCCESS) return L"";
    return buf;
}
DWORD RegDword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD def) {
    DWORD v = def, cb = sizeof(v);
    RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, &v, &cb);
    return v;
}
std::wstring Trim(std::wstring s) {
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t i = 0; while (i < s.size() && iswspace(s[i])) i++;
    s = s.substr(i);
    std::wstring r; bool sp = false;  // espaços repetidos (nome do processador)
    for (wchar_t c : s) { if (c == L' ') { if (!sp) r += c; sp = true; } else { r += c; sp = false; } }
    return r;
}

// ---- Som
Com<IMMDeviceEnumerator> Enumerator() {
    Com<IMMDeviceEnumerator> e;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e));
    return e;
}

IAudioEndpointVolume* EndpointVolume(EDataFlow flow) {
    auto e = Enumerator();
    if (!e) return nullptr;
    Com<IMMDevice> d;
    if (FAILED(e->GetDefaultAudioEndpoint(flow, eConsole, &d))) return nullptr;
    IAudioEndpointVolume* v = nullptr;
    d->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&v);
    return v;
}

MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8") IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
const CLSID CLSID_PolicyConfigClient = { 0x870af99c, 0x171d, 0x4f9e, { 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9 } };

// ---- Wi-Fi
HANDLE Wlan() {
    static HANDLE h = nullptr;
    if (!h) { DWORD ver = 0; if (WlanOpenHandle(2, nullptr, &ver, &h) != ERROR_SUCCESS) h = nullptr; }
    return h;
}
bool WlanIf(GUID& g) {
    HANDLE h = Wlan();
    if (!h) return false;
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    if (WlanEnumInterfaces(h, nullptr, &list) != ERROR_SUCCESS || !list) return false;
    bool ok = list->dwNumberOfItems > 0;
    if (ok) g = list->InterfaceInfo[0].InterfaceGuid;
    WlanFreeMemory(list);
    return ok;
}
std::wstring SsidText(const DOT11_SSID& s) {
    std::string a((const char*)s.ucSSID, s.uSSIDLength);
    int n = MultiByteToWideChar(CP_UTF8, 0, a.c_str(), (int)a.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, a.c_str(), (int)a.size(), w.data(), n);
    return w;
}
std::wstring XmlEscape(const std::wstring& s) {
    std::wstring r;
    for (wchar_t c : s) {
        if (c == L'&') r += L"&amp;"; else if (c == L'<') r += L"&lt;"; else if (c == L'>') r += L"&gt;";
        else if (c == L'"') r += L"&quot;"; else if (c == L'\'') r += L"&apos;"; else r += c;
    }
    return r;
}
std::map<std::wstring, DOT11_AUTH_ALGORITHM> g_wifiAuth;
std::map<std::wstring, std::wstring> g_wifiProfile;

// ---- Energia
const GUID SUB_VIDEO = { 0x7516b95f, 0xf776, 0x4464, { 0x8c, 0x53, 0x06, 0x16, 0x7f, 0x40, 0xcc, 0x99 } };
const GUID VIDEO_OFF = { 0x3c0bc021, 0xc8a8, 0x4e07, { 0xa9, 0x73, 0x6b, 0x14, 0xcb, 0xcb, 0x2b, 0x7e } };
const GUID SUB_SLEEP = { 0x238c9fa8, 0x0aad, 0x41ed, { 0x83, 0xf4, 0x97, 0xbe, 0x24, 0x2c, 0x8f, 0x20 } };
const GUID SLEEP_IDLE = { 0x29f6c1db, 0x86da, 0x48c5, { 0x9f, 0xdb, 0xf2, 0xb6, 0x7b, 0x1f, 0x44, 0xda } };
const GUID MODE_SAVER = { 0x961cc777, 0x2547, 0x4f9d, { 0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a } };
const GUID MODE_PERF = { 0xded574b5, 0x45a0, 0x4f42, { 0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38 } };

int ReadPower(const GUID& sub, const GUID& set) {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS) return 0;
    DWORD v = 0;
    PowerReadACValueIndex(nullptr, scheme, &sub, &set, &v);
    LocalFree(scheme);
    return (int)(v / 60);
}
void WritePower(const GUID& sub, const GUID& set, int minutes) {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS) return;
    PowerWriteACValueIndex(nullptr, scheme, &sub, &set, minutes * 60);
    PowerWriteDCValueIndex(nullptr, scheme, &sub, &set, minutes * 60);
    PowerSetActiveScheme(nullptr, scheme);  // aplica
    LocalFree(scheme);
}

// ---- Brilho (DDC/CI): leitura e escrita lentas, então em segundo plano
std::atomic<float> g_bright{ -2 };
std::atomic<float> g_brightWant{ -1 };
std::atomic<bool> g_brightBusy{ false };
template <class F> void WithMonitor(F f) {
    HMONITOR mon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    DWORD n = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(mon, &n) || !n) return;
    std::vector<PHYSICAL_MONITOR> pm(n);
    if (!GetPhysicalMonitorsFromHMONITOR(mon, n, pm.data())) return;
    f(pm[0].hPhysicalMonitor);
    DestroyPhysicalMonitors(n, pm.data());
}
void BrightWorker() {
    float v;
    while ((v = g_brightWant.exchange(-1)) >= 0) {
        WithMonitor([&](HANDLE h) {
            DWORD mn = 0, cur = 0, mx = 100;
            if (GetMonitorBrightness(h, &mn, &cur, &mx)) SetMonitorBrightness(h, mn + (DWORD)std::lround(v * (mx - mn)));
        });
    }
    g_brightBusy = false;
}

}  // namespace

// ================= Som
float sys::Volume() {
    IAudioEndpointVolume* v = EndpointVolume(eRender);
    float f = 0; if (v) { v->GetMasterVolumeLevelScalar(&f); v->Release(); }
    return f;
}
void sys::SetVolume(float f) {
    if (IAudioEndpointVolume* v = EndpointVolume(eRender)) { v->SetMasterVolumeLevelScalar(std::clamp(f, 0.f, 1.f), nullptr); if (f > 0) v->SetMute(FALSE, nullptr); v->Release(); }
}
bool sys::Muted() {
    IAudioEndpointVolume* v = EndpointVolume(eRender);
    BOOL m = FALSE; if (v) { v->GetMute(&m); v->Release(); }
    return m;
}
void sys::SetMuted(bool m) {
    if (IAudioEndpointVolume* v = EndpointVolume(eRender)) { v->SetMute(m, nullptr); v->Release(); }
}
float sys::InputVolume() {
    IAudioEndpointVolume* v = EndpointVolume(eCapture);
    float f = -1; if (v) { v->GetMasterVolumeLevelScalar(&f); v->Release(); }
    return f;
}
void sys::SetInputVolume(float f) {
    if (IAudioEndpointVolume* v = EndpointVolume(eCapture)) { v->SetMasterVolumeLevelScalar(std::clamp(f, 0.f, 1.f), nullptr); v->Release(); }
}
std::vector<sys::AudioDev> sys::AudioDevices(bool output) {
    std::vector<AudioDev> r;
    auto e = Enumerator();
    if (!e) return r;
    EDataFlow flow = output ? eRender : eCapture;
    std::wstring def;
    {
        Com<IMMDevice> d;
        if (SUCCEEDED(e->GetDefaultAudioEndpoint(flow, eConsole, &d))) { LPWSTR id = nullptr; d->GetId(&id); if (id) { def = id; CoTaskMemFree(id); } }
    }
    Com<IMMDeviceCollection> col;
    if (FAILED(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return r;
    UINT n = 0; col->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        Com<IMMDevice> d;
        if (FAILED(col->Item(i, &d))) continue;
        LPWSTR id = nullptr; d->GetId(&id);
        Com<IPropertyStore> ps;
        std::wstring name;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) name = pv.pwszVal;
            PropVariantClear(&pv);
        }
        if (id) { r.push_back({ id, name, def == id }); CoTaskMemFree(id); }
    }
    return r;
}
bool sys::SetDefaultAudio(const std::wstring& id) {
    Com<IPolicyConfig> pc;
    if (FAILED(CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig), (void**)&pc))) return false;
    bool ok = SUCCEEDED(pc->SetDefaultEndpoint(id.c_str(), eConsole));
    pc->SetDefaultEndpoint(id.c_str(), eMultimedia);
    pc->SetDefaultEndpoint(id.c_str(), eCommunications);
    return ok;
}

// ================= Wi-Fi
bool sys::WifiPresent() { GUID g; return WlanIf(g); }

std::wstring sys::WifiCurrent() {
    GUID g; if (!WlanIf(g)) return L"";
    DWORD sz = 0; PWLAN_CONNECTION_ATTRIBUTES ca = nullptr; WLAN_OPCODE_VALUE_TYPE t;
    if (WlanQueryInterface(Wlan(), &g, wlan_intf_opcode_current_connection, nullptr, &sz, (PVOID*)&ca, &t) != ERROR_SUCCESS || !ca) return L"";
    std::wstring s = ca->isState == wlan_interface_state_connected ? SsidText(ca->wlanAssociationAttributes.dot11Ssid) : L"";
    WlanFreeMemory(ca);
    return s;
}

std::vector<sys::WifiNet> sys::WifiNetworks() {
    std::vector<WifiNet> r;
    GUID g; if (!WlanIf(g)) return r;
    static DWORD lastScan = 0;
    if (GetTickCount() - lastScan > 8000) { lastScan = GetTickCount(); WlanScan(Wlan(), &g, nullptr, nullptr, nullptr); }  // resultado chega na próxima leitura
    PWLAN_AVAILABLE_NETWORK_LIST list = nullptr;
    if (WlanGetAvailableNetworkList(Wlan(), &g, 0, nullptr, &list) != ERROR_SUCCESS || !list) return r;
    for (DWORD i = 0; i < list->dwNumberOfItems; i++) {
        auto& n = list->Network[i];
        std::wstring ssid = SsidText(n.dot11Ssid);
        if (ssid.empty()) continue;
        auto it = std::find_if(r.begin(), r.end(), [&](const WifiNet& w) { return w.ssid == ssid; });
        bool connected = (n.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0;
        bool known = (n.dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) != 0;
        if (known) g_wifiProfile[ssid] = n.strProfileName;
        g_wifiAuth[ssid] = n.dot11DefaultAuthAlgorithm;
        if (it != r.end()) {  // a mesma rede aparece com e sem perfil: junta
            it->connected |= connected; it->known |= known; it->signal = std::max<int>(it->signal, n.wlanSignalQuality);
            continue;
        }
        r.push_back({ ssid, (int)n.wlanSignalQuality, n.bSecurityEnabled != FALSE, known, connected });
    }
    WlanFreeMemory(list);
    std::sort(r.begin(), r.end(), [](const WifiNet& a, const WifiNet& b) { return a.signal > b.signal; });
    return r;
}

bool sys::WifiConnect(const std::wstring& ssid, const std::wstring& password, bool secure) {
    GUID g; if (!WlanIf(g)) return false;
    std::wstring profile;
    auto known = g_wifiProfile.find(ssid);
    if (known != g_wifiProfile.end() && password.empty()) profile = known->second;
    else {
        DOT11_AUTH_ALGORITHM a = g_wifiAuth.count(ssid) ? g_wifiAuth[ssid] : DOT11_AUTH_ALGO_RSNA_PSK;
        std::wstring auth = !secure ? L"open" : a == DOT11_AUTH_ALGO_WPA3_SAE ? L"WPA3SAE" : a == DOT11_AUTH_ALGO_WPA_PSK ? L"WPAPSK" : L"WPA2PSK";
        std::wstring enc = secure ? L"AES" : L"none";
        std::wstring x = L"<?xml version=\"1.0\"?><WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\"><name>" + XmlEscape(ssid) +
                         L"</name><SSIDConfig><SSID><name>" + XmlEscape(ssid) + L"</name></SSID></SSIDConfig><connectionType>ESS</connectionType>"
                         L"<connectionMode>auto</connectionMode><MSM><security><authEncryption><authentication>" + auth +
                         L"</authentication><encryption>" + enc + L"</encryption><useOneX>false</useOneX></authEncryption>";
        if (secure) x += L"<sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>" + XmlEscape(password) + L"</keyMaterial></sharedKey>";
        x += L"</security></MSM></WLANProfile>";
        DWORD reason = 0;
        if (WlanSetProfile(Wlan(), &g, 0, x.c_str(), nullptr, TRUE, nullptr, &reason) != ERROR_SUCCESS) return false;
        profile = ssid;
    }
    WLAN_CONNECTION_PARAMETERS p{};
    p.wlanConnectionMode = wlan_connection_mode_profile;
    p.strProfile = profile.c_str();
    p.dot11BssType = dot11_BSS_type_infrastructure;
    return WlanConnect(Wlan(), &g, &p, nullptr) == ERROR_SUCCESS;
}

void sys::WifiDisconnect() {
    GUID g; if (WlanIf(g)) WlanDisconnect(Wlan(), &g, nullptr);
}

// ================= Bluetooth
std::vector<sys::BtDev> sys::BluetoothDevices() {
    std::vector<BtDev> r;
    BLUETOOTH_DEVICE_SEARCH_PARAMS sp{ sizeof(sp) };
    sp.fReturnAuthenticated = TRUE; sp.fReturnRemembered = TRUE; sp.fReturnConnected = TRUE;
    BLUETOOTH_DEVICE_INFO di{ sizeof(di) };
    HBLUETOOTH_DEVICE_FIND f = BluetoothFindFirstDevice(&sp, &di);
    if (!f) return r;
    do {
        if (di.fRemembered || di.fAuthenticated) r.push_back({ di.szName, di.fConnected != FALSE, di.ulClassofDevice });
        di.dwSize = sizeof(di);
    } while (BluetoothFindNextDevice(f, &di));
    BluetoothFindDeviceClose(f);
    std::sort(r.begin(), r.end(), [](const BtDev& a, const BtDev& b) { return a.connected > b.connected; });
    return r;
}

// ================= Rede
std::vector<sys::NetIf> sys::Interfaces() {
    std::vector<NetIf> r;
    ULONG sz = 32 * 1024;
    std::vector<BYTE> buf(sz);
    auto* a = (IP_ADAPTER_ADDRESSES*)buf.data();
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, a, &sz) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(sz); a = (IP_ADAPTER_ADDRESSES*)buf.data();
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, a, &sz) != NO_ERROR) return r;
    }
    for (; a; a = a->Next) {
        if (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211) continue;
        std::wstring desc = a->Description ? a->Description : L"";
        if (desc.find(L"Virtual") != std::wstring::npos || desc.find(L"Hyper-V") != std::wstring::npos || desc.find(L"Bluetooth") != std::wstring::npos ||
            desc.find(L"VPN") != std::wstring::npos || desc.find(L"TAP") != std::wstring::npos || desc.find(L"WAN Miniport") != std::wstring::npos) continue;
        NetIf n;
        n.name = a->FriendlyName ? a->FriendlyName : desc;
        n.kind = a->IfType == IF_TYPE_IEEE80211 ? L"Wi‑Fi" : L"Ethernet";
        n.up = a->OperStatus == IfOperStatusUp;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* s4 = (sockaddr_in*)u->Address.lpSockaddr;
            wchar_t ip[32];
            swprintf_s(ip, L"%u.%u.%u.%u", s4->sin_addr.S_un.S_un_b.s_b1, s4->sin_addr.S_un.S_un_b.s_b2, s4->sin_addr.S_un.S_un_b.s_b3, s4->sin_addr.S_un.S_un_b.s_b4);
            n.ip = ip;
            break;
        }
        r.push_back(n);
    }
    std::stable_sort(r.begin(), r.end(), [](const NetIf& x, const NetIf& y) { return x.up > y.up; });
    return r;
}

// ================= Energia
int sys::PowerMode() {
    using Fn = DWORD(WINAPI*)(GUID*);
    static Fn get = (Fn)GetProcAddress(DllSistema(L"powrprof.dll"), "PowerGetEffectiveOverlayScheme");
    GUID g{};
    if (!get || get(&g) != ERROR_SUCCESS) return 1;
    if (IsEqualGUID(g, MODE_SAVER)) return 0;
    if (IsEqualGUID(g, MODE_PERF)) return 2;
    return 1;
}
void sys::SetPowerMode(int m) {
    using Fn = DWORD(WINAPI*)(GUID);
    static Fn set = (Fn)GetProcAddress(DllSistema(L"powrprof.dll"), "PowerSetActiveOverlayScheme");
    if (set) set(m == 0 ? MODE_SAVER : m == 2 ? MODE_PERF : GUID{});
}
int sys::DisplayOffMinutes() { return ReadPower(SUB_VIDEO, VIDEO_OFF); }
void sys::SetDisplayOffMinutes(int m) { WritePower(SUB_VIDEO, VIDEO_OFF, m); }
int sys::SleepMinutes() { return ReadPower(SUB_SLEEP, SLEEP_IDLE); }
void sys::SetSleepMinutes(int m) { WritePower(SUB_SLEEP, SLEEP_IDLE, m); }
bool sys::HasBattery(int* percent, bool* charging) {
    SYSTEM_POWER_STATUS s{};
    if (!GetSystemPowerStatus(&s) || (s.BatteryFlag & 128) || s.BatteryFlag == 255) return false;
    if (percent) *percent = s.BatteryLifePercent;
    if (charging) *charging = (s.BatteryFlag & 8) != 0;
    return true;
}

// ================= Telas
float sys::Brightness() {
    if (g_bright == -2) {
        g_bright = -1;
        std::thread([] {
            WithMonitor([](HANDLE h) {
                DWORD mn = 0, cur = 0, mx = 100;
                if (GetMonitorBrightness(h, &mn, &cur, &mx) && mx > mn) g_bright = (float)(cur - mn) / (mx - mn);
            });
            if (g_bright >= 0) aj::Rebuild();
        }).detach();
    }
    return g_bright;
}
void sys::SetBrightness(float v) {
    g_bright = v;
    g_brightWant = std::clamp(v, 0.f, 1.f);
    if (!g_brightBusy.exchange(true)) std::thread(BrightWorker).detach();
}
std::vector<sys::Mode> sys::Resolutions() {
    std::vector<Mode> r;
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++) {
        if (dm.dmBitsPerPel != 32) continue;
        Mode m{ (int)dm.dmPelsWidth, (int)dm.dmPelsHeight };
        if (m.w < 1024) continue;
        if (std::none_of(r.begin(), r.end(), [&](const Mode& x) { return x.w == m.w && x.h == m.h; })) r.push_back(m);
    }
    std::sort(r.begin(), r.end(), [](const Mode& a, const Mode& b) { return a.w != b.w ? a.w > b.w : a.h > b.h; });
    return r;
}
sys::Mode sys::CurrentResolution() {
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm);
    return { (int)dm.dmPelsWidth, (int)dm.dmPelsHeight };
}
bool sys::SetResolution(Mode m) {
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm);
    dm.dmPelsWidth = m.w; dm.dmPelsHeight = m.h;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    return ChangeDisplaySettingsExW(nullptr, &dm, nullptr, CDS_UPDATEREGISTRY, nullptr) == DISP_CHANGE_SUCCESSFUL;
}
int sys::RefreshRate() {
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm);
    return (int)dm.dmDisplayFrequency;
}
std::vector<int> sys::RefreshRates() {
    Mode cur = CurrentResolution();
    std::vector<int> r;
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++)
        if ((int)dm.dmPelsWidth == cur.w && (int)dm.dmPelsHeight == cur.h && dm.dmBitsPerPel == 32 && dm.dmDisplayFrequency > 1 &&
            std::find(r.begin(), r.end(), (int)dm.dmDisplayFrequency) == r.end())
            r.push_back((int)dm.dmDisplayFrequency);
    std::sort(r.rbegin(), r.rend());
    return r;
}
bool sys::SetRefreshRate(int hz) {
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm);
    dm.dmDisplayFrequency = hz;
    dm.dmFields = DM_DISPLAYFREQUENCY;
    return ChangeDisplaySettingsExW(nullptr, &dm, nullptr, CDS_UPDATEREGISTRY, nullptr) == DISP_CHANGE_SUCCESSFUL;
}
float sys::ScaleFactor() { return GetDpiForSystem() / 96.f; }

// ================= Imagem de fundo
std::wstring sys::Wallpaper() {
    Com<IDesktopWallpaper> dw;
    if (FAILED(CoCreateInstance(__uuidof(DesktopWallpaper), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dw)))) {
        // sem o Explorador (Shell com explorer=0) a interface não existe: o registro tem o mesmo caminho
        wchar_t buf[1024]; DWORD cb = sizeof(buf);
        return RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WallPaper", RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS ? std::wstring(buf) : std::wstring();
    }
    LPWSTR mon = nullptr, path = nullptr;
    std::wstring r;
    if (SUCCEEDED(dw->GetMonitorDevicePathAt(0, &mon)) && SUCCEEDED(dw->GetWallpaper(mon, &path)) && path) r = path;
    if (mon) CoTaskMemFree(mon);
    if (path) CoTaskMemFree(path);
    return r;
}
bool sys::SetWallpaper(const std::wstring& p) {
    // avisa o Shell primeiro: a área de trabalho dele mostra o novo na hora (o Explorador demora para gravar)
    if (HWND sh = FindWindowW(L"ShellMensagens", nullptr)) {
        COPYDATASTRUCT cd{ 0x4C504150 /*'PAPL'*/, (DWORD)((p.size() + 1) * sizeof(wchar_t)), (void*)p.c_str() };
        DWORD_PTR r = 0;
        SendMessageTimeoutW(sh, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 300, &r);
    }
    Com<IDesktopWallpaper> dw;
    if (FAILED(CoCreateInstance(__uuidof(DesktopWallpaper), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dw))))  // sem o Explorador: o jeito clássico
        return SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (void*)p.c_str(), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE) != FALSE;
    return SUCCEEDED(dw->SetWallpaper(nullptr, p.c_str()));
}
// modo de ajuste: DESKTOP_WALLPAPER_POSITION <-> WallpaperStyle/TileWallpaper do registro (o que o Explorador grava)
int sys::WallpaperFit() {
    Com<IDesktopWallpaper> dw;
    DESKTOP_WALLPAPER_POSITION pos = DWPOS_FILL;
    if (SUCCEEDED(CoCreateInstance(__uuidof(DesktopWallpaper), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dw)))) { dw->GetPosition(&pos); return (int)pos; }
    wchar_t buf[16]; DWORD cb = sizeof(buf);
    int style = 10, tile = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WallpaperStyle", RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS) style = _wtoi(buf);
    cb = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"TileWallpaper", RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS) tile = _wtoi(buf);
    switch (style) { case 0: return tile ? DWPOS_TILE : DWPOS_CENTER; case 2: return DWPOS_STRETCH; case 6: return DWPOS_FIT; case 22: return DWPOS_SPAN; default: return DWPOS_FILL; }
}
void sys::SetWallpaperFit(int pos) {
    Com<IDesktopWallpaper> dw;
    if (SUCCEEDED(CoCreateInstance(__uuidof(DesktopWallpaper), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dw)))) { dw->SetPosition((DESKTOP_WALLPAPER_POSITION)pos); return; }
    const wchar_t* style = L"10"; const wchar_t* tile = L"0";
    switch (pos) { case DWPOS_CENTER: style = L"0"; break; case DWPOS_TILE: style = L"0"; tile = L"1"; break; case DWPOS_STRETCH: style = L"2"; break;
                   case DWPOS_FIT: style = L"6"; break; case DWPOS_SPAN: style = L"22"; break; }
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WallpaperStyle", REG_SZ, style, (DWORD)(wcslen(style) + 1) * 2);
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"TileWallpaper", REG_SZ, tile, (DWORD)(wcslen(tile) + 1) * 2);
    std::wstring cur = sys::Wallpaper();  // reaplica para avisar (a mesa do Shell relê o registro)
    SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (void*)cur.c_str(), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
}
std::vector<std::wstring> sys::WallpaperChoices() {
    std::vector<std::wstring> r;
    auto scan = [&](const std::wstring& dir, int depth, auto&& self) -> void {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring full = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth > 0) self(full, depth - 1, self); continue; }
            std::wstring ext = PathFindExtensionW(n.c_str());
            for (auto& c : ext) c = towlower(c);
            if ((ext == L".jpg" || ext == L".jpeg" || ext == L".png") && fd.nFileSizeLow > 100 * 1024) r.push_back(full);
        } while (FindNextFileW(h, &fd) && r.size() < 120);
        FindClose(h);
    };
    // só pastas de papel de parede (não as fotos e capturas de tela da pasta Imagens) e a do fundo atual
    std::wstring cur = Wallpaper();
    if (!cur.empty()) scan(cur.substr(0, cur.find_last_of(L'\\')), 0, scan);
    PWSTR pics = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics))) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((std::wstring(pics) + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                std::wstring n = fd.cFileName;
                for (auto& ch : n) ch = towlower(ch);
                if (n.find(L"wallpaper") != std::wstring::npos || n.find(L"papel") != std::wstring::npos || n.find(L"papéis") != std::wstring::npos ||
                    n.find(L"fundo") != std::wstring::npos || n.find(L"background") != std::wstring::npos)
                    scan(std::wstring(pics) + L"\\" + fd.cFileName, 1, scan);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        CoTaskMemFree(pics);
    }
    std::sort(r.begin(), r.end());
    r.erase(std::unique(r.begin(), r.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }), r.end());
    wchar_t win[MAX_PATH]; GetWindowsDirectoryW(win, MAX_PATH);
    scan(std::wstring(win) + L"\\Web\\Wallpaper", 1, scan);
    return r;
}

// ================= Sobre
const sys::Info& sys::About() {
    static Info i;
    static bool done = false;
    if (done) return i;
    done = true;
    wchar_t buf[256]; DWORD n = 256;
    if (GetComputerNameExW(ComputerNameDnsHostname, buf, &n)) i.computer = buf;
    i.cpu = Trim(RegStr(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString"));
    DISPLAY_DEVICEW dd{ sizeof(dd) };
    for (DWORD k = 0; EnumDisplayDevicesW(nullptr, k, &dd, 0); k++)
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) { i.gpu = dd.DeviceString; break; }
    ULONGLONG kb = 0;
    if (GetPhysicallyInstalledSystemMemory(&kb)) { swprintf_s(buf, L"%llu GB", (kb + 512 * 1024) / (1024 * 1024)); i.ram = buf; }
    const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::wstring product = RegStr(HKEY_LOCAL_MACHINE, cv, L"ProductName");
    int build = _wtoi(RegStr(HKEY_LOCAL_MACHINE, cv, L"CurrentBuild").c_str());
    if (build >= 22000 && product.find(L"Windows 10") == 0) product.replace(0, 10, L"Windows 11");  // o registro ainda diz 10
    i.windows = product + L" " + RegStr(HKEY_LOCAL_MACHINE, cv, L"DisplayVersion");
    swprintf_s(buf, L"%d.%lu", build, RegDword(HKEY_LOCAL_MACHINE, cv, L"UBR", 0));
    i.build = buf;
    std::wstring maker = RegStr(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemManufacturer");
    std::wstring model = RegStr(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemProductName");
    if (model.empty() || model.find(L"System Product") != std::wstring::npos || model.find(L"To be filled") != std::wstring::npos) {
        maker = RegStr(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"BaseBoardManufacturer");
        model = RegStr(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"BaseBoardProduct");
    }
    i.model = Trim(maker + L" " + model);
    n = 256; if (GetUserNameW(buf, &n)) i.user = buf;
    n = 256; if (GetUserNameExW(NameDisplay, buf, &n) && buf[0]) i.fullName = buf; else i.fullName = i.user;
    return i;
}

std::vector<sys::Disk> sys::Disks() {
    std::vector<Disk> r;
    DWORD mask = GetLogicalDrives();
    for (int k = 0; k < 26; k++) {
        if (!(mask & (1u << k))) continue;
        std::wstring root = std::wstring(1, (wchar_t)(L'A' + k)) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;
        ULARGE_INTEGER fr{}, tot{};
        if (!GetDiskFreeSpaceExW(root.c_str(), &fr, &tot, nullptr)) continue;
        wchar_t label[MAX_PATH] = {};
        GetVolumeInformationW(root.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
        r.push_back({ root, label, tot.QuadPart, fr.QuadPart });
    }
    return r;
}

// ================= Itens de início
static const wchar_t* RUN = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* APPROVED = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

std::vector<sys::Startup> sys::StartupItems() {
    std::vector<Startup> r;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN, 0, KEY_READ, &k) != ERROR_SUCCESS) return r;
    wchar_t name[256]; BYTE data[2048];
    for (DWORD i = 0;; i++) {
        DWORD nn = 256, cb = sizeof(data), type = 0;
        if (RegEnumValueW(k, i, name, &nn, nullptr, &type, data, &cb) != ERROR_SUCCESS) break;
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
        Startup s{ name, (wchar_t*)data, true };
        BYTE st[12]; DWORD sc = sizeof(st);
        if (RegGetValueW(HKEY_CURRENT_USER, APPROVED, name, RRF_RT_REG_BINARY, nullptr, st, &sc) == ERROR_SUCCESS && sc >= 1) s.enabled = (st[0] & 1) == 0;
        r.push_back(s);
    }
    RegCloseKey(k);
    std::sort(r.begin(), r.end(), [](const Startup& a, const Startup& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return r;
}
void sys::SetStartupEnabled(const std::wstring& name, bool on) {
    BYTE st[12] = { (BYTE)(on ? 2 : 3) };
    if (!on) { FILETIME ft; GetSystemTimeAsFileTime(&ft); memcpy(st + 4, &ft, 8); }
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, APPROVED, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(k, name.c_str(), 0, REG_BINARY, st, sizeof(st));
    RegCloseKey(k);
}

// ================= Aparência
static const wchar_t* PERSONALIZE = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
int sys::Theme() { return RegDword(HKEY_CURRENT_USER, PERSONALIZE, L"AppsUseLightTheme", 1) ? 0 : 1; }
void sys::SetTheme(int t) {
    DWORD light = t == 0 ? 1 : 0;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, PERSONALIZE, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    RegSetValueExW(k, L"AppsUseLightTheme", 0, REG_DWORD, (BYTE*)&light, sizeof(light));
    RegSetValueExW(k, L"SystemUsesLightTheme", 0, REG_DWORD, (BYTE*)&light, sizeof(light));
    RegCloseKey(k);
    std::thread([] { SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet", SMTO_ABORTIFHUNG, 200, nullptr); }).detach();
}

// ================= Teclado, mouse, janelas
int sys::KeyRepeat() { DWORD v = 31; SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &v, 0); return (int)v; }
void sys::SetKeyRepeat(int v) { SystemParametersInfoW(SPI_SETKEYBOARDSPEED, std::clamp(v, 0, 31), nullptr, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE); }
int sys::KeyDelay() { int v = 1; SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &v, 0); return v; }
void sys::SetKeyDelay(int v) { SystemParametersInfoW(SPI_SETKEYBOARDDELAY, std::clamp(v, 0, 3), nullptr, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE); }
int sys::MouseSpeed() { int v = 10; SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &v, 0); return v; }
void sys::SetMouseSpeed(int v) { SystemParametersInfoW(SPI_SETMOUSESPEED, 0, (PVOID)(INT_PTR)std::clamp(v, 1, 20), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE); }
int sys::DoubleClickMs() { return (int)GetDoubleClickTime(); }
void sys::SetDoubleClickMs(int ms) { SystemParametersInfoW(SPI_SETDOUBLECLICKTIME, std::clamp(ms, 200, 900), nullptr, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE); }
bool sys::ButtonsSwapped() { return GetSystemMetrics(SM_SWAPBUTTON) != 0; }
void sys::SetButtonsSwapped(bool s) { SystemParametersInfoW(SPI_SETMOUSEBUTTONSWAP, s, nullptr, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE); }
int sys::WheelLines() { UINT v = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &v, 0); return (int)v; }
void sys::SetWheelLines(int n) { SystemParametersInfoW(SPI_SETWHEELSCROLLLINES, std::clamp(n, 1, 20), nullptr, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE); }
// o valor gravado (com o Shell rodando o do momento fica desligado: quem encaixa é o Shell, ver src\shell\encaixe.cpp)
bool sys::SnapEnabled() {
    wchar_t v[8] = L"1"; DWORD n = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WindowArrangementActive", RRF_RT_REG_SZ, nullptr, v, &n);
    return v[0] != L'0';
}
void sys::SetSnapEnabled(bool on) {
    SystemParametersInfoW(0x0083 /*SPI_SETWINARRANGING*/, on, nullptr, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    if (on && FindWindowW(L"ShellMensagens", nullptr)) SystemParametersInfoW(0x0083, FALSE, nullptr, 0);  // gravado ligado; na sessão o Shell encaixa
}

// ================= Usuários e impressoras
std::vector<std::wstring> sys::LocalUsers() {
    std::vector<std::wstring> r;
    LPBYTE buf = nullptr; DWORD read = 0, total = 0;
    if (NetUserEnum(nullptr, 1, FILTER_NORMAL_ACCOUNT, &buf, MAX_PREFERRED_LENGTH, &read, &total, nullptr) == NERR_Success && buf) {
        auto* u = (USER_INFO_1*)buf;
        for (DWORD i = 0; i < read; i++)
            if (!(u[i].usri1_flags & UF_ACCOUNTDISABLE)) r.push_back(u[i].usri1_name);
        NetApiBufferFree(buf);
    }
    return r;
}
std::vector<std::wstring> sys::Printers() {
    std::vector<std::wstring> r;
    DWORD need = 0, n = 0;
    EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4, nullptr, 0, &need, &n);
    if (!need) return r;
    std::vector<BYTE> buf(need);
    if (!EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4, buf.data(), need, &need, &n)) return r;
    auto* p = (PRINTER_INFO_4W*)buf.data();
    for (DWORD i = 0; i < n; i++) r.push_back(p[i].pPrinterName);
    return r;
}
std::wstring sys::DefaultPrinter() {
    wchar_t buf[512]; DWORD n = 512;
    return GetDefaultPrinterW(buf, &n) ? buf : L"";
}
void sys::SetPrinterDefault(const std::wstring& name) { SetDefaultPrinterW(name.c_str()); }

ID2D1Bitmap* sys::AccountPicture() {
    static std::wstring path;
    static bool looked = false;
    if (!looked) {
        looked = true;
        HANDLE tok;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
            BYTE buf[256]; DWORD n = 0;
            if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &n)) {
                LPWSTR sid = nullptr;
                if (ConvertSidToStringSidW(((TOKEN_USER*)buf)->User.Sid, &sid)) {
                    std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AccountPicture\\Users\\") + sid;
                    for (auto img : { L"Image448", L"Image240", L"Image192", L"Image208" }) {
                        path = RegStr(HKEY_LOCAL_MACHINE, key.c_str(), img);
                        if (!path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) break;
                        path.clear();
                    }
                    LocalFree(sid);
                }
            }
            CloseHandle(tok);
        }
    }
    return path.empty() ? nullptr : aj::BitmapFromFile(path, 128);
}
