// Model definitions: everything the app needs to know about one laptop model.
// The built-in definition is the verified Acer Swift X 16 SFX16-61G (BIOS 1.56).
// Definitions created by Identify are stored as .ini files. A file can never mark itself as
// confirmed: confirmation is stored per laptop, keyed by ModelFingerprint(), and is only set
// after the app has read the live EC once (with the user's consent).
#pragma once
#include "FactoryTables.h"

#include <filesystem>
#include <string>
#include <vector>

namespace acer {

struct ProfileDef {
    std::wstring name;
    unsigned code = 0;  // vendor (AcerSense/WMI) profile code
    std::uint32_t accent = 0x60A5FA;
    Table factory{};
};

struct ModelDef {
    std::wstring title;                                // shown in the UI
    std::wstring manufacturer, product, board, bios;   // substrings matched against SMBIOS
    std::wstring source = L"builtin";                  // "builtin" or the .ini path
    std::uint8_t readCmd = 0x92, writeCmd = 0x93;      // EC extended-RAM commands (ports 0x66/0x62)
    std::uint16_t tableBase = 0x0800;                  // active fan table in EC RAM
    bool confirmed = true;                             // builtin: verified; files: from local settings only
    std::wstring backend = L"acer_table";              // "acer_table" or "nbfc"
    std::wstring nbfcConfig;                           // NBFC backend: imported config file
    std::vector<ProfileDef> profiles;
};

inline std::uint32_t DefaultAccent(const std::wstring& name, std::size_t index) {
    if (name == L"Silent") return 0x2DD4BF;
    if (name == L"Normal") return 0x60A5FA;
    if (name == L"Performance") return 0xF59E0B;
    if (name == L"Turbo") return 0xF43F5E;
    static const std::uint32_t others[] = {0x34D399, 0xE879F9, 0xFB923C, 0x38BDF8};
    return others[index % 4];
}

inline ModelDef BuiltinModel() {
    ModelDef m;
    m.title = L"Acer Swift X 16 · SFX16-61G";
    m.manufacturer = L"Acer";
    m.product = L"SFX16-61G";
    m.board = L"Ariya_PEH";
    m.bios = L"1.56";
    const wchar_t* names[4] = {L"Silent", L"Normal", L"Performance", L"Turbo"};
    const unsigned codes[4] = {2, 0, 3, 4};
    for (std::size_t i = 0; i < 4; ++i)
        m.profiles.push_back({names[i], codes[i], DefaultAccent(names[i], i), kFactoryTables[i]});
    return m;
}

inline bool MatchesMachine(const ModelDef& m) {
    auto has = [](const wchar_t* key, const std::wstring& needle) {
        return needle.empty() || BiosString(key).find(needle) != std::wstring::npos;
    };
    const bool usable = m.backend == L"nbfc" ? GetFileAttributesW(m.nbfcConfig.c_str()) != INVALID_FILE_ATTRIBUTES
                                               : !m.profiles.empty();
    return usable && has(L"SystemManufacturer", m.manufacturer) &&
           has(L"SystemProductName", m.product) && has(L"BaseBoardProduct", m.board) &&
           has(L"BIOSVersion", m.bios);
}

// ---------------------------------------------------------------------------
// .ini format
//   [model]   title, manufacturer, product, board, bios, read_cmd, write_cmd,
//             table_base, confirmed, profiles
//   [profileN] name, code, accent, table (240 hex digits)
// ---------------------------------------------------------------------------
inline std::wstring IniGet(const std::wstring& file, const wchar_t* section, const wchar_t* key) {
    wchar_t buf[1024] = {};
    GetPrivateProfileStringW(section, key, L"", buf, 1024, file.c_str());
    return buf;
}

inline unsigned long ParseNumber(const std::wstring& s) { return std::wcstoul(s.c_str(), nullptr, 0); }

inline std::wstring TableToHex(const Table& t) {
    static const wchar_t* digits = L"0123456789abcdef";
    std::wstring s;
    for (std::uint8_t b : t) {
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

inline bool HexToTable(const std::wstring& s, Table& t) {
    if (s.size() != kTableSize * 2) return false;
    for (std::size_t i = 0; i < kTableSize; ++i) {
        wchar_t pair[3] = {s[2 * i], s[2 * i + 1], 0};
        wchar_t* end = nullptr;
        const unsigned long v = std::wcstoul(pair, &end, 16);
        if (end != pair + 2) return false;
        t[i] = static_cast<std::uint8_t>(v);
    }
    return true;
}

inline bool LoadModel(const std::filesystem::path& path, ModelDef& m) {
    const std::wstring f = path.wstring();
    ModelDef r;
    r.source = f;
    r.title = IniGet(f, L"model", L"title");
    r.manufacturer = IniGet(f, L"model", L"manufacturer");
    r.product = IniGet(f, L"model", L"product");
    r.board = IniGet(f, L"model", L"board");
    r.bios = IniGet(f, L"model", L"bios");
    r.readCmd = static_cast<std::uint8_t>(ParseNumber(IniGet(f, L"model", L"read_cmd")));
    r.writeCmd = static_cast<std::uint8_t>(ParseNumber(IniGet(f, L"model", L"write_cmd")));
    r.tableBase = static_cast<std::uint16_t>(ParseNumber(IniGet(f, L"model", L"table_base")));
    r.confirmed = false;  // never trusted from the file; see ModelFingerprint
    const std::wstring backend = IniGet(f, L"model", L"backend");
    if (!backend.empty()) r.backend = backend;
    if (r.backend == L"nbfc") {
        r.nbfcConfig = IniGet(f, L"model", L"config");
        if (r.product.empty() || r.nbfcConfig.empty()) return false;
        if (r.title.empty()) r.title = r.manufacturer + L" " + r.product;
        m = r;
        return true;
    }
    const unsigned long count = ParseNumber(IniGet(f, L"model", L"profiles"));
    if (r.product.empty() || !r.readCmd || !r.writeCmd || !r.tableBase || count == 0 || count > 8) return false;
    for (unsigned long i = 0; i < count; ++i) {
        const std::wstring sec = L"profile" + std::to_wstring(i);
        ProfileDef p;
        p.name = IniGet(f, sec.c_str(), L"name");
        p.code = static_cast<unsigned>(ParseNumber(IniGet(f, sec.c_str(), L"code")));
        const std::wstring accent = IniGet(f, sec.c_str(), L"accent");
        p.accent = accent.empty() ? DefaultAccent(p.name, i) : static_cast<std::uint32_t>(std::wcstoul(accent.c_str(), nullptr, 16));
        if (p.name.empty() || !HexToTable(IniGet(f, sec.c_str(), L"table"), p.factory)) return false;
        r.profiles.push_back(p);
    }
    if (r.title.empty()) r.title = r.manufacturer + L" " + r.product;
    m = r;
    return true;
}

inline bool SaveModel(const std::filesystem::path& path, const ModelDef& m) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::remove(path, ec);
    const std::wstring f = path.wstring();
    auto put = [&](const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
        return WritePrivateProfileStringW(sec, key, v.c_str(), f.c_str()) != 0;
    };
    auto hex = [](unsigned v) {
        wchar_t b[16];
        swprintf_s(b, L"0x%X", v);
        return std::wstring(b);
    };
    bool ok = put(L"model", L"title", m.title) && put(L"model", L"manufacturer", m.manufacturer) &&
              put(L"model", L"product", m.product) && put(L"model", L"board", m.board) &&
              put(L"model", L"bios", m.bios) && put(L"model", L"read_cmd", hex(m.readCmd)) &&
              put(L"model", L"write_cmd", hex(m.writeCmd)) && put(L"model", L"table_base", hex(m.tableBase)) &&

              put(L"model", L"profiles", std::to_wstring(m.profiles.size())) && put(L"model", L"backend", m.backend) &&
              (m.nbfcConfig.empty() || put(L"model", L"config", m.nbfcConfig));
    for (std::size_t i = 0; ok && i < m.profiles.size(); ++i) {
        const std::wstring sec = L"profile" + std::to_wstring(i);
        const ProfileDef& p = m.profiles[i];
        wchar_t accent[16];
        swprintf_s(accent, L"%06X", p.accent);
        ok = put(sec.c_str(), L"name", p.name) && put(sec.c_str(), L"code", std::to_wstring(p.code)) &&
             put(sec.c_str(), L"accent", accent) && put(sec.c_str(), L"table", TableToHex(p.factory));
    }
    return ok;
}

// Stable fingerprint of everything that decides what the app writes: identity, EC commands,
// table address, profile mapping and tables (or the NBFC config file's bytes).
inline std::uint64_t Fnv1a(const void* data, std::size_t size, std::uint64_t h = 1469598103934665603ull) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
inline std::wstring ModelFingerprint(const ModelDef& m) {
    std::wstring id = m.backend + L"|" + m.manufacturer + L"|" + m.product + L"|" + m.board + L"|" + m.bios + L"|" +
                      std::to_wstring(m.readCmd) + L"|" + std::to_wstring(m.writeCmd) + L"|" + std::to_wstring(m.tableBase);
    for (const ProfileDef& p : m.profiles) id += L"|" + p.name + L":" + std::to_wstring(p.code) + L":" + TableToHex(p.factory);
    std::uint64_t h = Fnv1a(id.data(), id.size() * sizeof(wchar_t));
    if (m.backend == L"nbfc") {
        std::ifstream f(m.nbfcConfig, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(f)), {});
        h = Fnv1a(bytes.data(), bytes.size(), h);
    }
    wchar_t buf[17];
    swprintf_s(buf, L"%016llx", static_cast<unsigned long long>(h));
    return buf;
}

// Definition files live in "models" beside the exe (shipped) and in the user's
// data folder (created by Identify). A matching file wins over the built-in model.
inline bool FindModel(const std::vector<std::filesystem::path>& dirs, ModelDef& out) {
    for (const auto& dir : dirs) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            ModelDef m;
            if (entry.path().extension() == L".ini" && LoadModel(entry.path(), m) && MatchesMachine(m)) {
                out = m;
                return true;
            }
        }
    }
    const ModelDef builtin = BuiltinModel();
    if (MatchesMachine(builtin)) {
        out = builtin;
        return true;
    }
    return false;
}

}  // namespace acer
