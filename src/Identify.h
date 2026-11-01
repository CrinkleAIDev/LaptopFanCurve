// Identify: read-only discovery of a laptop's fan-control layout.
//   1. Fingerprint (no EC access): SMBIOS strings, DSDT EC regions/fields, vendor WMI classes.
//   2. BIOS scan: finds Acer-style 12 x 10-byte fan tables in a vendor BIOS update
//      (raw .fd/.bin, or an .exe/.zip unpacked with 7-Zip if it is installed).
//   3. Draft model definition for Acer laptops that expose APGeAction and whose BIOS
//      contains fan tables. Drafts start unconfirmed; the app confirms them against
//      the live EC (read only, with consent) before any write.
#pragma once
#include "ModelDef.h"
#include "Nbfc.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>

namespace acer::identify {

// ===========================================================================
// AML (DSDT) scanning: OperationRegions and named Field units
// ===========================================================================
struct Region {
    std::string name;
    std::uint8_t space = 0;  // 0 SystemMemory, 1 SystemIO, 3 EmbeddedControl, ...
    std::uint64_t base = 0, length = 0;
    bool known = false;  // base/length were constants
};
struct Field {
    std::string region, name;
    std::uint32_t bitOffset = 0, bitWidth = 0;
};
struct Aml {
    bool ok = false;
    bool hasEcDevice = false;
    std::vector<Region> regions;
    std::vector<Field> fields;
};

inline bool NameChar(std::uint8_t c, bool first) {
    return (c >= 'A' && c <= 'Z') || c == '_' || (!first && c >= '0' && c <= '9');
}
inline bool IsNameSeg(const std::uint8_t* p) {
    return NameChar(p[0], true) && NameChar(p[1], false) && NameChar(p[2], false) && NameChar(p[3], false);
}

// Returns false if the encoding is malformed.
inline bool PkgLength(const std::vector<std::uint8_t>& d, std::size_t& at, std::uint32_t& len) {
    if (at >= d.size()) return false;
    const std::uint8_t lead = d[at++];
    const int extra = lead >> 6;
    if (extra == 0) {
        len = lead & 0x3f;
        return true;
    }
    if (at + static_cast<std::size_t>(extra) > d.size()) return false;
    len = lead & 0x0f;
    for (int i = 0; i < extra; ++i) len |= static_cast<std::uint32_t>(d[at++]) << (4 + 8 * i);
    return true;
}

// Reads a NameString and returns its last segment.
inline bool NameString(const std::vector<std::uint8_t>& d, std::size_t& at, std::string& last) {
    while (at < d.size() && (d[at] == '\\' || d[at] == '^')) ++at;
    if (at >= d.size()) return false;
    int segs = 1;
    if (d[at] == 0x2e) { segs = 2; ++at; }
    else if (d[at] == 0x2f) { if (at + 1 >= d.size()) return false; segs = d[at + 1]; at += 2; }
    else if (d[at] == 0x00) { ++at; last.clear(); return true; }
    if (segs < 1 || at + 4u * static_cast<unsigned>(segs) > d.size()) return false;
    for (int i = 0; i < segs; ++i) {
        if (!IsNameSeg(&d[at])) return false;
        last.assign(reinterpret_cast<const char*>(&d[at]), 4);
        at += 4;
    }
    return true;
}

inline bool IntegerConst(const std::vector<std::uint8_t>& d, std::size_t& at, std::uint64_t& v) {
    if (at >= d.size()) return false;
    const std::uint8_t op = d[at++];
    int n = 0;
    switch (op) {
    case 0x00: v = 0; return true;
    case 0x01: v = 1; return true;
    case 0xff: v = ~0ull; return true;
    case 0x0a: n = 1; break;
    case 0x0b: n = 2; break;
    case 0x0c: n = 4; break;
    case 0x0e: n = 8; break;
    default: return false;
    }
    if (at + static_cast<std::size_t>(n) > d.size()) return false;
    v = 0;
    for (int i = 0; i < n; ++i) v |= static_cast<std::uint64_t>(d[at + static_cast<std::size_t>(i)]) << (8 * i);
    at += static_cast<std::size_t>(n);
    return true;
}

inline Aml ParseAml(const std::vector<std::uint8_t>& d) {
    Aml r;
    if (d.size() < 36) return r;
    r.ok = true;
    static const std::uint8_t pnp0c09[] = {0x0c, 0x41, 0xd0, 0x0c, 0x09};
    r.hasEcDevice = std::search(d.begin(), d.end(), std::begin(pnp0c09), std::end(pnp0c09)) != d.end();
    for (std::size_t i = 36; i + 8 < d.size(); ++i) {
        if (d[i] != 0x5b) continue;
        if (d[i + 1] == 0x80) {  // OperationRegion
            std::size_t at = i + 2;
            Region reg;
            if (!NameString(d, at, reg.name) || reg.name.empty() || at >= d.size()) continue;
            reg.space = d[at++];
            if (reg.space > 0x0a && reg.space < 0x80) continue;
            reg.known = IntegerConst(d, at, reg.base) && IntegerConst(d, at, reg.length);
            r.regions.push_back(reg);
        } else if (d[i + 1] == 0x81) {  // Field
            std::size_t at = i + 2;
            const std::size_t start = at;
            std::uint32_t len = 0;
            std::string region;
            if (!PkgLength(d, at, len) || start + len > d.size() || !NameString(d, at, region) || region.empty())
                continue;
            const std::size_t end = start + len;
            ++at;  // field flags
            std::uint32_t bit = 0;
            std::vector<Field> found;
            bool good = true;
            while (at < end && good) {
                const std::uint8_t c = d[at];
                std::uint32_t width = 0;
                if (c == 0x00) {
                    ++at;
                    good = PkgLength(d, at, width);
                    bit += width;
                } else if (c == 0x01) {
                    at += 3;
                } else if (c == 0x03) {
                    at += 4;
                } else if (at + 4 < end && IsNameSeg(&d[at])) {
                    Field f;
                    f.region = region;
                    f.name.assign(reinterpret_cast<const char*>(&d[at]), 4);
                    at += 4;
                    good = PkgLength(d, at, width);
                    f.bitOffset = bit;
                    f.bitWidth = width;
                    bit += width;
                    found.push_back(f);
                } else {
                    good = false;
                }
            }
            if (good) r.fields.insert(r.fields.end(), found.begin(), found.end());
        }
    }
    return r;
}

inline std::vector<std::uint8_t> ReadDsdt() {
    constexpr DWORD kAcpi = 'ACPI', kDsdt = 'TDSD';
    const UINT size = GetSystemFirmwareTable(kAcpi, kDsdt, nullptr, 0);
    std::vector<std::uint8_t> d(size);
    if (size == 0 || GetSystemFirmwareTable(kAcpi, kDsdt, d.data(), size) != size) d.clear();
    return d;
}

inline bool LooksThermal(const std::string& n) {
    static const char* hints[] = {"FAN", "FN", "RPM", "TMP", "TEM", "PWM", "FSP", "TACH", "THM", "TH", "DUTY"};
    for (const char* h : hints)
        if (n.find(h) != std::string::npos) return true;
    return false;
}

// ===========================================================================
// BIOS scan: Acer-style fan tables (12 rows x 10 bytes)
//   row: [index, flags, cpuUp, cpuDown, gpuUp, gpuDown, cpuRpm(BE16), gpuRpm(BE16)]
//   The last active stage has "up" = 0x7F; later rows are 0x7F sentinels.
// ===========================================================================
inline bool IsFanTable(const std::uint8_t* d) {
    int top[2] = {-1, -1};
    for (int r = 0; r < 12; ++r) {
        const std::uint8_t* row = d + r * 10;
        if (row[0] != r) return false;
        for (int fan = 0; fan < 2; ++fan) {
            const int up = row[2 + 2 * fan], down = row[3 + 2 * fan];
            if (up < 10 || up > 0x7f || down > up) return false;
            if (r >= 1 && up < 0x7f && down >= up) return false;
            if (up == 0x7f && top[fan] < 0) top[fan] = r;
        }
    }
    for (int fan = 0; fan < 2; ++fan) {
        if (top[fan] < 2) return false;
        for (int r = 0; r < 12; ++r) {
            const int rpm = (d[r * 10 + 6 + 2 * fan] << 8) | d[r * 10 + 7 + 2 * fan];
            if (r == 0 ? rpm > 3000 : r <= top[fan] ? (rpm < 500 || rpm > 9000) : rpm > 9000) return false;
        }
    }
    return true;
}

struct TableGroup {
    std::wstring file;
    std::uint64_t offset = 0;
    std::vector<Table> tables;
};

inline std::vector<TableGroup> ScanBytes(const std::vector<std::uint8_t>& d, const std::wstring& file) {
    std::vector<TableGroup> groups;
    if (d.size() < kTableSize) return groups;
    for (std::size_t i = 0; i + kTableSize <= d.size(); ++i) {
        if (d[i] != 0 || d[i + 10] != 1 || d[i + 20] != 2 || !IsFanTable(&d[i])) continue;
        TableGroup g;
        g.file = file;
        g.offset = i;
        std::size_t at = i;
        while (at + kTableSize <= d.size() && IsFanTable(&d[at])) {
            Table t;
            std::copy_n(d.begin() + static_cast<std::ptrdiff_t>(at), kTableSize, t.begin());
            g.tables.push_back(t);
            at += kTableSize;
        }
        groups.push_back(g);
        i = at - 1;
    }
    return groups;
}

inline std::wstring Find7Zip() {
    for (const wchar_t* p : {L"C:\\Program Files\\7-Zip\\7z.exe", L"C:\\Program Files (x86)\\7-Zip\\7z.exe"})
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return p;
    return L"";
}

inline bool Extract7z(const std::wstring& sevenZip, const std::filesystem::path& file, const std::filesystem::path& out) {
    std::wstring cmd = L"\"" + sevenZip + L"\" x -y -bd -o\"" + out.wstring() + L"\" \"" + file.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 180000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0 || code == 1;  // 1 = warnings
}

inline std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

// Scans a BIOS file. Archives and installers are unpacked with 7-Zip (two levels deep).
inline std::vector<TableGroup> ScanBiosFile(const std::filesystem::path& file, std::vector<std::wstring>& log,
                                            int depth = 0) {
    namespace fs = std::filesystem;
    std::vector<TableGroup> all;
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec || size > 512ull * 1024 * 1024) return all;
    auto found = ScanBytes(ReadFileBytes(file), file.filename().wstring());
    all.insert(all.end(), found.begin(), found.end());
    if (!all.empty() || depth >= 2) return all;

