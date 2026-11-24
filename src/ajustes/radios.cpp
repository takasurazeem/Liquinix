// Ligar e desligar Wi-Fi e Bluetooth (Windows.Devices.Radios), o mesmo que a Central de Ações faz.
// As chamadas são assíncronas: rodam numa thread própria e avisam a janela quando terminam.
#include "ajustes.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Radios.h>
#include <atomic>
#include <mutex>
#include <thread>

#pragma comment(lib, "windowsapp.lib")

using namespace winrt::Windows::Devices::Radios;
namespace wdr = winrt::Windows::Devices::Radios;

namespace {
std::mutex g_mx;
Radio g_radio[2]{ nullptr, nullptr };
std::atomic<int> g_state[2]{ -2, -2 };
std::atomic<bool> g_started{ false };

void Load() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try {
        RadioAccessStatus st = Radio::RequestAccessAsync().get();
        (void)st;
        auto all = Radio::GetRadiosAsync().get();
        int found[2] = { -1, -1 };
        for (auto const& r : all) {
            int k = r.Kind() == RadioKind::WiFi ? 0 : r.Kind() == RadioKind::Bluetooth ? 1 : -1;
            if (k < 0 || g_radio[k]) continue;
            {
                std::lock_guard<std::mutex> l(g_mx);
                g_radio[k] = r;
            }
            g_state[k] = r.State() == RadioState::On ? 1 : 0;
            found[k] = 1;
            r.StateChanged([k](Radio const& s, auto&&) {
                g_state[k] = s.State() == RadioState::On ? 1 : 0;
                aj::Rebuild();
            });
        }
        for (int k = 0; k < 2; k++) if (found[k] < 0) g_state[k] = -1;
    } catch (...) {
        g_state[0] = g_state[1] = -1;
    }
    aj::Rebuild();
}
}  // namespace

int sys::RadioState(Radio r) {
    if (!g_started.exchange(true)) std::thread(Load).detach();
    return g_state[r];
}

void sys::SetRadio(sys::Radio which, bool on) {
    g_state[which] = on ? 1 : 0;  // a chave já muda; o evento confirma
    std::thread([which, on] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        wdr::Radio r{ nullptr };
        {
            std::lock_guard<std::mutex> l(g_mx);
            r = g_radio[which];
        }
        if (!r) return;
        try {
            r.SetStateAsync(on ? wdr::RadioState::On : wdr::RadioState::Off).get();
        } catch (...) {}
        g_state[which] = r.State() == wdr::RadioState::On ? 1 : 0;
        aj::Rebuild();
    }).detach();
}
