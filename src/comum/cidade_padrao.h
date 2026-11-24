#pragma once
// Cidade padrão do Tempo (app Clima e widget da mesa) quando o ajustes.txt não tem tempoCidade: a do fuso horário
// do Windows. Antes era sempre Campinas, e quem é de fora do Brasil abria o Tempo no Brasil (Reddit, 20/11/2026).
// Fuso sem cidade na tabela: Londres (UTC).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>

namespace cidade {

struct Padrao {
    std::wstring nome = L"London", lat = L"51.5072", lon = L"-0.1276";
    Padrao() {
        static const struct { const wchar_t *tz, *nome, *lat, *lon; } T[] = {
            { L"E. South America Standard Time", L"Campinas", L"-22.9056", L"-47.0608" },
            { L"SA Eastern Standard Time", L"Fortaleza", L"-3.7319", L"-38.5267" },
            { L"Bahia Standard Time", L"Salvador", L"-12.9777", L"-38.5016" },
            { L"Tocantins Standard Time", L"Palmas", L"-10.1844", L"-48.3336" },
            { L"SA Western Standard Time", L"Manaus", L"-3.1190", L"-60.0217" },
            { L"Central Brazilian Standard Time", L"Cuiabá", L"-15.6014", L"-56.0979" },
            { L"Argentina Standard Time", L"Buenos Aires", L"-34.6037", L"-58.3816" },
            { L"Pacific SA Standard Time", L"Santiago", L"-33.4489", L"-70.6693" },
            { L"SA Pacific Standard Time", L"Bogotá", L"4.7110", L"-74.0721" },
            { L"Venezuela Standard Time", L"Caracas", L"10.4806", L"-66.9036" },
            { L"Central Standard Time (Mexico)", L"Ciudad de México", L"19.4326", L"-99.1332" },
            { L"Eastern Standard Time", L"New York", L"40.7128", L"-74.0060" },
            { L"Central Standard Time", L"Chicago", L"41.8781", L"-87.6298" },
            { L"Mountain Standard Time", L"Denver", L"39.7392", L"-104.9903" },
            { L"US Mountain Standard Time", L"Phoenix", L"33.4484", L"-112.0740" },
            { L"Pacific Standard Time", L"Los Angeles", L"34.0522", L"-118.2437" },
            { L"Alaskan Standard Time", L"Anchorage", L"61.2181", L"-149.9003" },
            { L"Hawaiian Standard Time", L"Honolulu", L"21.3069", L"-157.8583" },
            { L"Atlantic Standard Time", L"Halifax", L"44.6488", L"-63.5752" },
            { L"GMT Standard Time", L"London", L"51.5072", L"-0.1276" },
            { L"Greenwich Standard Time", L"Reykjavík", L"64.1466", L"-21.9426" },
            { L"W. Europe Standard Time", L"Berlin", L"52.5200", L"13.4050" },
            { L"Romance Standard Time", L"Paris", L"48.8566", L"2.3522" },
            { L"Central Europe Standard Time", L"Budapest", L"47.4979", L"19.0402" },
            { L"Central European Standard Time", L"Warszawa", L"52.2297", L"21.0122" },
            { L"GTB Standard Time", L"Athens", L"37.9838", L"23.7275" },
            { L"FLE Standard Time", L"Kyiv", L"50.4501", L"30.5234" },
            { L"E. Europe Standard Time", L"Chișinău", L"47.0105", L"28.8638" },
            { L"Turkey Standard Time", L"İstanbul", L"41.0082", L"28.9784" },
            { L"Russian Standard Time", L"Moscow", L"55.7558", L"37.6173" },
            { L"Israel Standard Time", L"Jerusalem", L"31.7683", L"35.2137" },
            { L"Egypt Standard Time", L"Cairo", L"30.0444", L"31.2357" },
            { L"South Africa Standard Time", L"Johannesburg", L"-26.2041", L"28.0473" },
            { L"W. Central Africa Standard Time", L"Lagos", L"6.5244", L"3.3792" },
            { L"E. Africa Standard Time", L"Nairobi", L"-1.2921", L"36.8219" },
            { L"Arab Standard Time", L"Riyadh", L"24.7136", L"46.6753" },
            { L"Arabian Standard Time", L"Dubai", L"25.2048", L"55.2708" },
            { L"Iran Standard Time", L"Tehran", L"35.6892", L"51.3890" },
            { L"Pakistan Standard Time", L"Karachi", L"24.8607", L"67.0011" },
            { L"India Standard Time", L"Mumbai", L"19.0760", L"72.8777" },
            { L"Bangladesh Standard Time", L"Dhaka", L"23.8103", L"90.4125" },
            { L"SE Asia Standard Time", L"Bangkok", L"13.7563", L"100.5018" },
            { L"Singapore Standard Time", L"Singapore", L"1.3521", L"103.8198" },
            { L"China Standard Time", L"Beijing", L"39.9042", L"116.4074" },
            { L"Taipei Standard Time", L"Taipei", L"25.0330", L"121.5654" },
            { L"Korea Standard Time", L"Seoul", L"37.5665", L"126.9780" },
            { L"Tokyo Standard Time", L"Tokyo", L"35.6762", L"139.6503" },
            { L"W. Australia Standard Time", L"Perth", L"-31.9505", L"115.8605" },
            { L"AUS Eastern Standard Time", L"Sydney", L"-33.8688", L"151.2093" },
            { L"E. Australia Standard Time", L"Brisbane", L"-27.4698", L"153.0251" },
            { L"New Zealand Standard Time", L"Auckland", L"-36.8485", L"174.7633" },
        };
        DYNAMIC_TIME_ZONE_INFORMATION d{};
        if (GetDynamicTimeZoneInformation(&d) == TIME_ZONE_ID_INVALID) return;
        for (auto& t : T) if (!wcscmp(d.TimeZoneKeyName, t.tz)) { nome = t.nome; lat = t.lat; lon = t.lon; return; }
    }
};

}  // namespace cidade