    std::wstring ext = file.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    if (ext != L".exe" && ext != L".zip" && ext != L".7z" && ext != L".cab" && ext != L".container") return all;
    const std::wstring sevenZip = Find7Zip();
    if (sevenZip.empty()) {
        log.push_back(L"7-Zip is not installed, so " + file.filename().wstring() +
                      L" could not be unpacked. Install 7-Zip or extract the .fd/.bin image yourself.");
        return all;
    }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path out = fs::path(tmp) / (L"FanIdentify_" + std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(depth));
    if (!Extract7z(sevenZip, file, out)) {
        log.push_back(L"7-Zip could not unpack " + file.filename().wstring() + L".");
        return all;
    }
    for (const auto& e : fs::recursive_directory_iterator(out, ec)) {
        if (!e.is_regular_file(ec) || e.file_size(ec) < 64 * 1024) continue;
        auto inner = ScanBiosFile(e.path(), log, depth + 1);
        all.insert(all.end(), inner.begin(), inner.end());
    }
    fs::remove_all(out, ec);
    return all;
}

// ===========================================================================
// Result, draft definition and report
// ===========================================================================
struct Result {
    std::wstring manufacturer, product, board, bios;
    Aml aml;
    std::vector<std::wstring> wmiClasses;
    std::vector<TableGroup> groups;
    std::vector<std::wstring> notes;
    bool hasDraft = false;
    ModelDef draft;
    bool hasNbfc = false;
    nbfc::Config nbfc;
};

// Loose match between a config's NotebookModel and this laptop's product name:
// every letter/digit run of 2+ characters in the product must appear in the model.
inline bool ModelNameMatches(const std::wstring& configModel, const std::wstring& product) {
    auto norm = [](std::wstring s) {
        for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
        return s;
    };
    const std::wstring model = norm(configModel), prod = norm(product);
    std::wstring token;
    int tokens = 0, hits = 0;
    for (std::size_t i = 0; i <= prod.size(); ++i) {
        if (i < prod.size() && iswalnum(prod[i])) {
            token += prod[i];
            continue;
        }
        if (token.size() >= 2) {
            ++tokens;
            hits += model.find(token) != std::wstring::npos;
        }
        token.clear();
    }
    return tokens > 0 && hits == tokens;
}

// Draft definition for an imported NBFC config (any manufacturer).
inline void BuildNbfcDraft(Result& r) {
    ModelDef m;
    m.backend = L"nbfc";
    m.manufacturer = r.manufacturer;
    m.product = r.product;
    m.board = r.board;
    m.title = r.product + L" \u00B7 NBFC";
    m.confirmed = false;
    m.source = L"identified";
    m.nbfcConfig = r.nbfc.file;
    r.draft = m;
    r.hasDraft = true;
}

inline bool HasClass(const Result& r, const wchar_t* name) {
    return std::find(r.wmiClasses.begin(), r.wmiClasses.end(), name) != r.wmiClasses.end();
}

inline void Fingerprint(Result& r, AcerWmi& wmi) {
    r.manufacturer = BiosString(L"SystemManufacturer");
    r.product = BiosString(L"SystemProductName");
    r.board = BiosString(L"BaseBoardProduct");
    r.bios = BiosString(L"BIOSVersion");
    r.aml = ParseAml(ReadDsdt());
    r.wmiClasses = wmi.ListVendorClasses();
}

// Picks the distinct tables (several BIOS images usually carry identical groups)
// and maps the first four onto AcerSense's Silent/Normal/Performance/Turbo codes.
inline void BuildDraft(Result& r) {
    r.hasDraft = false;
    const bool acer = r.manufacturer.find(L"Acer") != std::wstring::npos;
    if (!acer) { r.notes.push_back(L"Automatic definitions currently cover Acer laptops only."); return; }
    if (!HasClass(r, L"APGeAction")) { r.notes.push_back(L"Acer APGeAction WMI interface not found."); return; }
    if (r.groups.empty()) { r.notes.push_back(L"No fan tables found yet. Choose this laptop's BIOS update file."); return; }
    const TableGroup* best = &r.groups.front();
    for (const auto& g : r.groups)
        if (g.tables.size() > best->tables.size()) best = &g;
    for (const auto& g : r.groups)
        if (g.tables != best->tables && g.tables.size() >= 2)
            r.notes.push_back(L"BIOS images differ (board variants); using " + best->file + L". Confirm will check it.");
    ModelDef m;
    m.manufacturer = L"Acer";
    m.product = r.product;
    m.board = r.board;
    m.bios = r.bios;
    m.title = L"Acer " + r.product;
    m.confirmed = false;
    m.source = L"identified";
    const wchar_t* names[4] = {L"Silent", L"Normal", L"Performance", L"Turbo"};
    const unsigned codes[4] = {2, 0, 3, 4};
    for (std::size_t i = 0; i < best->tables.size() && i < 4; ++i)
        m.profiles.push_back({names[i], codes[i], DefaultAccent(names[i], i), best->tables[i]});
    r.draft = m;
    r.hasDraft = true;
}

inline std::wstring Hex(std::uint64_t v, int width = 0) {
    std::wostringstream s;
    s << L"0x" << std::hex << std::uppercase << std::setw(width) << std::setfill(L'0') << v;
    return s.str();
}

inline const wchar_t* SpaceName(std::uint8_t s) {
    switch (s) {
    case 0: return L"SystemMemory";
    case 1: return L"SystemIO";
    case 2: return L"PCI_Config";
    case 3: return L"EmbeddedControl";
    case 4: return L"SMBus";
    default: return L"other";
    }
}

inline std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

inline std::vector<std::wstring> Summary(const Result& r) {
    std::vector<std::wstring> out;
    out.push_back(r.manufacturer + L" " + r.product + L"  \u00B7  board " + r.board + L"  \u00B7  BIOS " + r.bios);
    std::size_t ecRegions = 0, thermal = 0;
    for (const auto& reg : r.aml.regions) ecRegions += reg.space == 3;
    for (const auto& f : r.aml.fields) thermal += LooksThermal(f.name);
    out.push_back(r.aml.ok ? std::wstring(L"ACPI: embedded controller ") + (r.aml.hasEcDevice ? L"found" : L"not found") +
                                 L", " + std::to_wstring(r.aml.regions.size()) + L" regions (" + std::to_wstring(ecRegions) +
                                 L" EC), " + std::to_wstring(thermal) + L" fan/temperature fields"
                           : L"ACPI: DSDT could not be read");
    std::wstring wmi = L"WMI: ";
    for (const wchar_t* k : {L"APGeAction", L"AcerGamingFunction", L"AcerGenericMethod"})
        if (HasClass(r, k)) wmi += std::wstring(k) + L"  ";
    if (wmi == L"WMI: ") wmi += std::to_wstring(r.wmiClasses.size()) + L" vendor classes, no known fan interface";
    out.push_back(wmi);
    if (r.groups.empty()) {
        out.push_back(L"BIOS: not scanned yet");
    } else {
        for (const auto& g : r.groups)
            out.push_back(L"BIOS: " + std::to_wstring(g.tables.size()) + L" fan tables in " + g.file + L" at " + Hex(g.offset));
    }
    for (const auto& n : r.notes) out.push_back(n);
    if (r.hasNbfc) {
        out.push_back(L"NBFC config: " + r.nbfc.model + (r.nbfc.author.empty() ? L"" : L" by " + r.nbfc.author) + L"  \u00B7  " +
                      std::to_wstring(r.nbfc.fans.size()) + L" fans  \u00B7  " + std::to_wstring(r.nbfc.registers.size()) +
                      L" register writes");
        out.push_back(ModelNameMatches(r.nbfc.model, r.product)
                          ? L"The config's model name matches this laptop."
                          : L"WARNING: the config is for \"" + r.nbfc.model + L"\" but this laptop is \"" + r.product +
                                L"\". Only continue if you are sure it is the same hardware.");
    } else if (r.hasDraft) {
        std::wstring p = L"Draft: ";
        for (const auto& pr : r.draft.profiles) {
            int stages = 0;
            for (int row = 1; row < 12 && pr.factory[static_cast<std::size_t>(row * 10 + 2)] != 0x7f; ++row) stages = row;
            p += pr.name + L" (" + std::to_wstring(stages + 1) + L" stages)  ";
        }
        out.push_back(p);
        out.push_back(L"EC table at " + Hex(r.draft.tableBase, 4) + L", commands " + Hex(r.draft.readCmd, 2) + L"/" +
                      Hex(r.draft.writeCmd, 2) + L" (Acer EC family). Confirm reads the live table once before use.");
    }
    return out;
}

inline std::wstring Report(const Result& r, const std::vector<std::wstring>& extra) {
    std::wostringstream s;
    s << L"Fan Control - Identify report\n\n";
    for (const auto& line : Summary(r)) s << line << L"\n";
    s << L"\n[ACPI regions]\n";
    for (const auto& reg : r.aml.regions)
        s << Widen(reg.name) << L"  " << SpaceName(reg.space)
          << (reg.known ? L"  base " + Hex(reg.base) + L"  length " + Hex(reg.length) : std::wstring(L"  (dynamic)")) << L"\n";
    s << L"\n[Fan / temperature fields]\n";
    std::map<std::string, const Region*> byName;
    for (const auto& reg : r.aml.regions) byName[reg.name] = &reg;
    for (const auto& f : r.aml.fields) {
        if (!LooksThermal(f.name)) continue;
        s << Widen(f.name) << L"  in " << Widen(f.region) << L"  +" << Hex(f.bitOffset / 8) << L" (" << f.bitWidth << L" bits)";
        const auto it = byName.find(f.region);
        if (it != byName.end() && it->second->known && f.bitOffset % 8 == 0)
            s << L"  -> " << SpaceName(it->second->space) << L" " << Hex(it->second->base + f.bitOffset / 8);
        s << L"\n";
    }
    s << L"\n[Vendor WMI classes]\n";
    for (const auto& c : r.wmiClasses) s << c << L"\n";
    s << L"\n[BIOS fan tables]\n";
    for (const auto& g : r.groups) {
        s << g.file << L" at " << Hex(g.offset) << L"\n";
        for (std::size_t t = 0; t < g.tables.size(); ++t) {
            s << L"  table " << t << L"\n";
            for (int row = 0; row < 12; ++row) {
                s << L"    ";
                for (int b = 0; b < 10; ++b)
                    s << std::hex << std::setw(2) << std::setfill(L'0') << unsigned(g.tables[t][static_cast<std::size_t>(row * 10 + b)]) << L" ";
                s << std::dec << L"\n";
            }
        }
    }
    if (!extra.empty()) {
        s << L"\n[Log]\n";
        for (const auto& e : extra) s << e << L"\n";
    }
    return s.str();
}

}  // namespace acer::identify
