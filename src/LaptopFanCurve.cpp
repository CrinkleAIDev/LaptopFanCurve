// Laptop Fan Curve - custom fan curves for laptops. Built-in support for every AcerSense profile on the
// Acer Swift X 16 SFX16-61G (Ariya_PEH, BIOS V1.56). Direct2D UI.
//
// Acer table backend (verified on an SFX16-61G):
// - The EC runs the 120-byte table at XRAM 0x0800 for the active profile and
//   follows changes within seconds. A profile switch or sleep reloads Acer's table.
// - A canary byte (0x0877) marks our table; when it reverts, the custom table is
//   written again. Acer's WMI profile command reloads the factory table (undo).
// - Each profile's factory table is checked against the live EC once before the
//   app will ever write that profile.
#include "Identify.h"
#include "Safety.h"

#include <commctrl.h>
#include <commdlg.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <sstream>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace acer;
namespace fs = std::filesystem;
using D2D1::ColorF;

namespace {

// ===========================================================================
// Fan table model
// A "point" i (1..stages) of a fan's curve is (temperature that enters stage i,
// fan RPM of stage i). Entering stage i is row (i-1)'s "up" byte; stage i's
// "down" byte keeps Acer's hysteresis distance below that temperature.
// ===========================================================================
constexpr std::size_t kCanaryIndex = 0x77;
constexpr int kMinTemp = 30, kMaxTemp = 100, kMinRpm = 1000, kMaxRpm = 5000, kChartRpm = 5000;
using safety::kWatchdogC;

std::size_t UpIndex(int row, int fan) { return static_cast<std::size_t>(row * 10 + 2 + fan * 2); }
std::size_t DownIndex(int row, int fan) { return UpIndex(row, fan) + 1; }

int Stages(const Table& t, int fan) {
    for (int r = 1; r < 12; ++r)
        if (t[UpIndex(r, fan)] == 0x7f) return r;
    return 11;
}
int PointTemp(const Table& t, int fan, int i) { return t[UpIndex(i - 1, fan)]; }
int PointRpm(const Table& t, int fan, int i) { return RowRpm(t, static_cast<std::size_t>(i), fan == 1); }

int Hysteresis(const Table& factory, int fan, int i) {
    const int h = PointTemp(factory, fan, i) - factory[DownIndex(i, fan)];
    return h >= 2 ? h : 3;
}

void SetPointTemp(Table& t, const Table& factory, int fan, int i, int temp) {
    t[UpIndex(i - 1, fan)] = static_cast<std::uint8_t>(temp);
    const int down = std::clamp(temp - Hysteresis(factory, fan, i), 20, temp - 1);
    t[DownIndex(i, fan)] = static_cast<std::uint8_t>(down);
}
void SetPointRpm(Table& t, int fan, int i, int rpm) {
    SetRowRpm(t, static_cast<std::size_t>(i), fan == 1, static_cast<std::uint16_t>(rpm));
}

bool Validate(const Table& t, const Table& factory, std::wstring& reason) {
    for (int r = 0; r < 12; ++r)
        if (t[r * 10] != factory[r * 10] || t[r * 10 + 1] != factory[r * 10 + 1]) {
            reason = L"Table structure differs from Acer's.";
            return false;
        }
    for (int fan = 0; fan < 2; ++fan) {
        const int k = Stages(factory, fan);
        if (Stages(t, fan) != k || RowRpm(t, 0, fan == 1) != RowRpm(factory, 0, fan == 1)) {
            reason = L"Stage layout differs from Acer's.";
            return false;
        }
        for (int i = 1; i <= k; ++i) {
            const int x = PointTemp(t, fan, i), y = PointRpm(t, fan, i), down = t[DownIndex(i, fan)];
            const std::wstring at = std::wstring(fan ? L" (GPU" : L" (CPU") + L" stage " + std::to_wstring(i) + L")";
            if (x < kMinTemp || x > kMaxTemp) { reason = L"Temperatures must be 30-100 \u00B0C" + at; return false; }
            if (y < kMinRpm || y > kMaxRpm) { reason = L"Fan speeds must be 1000-5000 RPM" + at; return false; }
            if (down < 20 || down >= x) { reason = L"Invalid hysteresis" + at; return false; }
        }
        for (int r = k + 1; r < 12; ++r)
            for (int b = 2; b < 10; ++b)
                if (t[r * 10 + b] != factory[r * 10 + b]) { reason = L"Unused rows must stay Acer's."; return false; }
    }
    return true;
}

// True when any point starts later or spins slower than Acer's curve.
bool Quieter(const Table& t, const Table& factory) {
    for (int fan = 0; fan < 2; ++fan)
        for (int i = 1; i <= Stages(factory, fan); ++i)
            if (PointTemp(t, fan, i) > PointTemp(factory, fan, i) || PointRpm(t, fan, i) < PointRpm(factory, fan, i))
                return true;
    return false;
}

std::wstring Widen(const char* s) { return std::wstring(s, s + strlen(s)); }

std::wstring HexTable(const Table& t) {
    std::wostringstream s;
    for (std::size_t i = 0; i < t.size(); ++i)
        s << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<unsigned>(t[i]) << (i % 10 == 9 ? L" | " : L" ");
    return s.str();
}

// ===========================================================================
// Storage (%LOCALAPPDATA%\LaptopFanCurve)
// ===========================================================================
fs::path DataDir() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    fs::path dir = (n && n < MAX_PATH) ? fs::path(buf) / L"LaptopFanCurve" : fs::path(L".");
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}
fs::path SettingsPath() { return DataDir() / L"settings.ini"; }

void AppLog(const std::wstring& line) {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    localtime_s(&tm, &t);
    std::wofstream f(DataDir() / L"log.txt", std::ios::app);
    f << std::put_time(&tm, L"%Y-%m-%d %H:%M:%S") << L"  " << line << L"\n";
}

int GetSetting(const std::wstring& key, int def = 0) {
    return static_cast<int>(GetPrivateProfileIntW(L"app", key.c_str(), def, SettingsPath().c_str()));
}
void PutSetting(const std::wstring& key, int value) {
    WritePrivateProfileStringW(L"app", key.c_str(), std::to_wstring(value).c_str(), SettingsPath().c_str());
}
std::wstring GetSettingText(const std::wstring& key) {
    wchar_t buf[2048] = {};
    GetPrivateProfileStringW(L"app", key.c_str(), L"", buf, 2048, SettingsPath().c_str());
    return buf;
}
void PutSettingText(const std::wstring& key, const std::wstring& value) {
    WritePrivateProfileStringW(L"app", key.c_str(), value.c_str(), SettingsPath().c_str());
}

bool LoadTable(const fs::path& p, Table& t) {
    std::ifstream f(p, std::ios::binary);
    return f && f.read(reinterpret_cast<char*>(t.data()), static_cast<std::streamsize>(t.size())) &&
           f.gcount() == static_cast<std::streamsize>(t.size());
}
bool SaveTable(const fs::path& p, const Table& t) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(t.data()), static_cast<std::streamsize>(t.size()));
    return f.good();
}

// Start with Windows: logon task with highest privileges; XML so it runs on battery
// and is never stopped after 72 hours.
constexpr wchar_t kTaskName[] = L"LaptopFanCurve";

bool RunHidden(std::wstring cmd) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

bool StartupTaskExists() { return RunHidden(std::wstring(L"schtasks.exe /Query /TN ") + kTaskName); }

bool SetStartupTask(bool enable) {
    if (!enable) return RunHidden(std::wstring(L"schtasks.exe /Delete /F /TN ") + kTaskName);
    wchar_t exe[MAX_PATH] = {}, domain[256] = {}, user[256] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    GetEnvironmentVariableW(L"USERDOMAIN", domain, 256);
    GetEnvironmentVariableW(L"USERNAME", user, 256);
    std::wstring account = std::wstring(domain) + L"\\" + user, command = exe;
    for (std::wstring* s : {&account, &command})
        for (std::size_t at = 0; (at = s->find(L'&', at)) != std::wstring::npos; at += 5) s->replace(at, 1, L"&amp;");
    const std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + account +
        L"</UserId><Delay>PT10S</Delay></LogonTrigger></Triggers>\r\n"
        L"<Principals><Principal id=\"Author\"><UserId>" + account +
        L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel>"
        L"</Principal></Principals>\r\n"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>7</Priority></Settings>\r\n"
        L"<Actions Context=\"Author\"><Exec><Command>" + command +
        L"</Command><Arguments>/tray</Arguments></Exec></Actions>\r\n</Task>\r\n";
    const fs::path file = DataDir() / L"startup_task.xml";
    {
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f.write("\xFF\xFE", 2);
        f.write(reinterpret_cast<const char*>(xml.data()), static_cast<std::streamsize>(xml.size() * 2));
        if (!f.good()) return false;
    }
    return RunHidden(std::wstring(L"schtasks.exe /Create /F /TN ") + kTaskName + L" /XML \"" + file.wstring() + L"\"");
}

// ===========================================================================
// Application state
// ===========================================================================
enum class Verify { Unknown = 0, Ok = 1, Mismatch = 2 };

struct Profile {
    std::wstring name;
    unsigned code = 0;  // AcerSense/WMI profile code
    std::uint32_t accent = 0;
    Table factory{};
    Table saved{};
    bool hasSaved = false;
    bool enabled = false;  // maintain the custom table while in this profile
    Table draft{};
    Verify verified = Verify::Unknown;
    bool applied = false;
    bool tripped = false;
};

enum Id : int {
    kNone = 0, kTab0 = 1, kFanCpu = 10, kFanGpu, kToggleCustom, kReset, kSwitchTo, kApply, kToggleStartup,
    kRestoreAll, kIdentifyView, kScan, kChooseBios, kUseDraft, kConfirm, kSaveReport, kBack, kImportNbfc, kNbfcHelp, kGetPawnIO, kCheckAgain,
    kFan0 = 40, kTrayOpen = 100, kTrayRestoreExit, kTrayExit
};
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT_PTR kTimer = 1, kNbTimer = 2;
constexpr wchar_t kWindowClass[] = L"LaptopFanCurveWindow";
constexpr float kWinW = 1200.f, kWinH = 780.f;

struct Hit {
    D2D1_RECT_F r;
    int id;
    bool enabled;
};

struct App {
    AcerWmi wmi;
    PawnEc ec;
    bool wmiOk = false, ecOk = false;
    std::wstring ecError;
    bool hasModel = false;
    ModelDef model;
    std::vector<Profile> profiles;
    bool identifyView = false;
    bool scanned = false;
    identify::Result found;
    std::vector<std::wstring> identifyLog;

    // NBFC backend (imported NoteBook FanControl config): the app runs the curve itself.
    bool nbfcMode = false;
    nbfc::Config nb;
    std::vector<std::vector<nbfc::Threshold>> nbDraft, nbSaved;
    nbfc::Engine nbEngine;
    std::vector<double> nbRead;
    unsigned nbTemp = 0;
    bool nbTempOk = false;
    int nbTempFails = 0;
    ULONGLONG lastNbTick = 0;
    unsigned laptopProfile = 99;
    Telemetry tele;
    std::deque<unsigned> cpuHist, gpuHist;
    int failures = 0, teleFails = 0;
    ULONGLONG lastFullVerify = 0, lastEcAttempt = 0;
    bool startup = false, quitting = false, trayHintShown = false, startHidden = false;
    std::wstring status;

    // UI
    int tab = 0, fan = 0, selPoint = 1, hoverPoint = 0, hoverId = kNone, pressedId = kNone;
    bool dragging = false;
    HWND hwnd = nullptr;
    UINT dpi = 96;
    std::vector<Hit> hits;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d;
    Microsoft::WRL::ComPtr<IDWriteFactory> dw;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> rt;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> dash;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fTitle, fBig, fH, fBody, fBodyBold, fSmall, fNum;
    HICON icon = nullptr, smallIcon = nullptr;
    NOTIFYICONDATAW tray{};
};
App* g = nullptr;

int IndexOfCode(App& a, unsigned code) {
    for (std::size_t i = 0; i < a.profiles.size(); ++i)
        if (a.profiles[i].code == code) return static_cast<int>(i);
    return -1;
}
int ProfileCount(const App& a) { return static_cast<int>(a.profiles.size()); }
const Table& Baseline(const Profile& p) { return p.hasSaved ? p.saved : p.factory; }
Table Target(const Profile& p) {
    Table t = p.saved;
    if (t[kCanaryIndex] == p.factory[kCanaryIndex]) t[kCanaryIndex] ^= 0x01;  // unreachable or +1 RPM
    return t;
}
std::wstring KeyFor(const Profile& p, const wchar_t* what) { return std::wstring(what) + L"_" + p.name; }

void Notify(App& a, const std::wstring& title, const std::wstring& text) {
    a.tray.uFlags = NIF_INFO;
    wcsncpy_s(a.tray.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(a.tray.szInfo, text.c_str(), _TRUNCATE);
    a.tray.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &a.tray);
}

void SaveProfile(Profile& p) {
    if (p.hasSaved) SaveTable(DataDir() / (std::wstring(L"custom_") + p.name + L".bin"), p.saved);
    PutSetting(KeyFor(p, L"enabled"), p.enabled ? 1 : 0);
    PutSetting(KeyFor(p, L"verified"), static_cast<int>(p.verified));
}

// Builds the profile list from the model definition and loads saved curves.
void LoadProfiles(App& a) {
    a.profiles.clear();
    for (const ProfileDef& d : a.model.profiles) {
        Profile p;
        p.name = d.name;
        p.code = d.code;
        p.accent = d.accent;
        p.factory = d.factory;
        a.profiles.push_back(p);
    }
    a.ec.Configure(a.model.tableBase, a.model.readCmd, a.model.writeCmd);
    a.tab = 0;
    for (Profile& p : a.profiles) {
        Table t{};
        std::wstring reason;
        if (LoadTable(DataDir() / (std::wstring(L"custom_") + p.name + L".bin"), t) && Validate(t, p.factory, reason)) {
            p.saved = t;
            p.hasSaved = true;
        }
        p.enabled = p.hasSaved && GetSetting(KeyFor(p, L"enabled")) != 0;
        p.verified = static_cast<Verify>(GetSetting(KeyFor(p, L"verified")));
    }
    if (a.model.source != L"builtin" || a.profiles.empty()) {
        for (Profile& p : a.profiles) p.draft = Baseline(p);
        return;
    }
    Profile& silent = a.profiles[0];
    silent.verified = Verify::Ok;  // verified byte-for-byte on an SFX16-61G
    for (Profile& p : a.profiles) p.draft = Baseline(p);
}

// ===========================================================================
// NBFC backend
// ===========================================================================
struct PawnPort : nbfc::EcPort {
    explicit PawnPort(PawnEc& e) : ec(e) {}
    std::uint8_t Read(std::uint8_t reg) override { return ec.ReadEc(reg); }
    void Write(std::uint8_t reg, std::uint8_t value) override { ec.WriteEc(reg, value); }
    PawnEc& ec;
};

bool SameCurve(const std::vector<nbfc::Threshold>& x, const std::vector<nbfc::Threshold>& y) {
    if (x.size() != y.size()) return false;
    for (std::size_t i = 0; i < x.size(); ++i)
        if (x[i].up != y[i].up || x[i].down != y[i].down || std::fabs(x[i].speed - y[i].speed) > 0.001) return false;
    return true;
}
bool SameCurves(const std::vector<std::vector<nbfc::Threshold>>& x, const std::vector<std::vector<nbfc::Threshold>>& y) {
    if (x.size() != y.size()) return false;
    for (std::size_t i = 0; i < x.size(); ++i)
        if (!SameCurve(x[i], y[i])) return false;
    return true;
}

// Saved as "up:down:speed;..." per fan.
std::wstring CurveText(const std::vector<nbfc::Threshold>& c) {
    std::wostringstream s;
    for (const auto& t : c) s << t.up << L':' << t.down << L':' << t.speed << L';';
    return s.str();
}
bool ParseCurveText(const std::wstring& text, std::vector<nbfc::Threshold>& out) {
    out.clear();
    std::wistringstream s(text);
    nbfc::Threshold t;
    wchar_t c1, c2, c3;
    while (s >> t.up >> c1 >> t.down >> c2 >> t.speed >> c3) {
        if (c1 != L':' || c2 != L':' || c3 != L';') return false;
        out.push_back(t);
    }
    return !out.empty();
}

std::vector<std::vector<nbfc::Threshold>> ConfigCurves(const nbfc::Config& c) {
    std::vector<std::vector<nbfc::Threshold>> curves;
    for (const auto& f : c.fans) curves.push_back(f.thresholds);
    return curves;
}

bool LoadNbfcModel(App& a) {
    std::wstring error;
    if (!nbfc::Load(a.model.nbfcConfig, a.nb, error)) {
        a.status = L"The NBFC config could not be loaded: " + error;
        AppLog(a.status);
        return false;
    }
    a.nbfcMode = true;
    a.profiles.clear();
    Profile p;
    p.name = a.nb.model.empty() ? std::wstring(L"Fan curve") : a.nb.model;
    p.code = 0xFFFF;
    p.accent = 0x2DD4BF;
    p.verified = Verify::Ok;
    a.nbSaved = ConfigCurves(a.nb);
    bool saved = true;
    for (std::size_t f = 0; f < a.nbSaved.size(); ++f) {
        std::vector<nbfc::Threshold> c;
        if (ParseCurveText(GetSettingText(L"nbfc_curve_" + std::to_wstring(f)), c) && c.size() == a.nbSaved[f].size())
            a.nbSaved[f] = c;
        else
            saved = false;
    }
    p.hasSaved = saved;
    p.enabled = saved && GetSetting(KeyFor(p, L"enabled")) != 0;
    a.profiles.push_back(p);
    a.nbDraft = a.nbSaved;
    a.tab = 0;
    a.fan = 0;
    a.selPoint = 1;
    return true;
}

void SaveNbfc(App& a) {
    for (std::size_t f = 0; f < a.nbSaved.size(); ++f) PutSettingText(L"nbfc_curve_" + std::to_wstring(f), CurveText(a.nbSaved[f]));
    PutSetting(KeyFor(a.profiles[0], L"enabled"), a.profiles[0].enabled ? 1 : 0);
}

// Hands the fans back to the firmware (writes the config's reset values).
void StopNbfc(App& a, const std::wstring& why) {
    if (!a.nbEngine.Running()) return;
    try {
        EcLock lock(2000);
        PawnPort port(a.ec);
        a.nbEngine.Stop(port);
    } catch (const std::exception& e) {
        AppLog(L"NBFC reset error: " + Widen(e.what()));
    }
    if (!a.profiles.empty()) a.profiles[0].applied = false;
    AppLog(L"NBFC control stopped: " + why);
}

// The control loop for NBFC configs, run every EcPollInterval (at least 500 ms).
void NbTick(App& a) {
    if (!a.nbfcMode || a.profiles.empty()) return;
    const ULONGLONG now = GetTickCount64();
    if (now - a.lastNbTick < static_cast<ULONGLONG>(std::max(a.nb.pollMs, 500))) return;
    a.lastNbTick = now;
    unsigned t = 0;
    a.nbTempOk = a.tele.ok ? (t = a.tele.cpuTemp, true) : a.wmi.ThermalZoneC(t);
    if (a.nbTempOk) a.nbTemp = t;
    if (!a.ecOk || !a.model.confirmed) return;
    Profile& p = a.profiles[0];
    try {
        PawnPort port(a.ec);
        EcLock lock(1000);
        a.nbRead.clear();
        for (const auto& f : a.nb.fans) a.nbRead.push_back(nbfc::Engine::ReadPercent(port, f));
        if (p.enabled) {
            const safety::NbfcAction temp = safety::NbfcTemperature(a.nbTempFails, a.nbTempOk);
            if (temp != safety::NbfcAction::Run) {
                if (temp == safety::NbfcAction::HandBack && a.nbEngine.Running()) {
                    a.nbEngine.Stop(port);
                    p.applied = false;
                    AppLog(L"NBFC: no temperature reading; fans returned to the firmware.");
                    a.status = L"No temperature reading, so the fans were handed back to the firmware.";
                }
                return;
            }
            if (!a.nbEngine.Running()) {
                a.nbEngine.Start(a.nb, a.nbSaved);
                AppLog(L"NBFC control started.");
            }
            a.nbEngine.Step(port, static_cast<int>(a.nbTemp));
            p.applied = true;
        } else if (a.nbEngine.Running()) {
            a.nbEngine.Stop(port);
            p.applied = false;
            AppLog(L"NBFC control stopped.");
        }
        safety::EcErrorLimitReached(a.failures, false);
    } catch (const std::exception& e) {
        AppLog(L"EC error: " + Widen(e.what()));
        if (safety::EcErrorLimitReached(a.failures, true)) {
            p.enabled = false;
            SaveNbfc(a);
            StopNbfc(a, L"repeated EC errors");
            Notify(a, L"Custom fan curve stopped", L"Repeated EC errors. The fans were handed back to the firmware.");
            a.status = L"Stopped after repeated EC errors; the firmware controls the fans again.";
        }
    }
}

// ===========================================================================
// Hardware operations
// ===========================================================================
bool ReloadFactory(App& a, const Profile& p) {
    std::uint64_t raw = 0;
    const bool ok = a.wmi.SetFactoryProfile(p.code, raw);
    Sleep(900);
    return ok;
}

// Checks Acer's table for this profile against the live EC, once per profile.
void VerifyProfile(App& a, Profile& p) {
    EcLock lock(1000);
    Table live = a.ec.ReadTable();
    if (live != p.factory && live != Target(p)) {
        ReloadFactory(a, p);
        live = a.ec.ReadTable();
    }
    p.verified = (live == p.factory || (p.hasSaved && live == Target(p))) ? Verify::Ok : Verify::Mismatch;
    if (p.verified == Verify::Mismatch) {
        AppLog(std::wstring(L"Acer table mismatch for ") + p.name + L"; editing disabled. Live: " + HexTable(live));
        p.enabled = false;
    } else {
        AppLog(std::wstring(L"Acer ") + p.name + L" table verified against the live EC.");
    }
    SaveProfile(p);
}

void ApplyProfile(App& a, Profile& p) {
    EcLock lock(1000);
    const Table target = Target(p);
    Table live = a.ec.ReadTable();
    if (live == target) {
        p.applied = true;
        return;
    }
    if (live != p.factory) {
        ReloadFactory(a, p);
        live = a.ec.ReadTable();
        if (live != p.factory) throw std::runtime_error("live table is not Acer's table for this profile");
    }
    a.ec.WriteTableDiff(live, target);
    if (a.ec.ReadTable() != target) throw std::runtime_error("verification after write failed");
    unsigned now = 99;
    if (a.wmi.GetProfile(now) && now != p.code) {
        const int idx = IndexOfCode(a, now);
        if (idx >= 0) ReloadFactory(a, a.profiles[static_cast<std::size_t>(idx)]);
        AppLog(L"Profile changed during apply; reloaded Acer's table.");
        return;
    }
    std::size_t changed = 0;
    for (std::size_t i = 0; i < kTableSize; ++i) changed += live[i] != target[i];
    p.applied = true;
    a.lastFullVerify = GetTickCount64();
    AppLog(std::wstring(L"Custom ") + p.name + L" curve applied (" + std::to_wstring(changed) + L" bytes).");
}

// Puts Acer's table back if the laptop is currently in this profile.
void RestoreProfile(App& a, Profile& p, const std::wstring& why) {
    p.applied = false;
    if (a.laptopProfile != p.code) return;
    AppLog(std::wstring(L"Restoring Acer ") + p.name + L" curve: " + why);
    ReloadFactory(a, p);
    if (!a.ecOk || !a.model.confirmed) return;
    try {
        EcLock lock(1000);
        const Table live = a.ec.ReadTable();
        if (live != p.factory) {
            a.ec.WriteTableDiff(live, p.factory);
            if (a.ec.ReadTable() != p.factory) AppLog(L"Restore could not be verified; a reboot restores Acer's tables.");
        }
    } catch (const std::exception& e) {
        AppLog(L"Restore error: " + Widen(e.what()));
    }
}

// Opens PawnIO; logs the reason on failure. Called at start and retried from Tick.
void ConnectEc(App& a) {
    a.lastEcAttempt = GetTickCount64();
    try {
        a.ec.Connect();
        if (!a.ecOk && !a.ecError.empty()) AppLog(L"Fan controller connected after retry.");
        a.ecOk = true;
        a.ecError.clear();
    } catch (const std::exception& e) {
        const std::wstring error = Widen(e.what());
        if (error != a.ecError) AppLog(L"Fan controller unavailable: " + error);
        a.ecOk = false;
        a.ecError = error;
    }
}

void Tick(App& a) {
    if (!a.ecOk && GetTickCount64() - a.lastEcAttempt > 10000) ConnectEc(a);
    const unsigned previous = a.laptopProfile;
    if (a.wmi.GetProfile(a.laptopProfile) && previous != 99 && previous != a.laptopProfile)
        AppLog(L"Laptop profile changed to code " + std::to_wstring(a.laptopProfile));
    a.tele = a.wmi.Read();
    if (!a.tele.ok && a.nbfcMode && a.nbTempOk) {
        a.cpuHist.push_back(a.nbTemp);
        while (a.cpuHist.size() > 90) a.cpuHist.pop_front();
    }
    if (a.tele.ok) {
        a.cpuHist.push_back(a.tele.cpuTemp);
        a.gpuHist.push_back(a.tele.gpuTemp);
        while (a.cpuHist.size() > 90) a.cpuHist.pop_front();
        while (a.gpuHist.size() > 90) a.gpuHist.pop_front();
    }
    if (a.nbfcMode) return;  // NbTick runs NBFC configs on its own timer
    const int idx = IndexOfCode(a, a.laptopProfile);
    for (int i = 0; i < ProfileCount(a); ++i)
        if (i != idx) a.profiles[static_cast<std::size_t>(i)].applied = false;
    // Unconfirmed (identified) models never touch the EC until Confirm succeeds.
    if (idx < 0 || !a.ecOk || !a.hasModel || !a.model.confirmed) return;
    Profile& p = a.profiles[static_cast<std::size_t>(idx)];
    try {
        if (p.verified == Verify::Unknown) VerifyProfile(a, p);
        if (!p.enabled || p.tripped || p.verified != Verify::Ok) return;
        const safety::AcerAction guard = safety::AcerWatchdog(a.teleFails, p.applied, Quieter(p.saved, p.factory), a.tele.ok,
                                                              a.tele.cpuTemp, a.tele.gpuTemp);
        if (guard == safety::AcerAction::RestoreNoReadings) {
            p.tripped = true;
            RestoreProfile(a, p, L"temperature readings unavailable");
            Notify(a, L"Acer fan curve restored", L"Temperature readings stopped, so your quieter curve was turned off for safety.");
            a.status = L"Temperature readings stopped; Acer's curve was restored for safety.";
            return;
        }
        if (guard == safety::AcerAction::RestoreHot) {
            p.tripped = true;
            RestoreProfile(a, p, L"watchdog at " + std::to_wstring(std::max(a.tele.cpuTemp, a.tele.gpuTemp)) + L" C");
            Notify(a, L"Acer fan curve restored", std::wstring(L"Temperature reached 95 \u00B0C with your quieter ") +
                                                      p.name + L" curve. Apply it again to re-enable.");
            a.status = L"Watchdog restored Acer's curve at 95 \u00B0C.";
            return;
        }
        bool need = !p.applied;
        if (!need) {
            EcLock lock(1000);
            need = safety::NeedsReapply(p.applied, a.ec.ReadXram(static_cast<std::uint16_t>(a.ec.TableBase() + kCanaryIndex)),
                                        Target(p)[kCanaryIndex]);
            if (need) AppLog(L"EC reloaded its table; re-applying.");
            else if (GetTickCount64() - a.lastFullVerify > 60000) {
                need = a.ec.ReadTable() != Target(p);
                a.lastFullVerify = GetTickCount64();
                if (need) AppLog(L"Periodic check found a changed table; re-applying.");
            }
        }
        if (need) ApplyProfile(a, p);
        safety::EcErrorLimitReached(a.failures, false);
    } catch (const std::exception& e) {
        AppLog(L"EC error: " + Widen(e.what()));
        if (safety::EcErrorLimitReached(a.failures, true)) {
            p.enabled = false;
            SaveProfile(p);
            RestoreProfile(a, p, L"repeated EC errors");
            Notify(a, L"Custom fan curve stopped", L"Repeated EC errors. Acer's curve was restored.");
            a.status = L"Stopped after repeated EC errors; Acer's curve restored.";
        }
    }
}

// ===========================================================================
// Commands
// ===========================================================================
bool ValidNbfcCurves(const std::vector<std::vector<nbfc::Threshold>>& curves, std::wstring& reason) {
    for (std::size_t f = 0; f < curves.size(); ++f)
        for (std::size_t i = 0; i < curves[f].size(); ++i) {
            const nbfc::Threshold& t = curves[f][i];
            if (t.up < 0 || t.up > 110 || t.speed < 0 || t.speed > 100 || (i > 0 && (t.down < 0 || t.down >= t.up))) {
                reason = L"Fan " + std::to_wstring(f + 1) + L" point " + std::to_wstring(i + 1) + L" is out of range.";
                return false;
            }
        }
    return true;
}

void CmdApplyNbfc(App& a) {
    Profile& p = a.profiles[0];
    std::wstring reason;
    if (!a.model.confirmed) {
        a.status = L"Confirm this model with the laptop first (Identify screen).";
        return;
    }
    if (!ValidNbfcCurves(a.nbDraft, reason)) {
        a.status = reason;
        return;
    }
    // Consent is tied to this exact config file on this laptop (see ModelFingerprint).
    const std::wstring consentKey = L"nbfc_consent_" + ModelFingerprint(a.model);
    if (!GetSetting(consentKey)) {
        const bool match = identify::ModelNameMatches(a.nb.model, BiosString(L"SystemProductName"));
        const std::wstring text =
            L"This NoteBook FanControl config was written by " + (a.nb.author.empty() ? std::wstring(L"the community") : a.nb.author) +
            L" for \"" + a.nb.model + L"\". This laptop is \"" + BiosString(L"SystemProductName") + L"\"." +
            L"\n\nWhile your curve is on, the app writes the config's fan registers in the fan controller and runs the "
            L"curve itself. Closing the app hands the fans back to the firmware. A config made for different hardware "
            L"can make fans misbehave; if anything sounds wrong, click 'Restore firmware control'.";
        TASKDIALOGCONFIG tc{};
        tc.cbSize = sizeof(tc);
        tc.hwndParent = a.hwnd;
        tc.pszWindowTitle = L"Use an NBFC config";
        tc.pszMainIcon = match ? TD_INFORMATION_ICON : TD_WARNING_ICON;
        tc.pszMainInstruction = match ? L"Turn on this NBFC config?" : L"This config was made for a different model name";
        tc.pszContent = text.c_str();
        tc.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
        tc.nDefaultButton = IDNO;
        if (!match) tc.pszVerificationText = L"I am sure this config is for this laptop's exact hardware";
        int button = 0;
        BOOL sure = FALSE;
        if (FAILED(TaskDialogIndirect(&tc, &button, nullptr, &sure)) || button != IDYES) return;
        if (!match && !sure) {
            a.status = L"Not turned on: tick the confirmation box to use a config made for a different model name.";
            return;
        }
        PutSetting(consentKey, 1);
    }
    a.nbSaved = a.nbDraft;
    p.hasSaved = true;
    p.enabled = true;
    SaveNbfc(a);
    if (a.nbEngine.Running()) a.nbEngine.Start(a.nb, a.nbSaved);  // pick up the new curve
    a.failures = 0;
    a.lastNbTick = 0;
    NbTick(a);
    a.status = p.applied ? L"Your curve is running." : L"Saved; starting...";
}

void CmdApply(App& a) {
    if (a.nbfcMode) {
        CmdApplyNbfc(a);
        return;
    }
    Profile& p = a.profiles[static_cast<std::size_t>(a.tab)];
    std::wstring reason;
    if (!a.model.confirmed) {
        a.status = L"Confirm this identified model with the laptop first (Identify screen).";
        return;
    }
    if (p.verified == Verify::Mismatch) {
        a.status = L"Acer's table for this profile did not match the laptop; custom curves are disabled for it.";
        return;
    }
    if (!Validate(p.draft, p.factory, reason)) {
        a.status = reason;
        return;
    }
    if (Quieter(p.draft, p.factory) &&
        MessageBoxW(a.hwnd,
                    L"Parts of this curve start later or spin slower than Acer's.\n\n"
                    L"If the CPU or GPU reaches 95 \u00B0C, the app restores Acer's curve automatically. "
                    L"Watch temperatures under load the first time.\n\nApply this curve?",
                    L"Less cooling than Acer's curve", MB_YESNO | MB_ICONWARNING) != IDYES)
        return;
    p.saved = p.draft;
    p.hasSaved = true;
    p.enabled = true;
    p.tripped = false;
    p.applied = false;
    SaveProfile(p);
    a.failures = 0;
    Tick(a);
    if (a.laptopProfile == p.code)
        a.status = p.applied ? std::wstring(L"Custom ") + p.name + L" curve applied and verified."
                             : L"Saved; applying...";
    else
        a.status = std::wstring(L"Saved. It runs whenever the laptop is in ") + p.name + L" mode.";
}

void CmdToggleCustom(App& a) {
    Profile& p = a.profiles[static_cast<std::size_t>(a.tab)];
    if (a.nbfcMode && p.enabled) {
        p.enabled = false;
        SaveNbfc(a);
        StopNbfc(a, L"custom curve turned off");
        a.status = L"The firmware controls the fans again.";
        return;
    }
    if (p.enabled) {
        p.enabled = false;
        p.tripped = false;
        SaveProfile(p);
        RestoreProfile(a, p, L"custom curve turned off");
        a.status = std::wstring(L"Acer's ") + p.name + L" curve is in use.";
    } else {
        CmdApply(a);  // enables the curve shown in the editor
    }
}

void CmdSwitchTo(App& a) {
    const Profile& p = a.profiles[static_cast<std::size_t>(a.tab)];
    std::uint64_t raw = 0;
    const bool ok = a.wmi.SetFactoryProfile(p.code, raw) && raw == 0;
    AppLog(std::wstring(L"Switch laptop to ") + p.name + (ok ? L": ok" : L": failed"));
    Sleep(800);
    Tick(a);
    a.status = ok ? std::wstring(L"Laptop switched to ") + p.name + L"."
                  : std::wstring(L"Acer refused the switch to ") + p.name + L" (some modes need the charger).";
}

void CmdRestoreAll(App& a) {
    if (a.nbfcMode) {
        if (!a.profiles.empty()) a.profiles[0].enabled = false;
        SaveNbfc(a);
        StopNbfc(a, L"restore firmware control");
        a.status = L"The firmware controls the fans again. Your curve is still saved.";
        return;
    }
    for (Profile& p : a.profiles) {
        p.enabled = false;
        p.tripped = false;
        SaveProfile(p);
        RestoreProfile(a, p, L"restore all");
    }
    a.status = L"All profiles use Acer's curves. Your curves are still saved.";
}

// ===========================================================================
// Identify commands
// ===========================================================================
std::vector<std::filesystem::path> ModelDirs() { return {fs::path(ExeDirectory()) / L"models", DataDir() / L"models"}; }

// Activates a.model with the right backend. Returns false if it cannot be used.
bool LoadModelIntoApp(App& a) {
    if (a.nbEngine.Running()) StopNbfc(a, L"model changed");
    a.nbfcMode = false;
    if (a.model.source != L"builtin")  // never trusted from the definition file itself
        a.model.confirmed = GetSetting(L"confirmed_" + ModelFingerprint(a.model)) != 0;
    if (a.model.backend == L"nbfc") {
        if (!LoadNbfcModel(a)) return false;
        a.ec.Configure(a.model.tableBase, a.model.readCmd, a.model.writeCmd);
        return true;
    }
    LoadProfiles(a);
    return true;
}

void CmdImportNbfc(App& a) {
    if (!a.scanned) {
        a.found = identify::Result{};
        identify::Fingerprint(a.found, a.wmi);
        a.scanned = true;
    }
    wchar_t name[MAX_PATH] = L"";
    OPENFILENAMEW o{sizeof(o)};
    o.hwndOwner = a.hwnd;
    o.lpstrFilter = L"NoteBook FanControl config (*.xml;*.json)\0*.xml;*.json\0All files (*.*)\0*.*\0\0";
    o.lpstrFile = name;
    o.nMaxFile = MAX_PATH;
    o.lpstrTitle = L"Choose your laptop model's NoteBook FanControl config";
    o.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&o)) return;
    nbfc::Config c;
    std::wstring error;
    if (!nbfc::Load(name, c, error)) {
        a.status = error;
        return;
    }
    a.found.nbfc = c;
    a.found.hasNbfc = true;
    identify::BuildNbfcDraft(a.found);
    AppLog(L"Identify: imported NBFC config " + std::wstring(name));
    a.status = L"Config loaded. Check the model name below, then click Use and Confirm.";
}

void CmdNbfcHelp(App& a) {
    const int r = MessageBoxW(
        a.hwnd,
        L"NoteBook FanControl (NBFC) is an open-source project whose users have written fan settings for hundreds "
        L"of laptop models. To use your model's settings:\n\n"
        L"1. Open the NBFC config list (Yes opens it in your browser):\n"
        L"     github.com/hirschmann/nbfc/tree/master/Configs  (.xml)\n"
        L"     or github.com/nbfc-linux/nbfc-linux/tree/main/share/nbfc/configs  (.json)\n"
        L"2. Find the file named after your laptop model (or the closest series).\n"
        L"3. Open it, click \"Download raw file\", and save it.\n"
        L"4. Click \"NBFC config...\" here and choose the saved file.\n\n"
        L"Configs written by other people are not checked by this app; the model name must match your laptop.\n\n"
        L"Open the config list now?",
        L"Getting your laptop's NBFC config", MB_YESNO | MB_ICONINFORMATION);
    if (r == IDYES)
        ShellExecuteW(a.hwnd, L"open", L"https://github.com/hirschmann/nbfc/tree/master/Configs", nullptr, nullptr, SW_SHOWNORMAL);
}

void CmdScan(App& a) {
    a.found = identify::Result{};
    identify::Fingerprint(a.found, a.wmi);
    identify::BuildDraft(a.found);
    a.scanned = true;
    AppLog(L"Identify: fingerprint taken.");
    a.status = L"Scan complete. Next, choose this laptop's BIOS update file from the manufacturer's website.";
}

void CmdChooseBios(App& a) {
    if (!a.scanned) CmdScan(a);
    wchar_t name[MAX_PATH] = L"";
    OPENFILENAMEW o{sizeof(o)};
    o.hwndOwner = a.hwnd;
    o.lpstrFilter = L"BIOS update (*.exe;*.zip;*.fd;*.bin;*.rom)\0*.exe;*.zip;*.fd;*.bin;*.rom\0All files (*.*)\0*.*\0\0";
    o.lpstrFile = name;
    o.nMaxFile = MAX_PATH;
    o.lpstrTitle = L"Choose the BIOS update for this laptop (it is only read, never run)";
    o.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&o)) return;
    a.status = L"Scanning the BIOS file...";
    InvalidateRect(a.hwnd, nullptr, FALSE);
    UpdateWindow(a.hwnd);
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    a.found.groups = identify::ScanBiosFile(name, a.identifyLog);
    a.found.notes.clear();
    identify::BuildDraft(a.found);
    AppLog(L"Identify: scanned " + std::wstring(name) + L", " + std::to_wstring(a.found.groups.size()) + L" table groups.");
    a.status = a.found.hasDraft ? L"Fan tables found. Click Use, then Confirm."
                                : L"No usable definition yet. Save the report and share it so this model can be added.";
}

void CmdUseDraft(App& a) {
    if (!a.found.hasDraft) return;
    ModelDef m = a.found.draft;
    const fs::path file = DataDir() / L"models" / (m.product + L".ini");
    m.source = file.wstring();
    if (m.backend == L"nbfc") {  // keep a private copy of the imported config next to the definition
        const fs::path copy = DataDir() / L"models" / (m.product + L"_nbfc" + fs::path(m.nbfcConfig).extension().wstring());
        std::error_code ec;
        fs::create_directories(copy.parent_path(), ec);
        if (fs::path(m.nbfcConfig) != copy) fs::copy_file(m.nbfcConfig, copy, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            a.status = L"Could not copy the NBFC config.";
            return;
        }
        m.nbfcConfig = copy.wstring();
    }
    if (!SaveModel(file, m)) {
        a.status = L"Could not save the model definition.";
        return;
    }
    a.model = m;
    a.hasModel = LoadModelIntoApp(a);
    if (!a.hasModel) return;
    AppLog(L"Identify: using draft definition " + file.wstring());
    a.status = L"Definition saved. Click Confirm to check it against the fan controller (read only).";
}

// Reads the live fan table once and matches it to the definition's factory tables.
// Fixes the profile mapping if the live table belongs to a different table slot.
void CmdConfirm(App& a) {
    if (!a.hasModel || a.model.confirmed) return;
    if (!a.ecOk) {
        a.status = L"Fan controller access is unavailable: " + a.ecError;
        return;
    }
    if (a.nbfcMode) {  // NBFC: read the fan registers once with the standard ACPI EC read command
        if (MessageBoxW(a.hwnd,
                        L"Confirm reads this config's fan registers once, using the standard ACPI read command that "
                        L"Windows itself uses. Nothing is written.\n\nContinue?",
                        L"Confirm with laptop", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        std::wstring readings;
        try {
            EcLock lock(1000);
            PawnPort port(a.ec);
            for (const auto& f : a.nb.fans)
                readings += f.name + L" " + std::to_wstring(std::lround(nbfc::Engine::ReadPercent(port, f))) + L"%   ";
        } catch (const std::exception& e) {
            a.status = L"Read failed: " + Widen(e.what());
            return;
        }
        a.identifyLog.push_back(L"Confirm: fan registers read: " + readings);
        a.model.confirmed = true;
        PutSetting(L"confirmed_" + ModelFingerprint(a.model), 1);
        AppLog(L"Identify: NBFC model confirmed; " + readings);
        a.status = L"Fan registers read: " + readings + L" Turn on your curve and listen to the fans the first time.";
        return;
    }
    if (MessageBoxW(a.hwnd,
                    L"EXPERIMENTAL: this laptop model has not been tested with Laptop Fan Curve.\n\n"
                    L"Confirm reads the active fan table from the embedded controller using the Acer EC-family read "
                    L"command. Nothing is written. It reads the first 10 bytes and stops at once if they do not look "
                    L"like a fan table.\n\nYour firmware matches the verified Acer layout, but that cannot prove the "
                    L"controller understands the command the same way. Continue?",
                    L"Confirm with laptop (experimental)", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
    Table live{};
    try {
        EcLock lock(1000);
        std::uint8_t row0[kRowSize];
        for (std::size_t i = 0; i < kRowSize; ++i) row0[i] = a.ec.ReadXram(static_cast<std::uint16_t>(a.ec.TableBase() + i));
        if (!safety::LooksLikeFanTableRow0(row0)) {
            Table first{};
            std::copy(std::begin(row0), std::end(row0), first.begin());
            a.identifyLog.push_back(L"Confirm: the first bytes are not a fan table: " + HexTable(first).substr(0, 30));
            AppLog(L"Identify confirm stopped: first row is not a fan table.");
            a.status = L"Stopped: the controller did not return a fan table. Nothing was written. Save a report and share it.";
            return;
        }
        live = a.ec.ReadTable();
    } catch (const std::exception& e) {
        a.status = L"Read failed: " + Widen(e.what());
        return;
    }
    unsigned code = 99;
    a.wmi.GetProfile(code);
    int match = -1;
    for (int i = 0; i < ProfileCount(a); ++i)
        if (a.profiles[static_cast<std::size_t>(i)].factory == live) match = i;
    if (match < 0) {
        a.identifyLog.push_back(L"Confirm: live table matched no BIOS table: " + HexTable(live));
        AppLog(L"Identify confirm mismatch: " + HexTable(live));
        a.status = L"The live table did not match the BIOS tables. Save the report and share it; nothing was written.";
        return;
    }
    const int current = IndexOfCode(a, code);
    if (current >= 0 && current != match) {
        // The live table belongs to another slot: swap names/codes so tables stay with their modes.
        ProfileDef& x = a.model.profiles[static_cast<std::size_t>(current)];
        ProfileDef& y = a.model.profiles[static_cast<std::size_t>(match)];
        std::swap(x.name, y.name);
        std::swap(x.code, y.code);
        std::swap(x.accent, y.accent);
        a.identifyLog.push_back(L"Confirm: corrected the mode mapping for code " + std::to_wstring(code) + L".");
    }
    a.model.confirmed = true;
    SaveModel(a.model.source, a.model);  // keeps a corrected mode mapping
    PutSetting(L"confirmed_" + ModelFingerprint(a.model), 1);
    LoadProfiles(a);
    for (Profile& p : a.profiles)
        if (p.code == code) {
            p.verified = Verify::Ok;
            SaveProfile(p);
        }
    AppLog(L"Identify: model confirmed against the live EC.");
    a.status = L"Confirmed. The live fan table matches the BIOS. Other modes are checked the first time they are used.";
}

void CmdSaveReport(App& a) {
    if (!a.scanned) CmdScan(a);
    wchar_t name[MAX_PATH] = L"LaptopFanCurve_identify_report.txt";
    OPENFILENAMEW o{sizeof(o)};
    o.hwndOwner = a.hwnd;
    o.lpstrFilter = L"Text report (*.txt)\0*.txt\0\0";
    o.lpstrFile = name;
    o.nMaxFile = MAX_PATH;
    o.lpstrDefExt = L"txt";
    o.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&o)) return;
    std::wofstream f(name);
    f << identify::Report(a.found, a.identifyLog);
    a.status = f.good() ? std::wstring(L"Report saved to ") + name : L"Could not save the report.";
}

// ===========================================================================
// Rendering helpers
// ===========================================================================
D2D1_COLOR_F Rgb(std::uint32_t rgb, float alpha = 1.f) { return ColorF(rgb, alpha); }
D2D1_RECT_F R(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
bool Inside(const D2D1_RECT_F& r, D2D1_POINT_2F p) { return p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom; }

constexpr std::uint32_t kIdentifyAccent = 0x60A5FA;
constexpr std::uint32_t kBg = 0x0E1014, kCard = 0x171A21, kCardHi = 0x1E222B, kBorder = 0x262B35,
                        kText = 0xE9ECF1, kMuted = 0x8A93A3, kGrid = 0x232833, kGpuColor = 0xA78BFA;

void Fill(App& a, const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c) {
    a.brush->SetColor(c);
    a.rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), a.brush.Get());
}
void Stroke(App& a, const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c, float w = 1.f) {
    a.brush->SetColor(c);
    a.rt->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), a.brush.Get(), w);
}
void Line(App& a, D2D1_POINT_2F p0, D2D1_POINT_2F p1, D2D1_COLOR_F c, float w = 1.f, ID2D1StrokeStyle* s = nullptr) {
    a.brush->SetColor(c);
    a.rt->DrawLine(p0, p1, a.brush.Get(), w, s);
}
void Circle(App& a, D2D1_POINT_2F c, float r, D2D1_COLOR_F col) {
    a.brush->SetColor(col);
    a.rt->FillEllipse(D2D1::Ellipse(c, r, r), a.brush.Get());
}
void Text(App& a, const std::wstring& s, const D2D1_RECT_F& r, IDWriteTextFormat* f, D2D1_COLOR_F c,
          DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
          DWRITE_PARAGRAPH_ALIGNMENT para = DWRITE_PARAGRAPH_ALIGNMENT_CENTER) {
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(para);
    a.brush->SetColor(c);
    a.rt->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), f, r, a.brush.Get());
}

void FanGlyph(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b, D2D1_POINT_2F c, float r, D2D1_COLOR_F blade,
              D2D1_COLOR_F hub) {
    D2D1_MATRIX_3X2_F old;
    rt->GetTransform(&old);
    b->SetColor(blade);
    for (int i = 0; i < 3; ++i) {
        rt->SetTransform(D2D1::Matrix3x2F::Rotation(120.f * i + 15.f, c) * *D2D1::Matrix3x2F::ReinterpretBaseType(&old));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x + r * 0.18f, c.y - r * 0.5f), r * 0.3f, r * 0.52f), b);
    }
    rt->SetTransform(old);
    b->SetColor(hub);
    rt->FillEllipse(D2D1::Ellipse(c, r * 0.2f, r * 0.2f), b);
}

void AddHit(App& a, D2D1_RECT_F r, int id, bool enabled = true) { a.hits.push_back({r, id, enabled}); }

void ButtonW(App& a, D2D1_RECT_F r, int id, const std::wstring& label, bool primary, bool enabled,
             std::uint32_t accent) {
    const bool hover = a.hoverId == id && enabled;
    D2D1_COLOR_F bg = primary ? Rgb(accent, enabled ? (hover ? 1.f : 0.9f) : 0.25f)
                              : Rgb(hover ? 0x2A303B : kCardHi, 1.f);
    Fill(a, r, 8, bg);
    if (!primary) Stroke(a, r, 8, Rgb(kBorder));
    Text(a, label, r, a.fBodyBold.Get(), primary ? Rgb(0x0B0D10, enabled ? 1.f : 0.6f) : Rgb(kText, enabled ? 1.f : 0.4f),
         DWRITE_TEXT_ALIGNMENT_CENTER);
    AddHit(a, r, id, enabled);
}

void Switch(App& a, D2D1_POINT_2F at, bool on, int id, std::uint32_t accent, const std::wstring& label) {
    const D2D1_RECT_F track = R(at.x, at.y, at.x + 42, at.y + 24);
    Fill(a, track, 12, on ? Rgb(accent) : Rgb(0x3A404C));
    Circle(a, D2D1::Point2F(on ? track.right - 12 : track.left + 12, at.y + 12), 9, Rgb(on ? 0x0B0D10 : 0xC9CED8));
    Text(a, label, R(at.x + 54, at.y - 2, at.x + 420, at.y + 26), a.fBody.Get(), Rgb(kText));
    AddHit(a, R(at.x, at.y - 2, at.x + 54 + 9.f * static_cast<float>(label.size()), at.y + 26), id);
}

// ===========================================================================
// Chart
// ===========================================================================
// ---------------------------------------------------------------------------
// Curve view: one editor for Acer tables (RPM) and NBFC fans (% speed).
// Points are 1-based. "factory" selects Acer's/the config's defaults.
// ---------------------------------------------------------------------------
struct Axis {
    int minValue, maxValue, step, grid;
    const wchar_t* unit;
    const wchar_t* zero;
    const wchar_t* defaults;
};
Axis CurveAxis(const App& a) {
    if (a.nbfcMode) return {0, 100, 1, 20, L"%", L"0%", L"Config"};
    return {kMinRpm, kChartRpm, 50, 1000, L" rpm", L"off", L"Acer"};
}
int FanCount(const App& a) { return a.nbfcMode ? static_cast<int>(a.nb.fans.size()) : 2; }
std::wstring FanName(const App& a, int f) {
    return a.nbfcMode ? a.nb.fans[static_cast<std::size_t>(f)].name : std::wstring(f ? L"GPU fan" : L"CPU fan");
}
const std::vector<nbfc::Threshold>& NbCurve(const App& a, int f, bool factory) {
    return factory ? a.nb.fans[static_cast<std::size_t>(f)].thresholds : a.nbDraft[static_cast<std::size_t>(f)];
}
const Table& AcerCurve(const App& a, bool factory) {
    const Profile& p = a.profiles[static_cast<std::size_t>(a.tab)];
    return factory ? p.factory : p.draft;
}
int CPoints(const App& a, int f, bool factory) {
    return a.nbfcMode ? static_cast<int>(NbCurve(a, f, factory).size()) : Stages(AcerCurve(a, factory), f);
}
int CTemp(const App& a, int f, int i, bool factory) {
    return a.nbfcMode ? NbCurve(a, f, factory)[static_cast<std::size_t>(i - 1)].up : PointTemp(AcerCurve(a, factory), f, i);
}
int CValue(const App& a, int f, int i, bool factory) {
    return a.nbfcMode ? static_cast<int>(std::lround(NbCurve(a, f, factory)[static_cast<std::size_t>(i - 1)].speed))
                      : PointRpm(AcerCurve(a, factory), f, i);
}
// Value before the first point: Acer row 0 (fans off); NBFC's first threshold applies from the start.
int CBase(const App& a, int f, bool factory) {
    return a.nbfcMode ? CValue(a, f, 1, factory) : RowRpm(AcerCurve(a, factory), 0, f == 1);
}
void CSet(App& a, int f, int i, int temp, int value) {
    if (a.nbfcMode) {
        nbfc::Threshold& t = a.nbDraft[static_cast<std::size_t>(f)][static_cast<std::size_t>(i - 1)];
        if (temp != t.up) {
            const nbfc::Threshold& d = a.nb.fans[static_cast<std::size_t>(f)].thresholds[static_cast<std::size_t>(i - 1)];
            t.up = temp;
            if (i > 1) t.down = std::clamp(temp - std::max(d.up - d.down, 1), 0, temp - 1);
        }
        if (value != CValue(a, f, i, false)) t.speed = value;
        return;
    }
    Profile& p = a.profiles[static_cast<std::size_t>(a.tab)];
    if (temp != PointTemp(p.draft, f, i)) SetPointTemp(p.draft, p.factory, f, i, temp);
    SetPointRpm(p.draft, f, i, value);
}
bool DraftIsDefault(const App& a) {
    return a.nbfcMode ? SameCurves(a.nbDraft, ConfigCurves(a.nb)) : AcerCurve(a, false) == AcerCurve(a, true);
}
bool DraftIsSaved(const App& a) {
    if (a.nbfcMode) return SameCurves(a.nbDraft, a.nbSaved);
    const Profile& p = a.profiles[static_cast<std::size_t>(a.tab)];
    return p.draft == Baseline(p);
}
std::uint32_t FanColor(const App& a, int f) {
    static const std::uint32_t others[] = {kGpuColor, 0xFB923C, 0x38BDF8, 0xE879F9};
    return f == 0 ? a.profiles[static_cast<std::size_t>(a.tab)].accent : others[(f - 1) % 4];
}

struct Plot {
    D2D1_RECT_F r;
    float tMin, tMax, yMax;
    float X(float t) const { return r.left + (t - tMin) / (tMax - tMin) * (r.right - r.left); }
    float Y(float v) const { return r.bottom - v / yMax * (r.bottom - r.top); }
    float T(float x) const { return tMin + (x - r.left) / (r.right - r.left) * (tMax - tMin); }
    float V(float y) const { return (r.bottom - y) / (r.bottom - r.top) * yMax; }
};

Plot ChartPlot(const App& a) {
    float lo = float(kMinTemp), hi = float(kMaxTemp);
    if (a.nbfcMode)
        for (int f = 0; f < FanCount(a); ++f)
            for (bool factory : {false, true})
                for (int i = 1; i <= CPoints(a, f, factory); ++i) {
                    lo = std::min(lo, std::floor(CTemp(a, f, i, factory) / 10.f) * 10.f);
                    hi = std::max(hi, std::ceil(CTemp(a, f, i, factory) / 10.f) * 10.f);
                }
    return {R(410, 180, 1150, 524), lo, hi, float(CurveAxis(a).maxValue)};
}

D2D1_POINT_2F PointPos(const App& a, const Plot& pl, int fan, int i, bool factory) {
    return D2D1::Point2F(pl.X(float(CTemp(a, fan, i, factory))), pl.Y(float(CValue(a, fan, i, factory))));
}

Microsoft::WRL::ComPtr<ID2D1PathGeometry> StepPath(App& a, const Plot& pl, int fan, bool factory, bool closed) {
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    a.d2d->CreatePathGeometry(path.GetAddressOf());
    path->Open(sink.GetAddressOf());
    const float base = pl.Y(float(CBase(a, fan, factory)));
    sink->BeginFigure(D2D1::Point2F(pl.X(pl.tMin), closed ? pl.r.bottom : base),
                      closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    if (closed) sink->AddLine(D2D1::Point2F(pl.X(pl.tMin), base));
    float lastY = base;
    for (int i = 1; i <= CPoints(a, fan, factory); ++i) {
        const D2D1_POINT_2F p = PointPos(a, pl, fan, i, factory);
        sink->AddLine(D2D1::Point2F(p.x, lastY));
        sink->AddLine(p);
        lastY = p.y;
    }
    sink->AddLine(D2D1::Point2F(pl.X(pl.tMax), lastY));
    if (closed) sink->AddLine(D2D1::Point2F(pl.X(pl.tMax), pl.r.bottom));
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    sink->Close();
    return path;
}

std::wstring ValueText(const App& a, int v) { return std::to_wstring(v) + CurveAxis(a).unit; }

void DrawChart(App& a) {
    Profile& p = a.profiles[static_cast<std::size_t>(a.tab)];
    const Plot pl = ChartPlot(a);
    const Axis ax = CurveAxis(a);
    const D2D1_RECT_F card = R(340, 100, 1180, 600);
    Fill(a, card, 14, Rgb(kCard));
    Stroke(a, card, 14, Rgb(kBorder));

    // Fan selector (right-aligned, up to four fans)
    Text(a, (a.nbfcMode ? std::wstring(L"NBFC") : p.name) + L" \u00B7 FAN CURVE", R(364, 112, 700, 136), a.fH.Get(), Rgb(kMuted));
    const int fans = std::min(FanCount(a), 4);
    for (int f = 0; f < fans; ++f) {
        const float left = 1158.f - 134.f * float(fans - f);
        const D2D1_RECT_F r = R(left, 110, left + 124, 140);
        const bool sel = a.fan == f;
        Fill(a, r, 15, sel ? Rgb(FanColor(a, f), 0.18f) : Rgb(a.hoverId == kFan0 + f ? 0x252A34 : kCardHi));
        Stroke(a, r, 15, sel ? Rgb(FanColor(a, f), 0.8f) : Rgb(kBorder));
        Circle(a, D2D1::Point2F(r.left + 18, (r.top + r.bottom) / 2), 4.5f, Rgb(FanColor(a, f)));
        Text(a, FanName(a, f), R(r.left + 28, r.top, r.right - 6, r.bottom), a.fBodyBold.Get(), Rgb(kText, sel ? 1.f : 0.7f));
        AddHit(a, r, kFan0 + f);
    }

    // Grid and axes
    for (int t = int(pl.tMin); t <= int(pl.tMax); t += 10) {
        Line(a, D2D1::Point2F(pl.X(float(t)), pl.r.top), D2D1::Point2F(pl.X(float(t)), pl.r.bottom), Rgb(kGrid));
        Text(a, std::to_wstring(t) + L"\u00B0", R(pl.X(float(t)) - 30, pl.r.bottom + 6, pl.X(float(t)) + 30, pl.r.bottom + 26),
             a.fSmall.Get(), Rgb(kMuted), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    for (int v = 0; v <= ax.maxValue; v += ax.grid) {
        Line(a, D2D1::Point2F(pl.r.left, pl.Y(float(v))), D2D1::Point2F(pl.r.right, pl.Y(float(v))), Rgb(kGrid));
        Text(a, v ? std::to_wstring(v) + (a.nbfcMode ? L"%" : L"") : std::wstring(ax.zero),
             R(pl.r.left - 58, pl.Y(float(v)) - 10, pl.r.left - 10, pl.Y(float(v)) + 10), a.fSmall.Get(), Rgb(kMuted),
             DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    Text(a, a.nbfcMode ? L"SPEED" : L"RPM", R(pl.r.left - 58, pl.r.top - 28, pl.r.left - 10, pl.r.top - 12), a.fSmall.Get(),
         Rgb(kMuted), DWRITE_TEXT_ALIGNMENT_TRAILING);

    // Curves: other fans faded, then the default reference, then the edited fan
    for (int f = 0; f < FanCount(a); ++f) {
        if (f == a.fan) continue;
        a.brush->SetColor(Rgb(FanColor(a, f), 0.35f));
        a.rt->DrawGeometry(StepPath(a, pl, f, false, false).Get(), a.brush.Get(), 2.f);
    }
    if (!DraftIsDefault(a)) {
        a.brush->SetColor(Rgb(0xC9CED8, 0.45f));
        a.rt->DrawGeometry(StepPath(a, pl, a.fan, true, false).Get(), a.brush.Get(), 1.5f, a.dash.Get());
    }
    const std::uint32_t color = FanColor(a, a.fan);
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> stops;
    const D2D1_GRADIENT_STOP gs[2] = {{0.f, Rgb(color, 0.28f)}, {1.f, Rgb(color, 0.0f)}};
    a.rt->CreateGradientStopCollection(gs, 2, stops.GetAddressOf());
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> grad;
    a.rt->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, pl.r.top), D2D1::Point2F(0, pl.r.bottom)), stops.Get(),
        grad.GetAddressOf());
    a.rt->FillGeometry(StepPath(a, pl, a.fan, false, true).Get(), grad.Get());
    a.brush->SetColor(Rgb(color));
    a.rt->DrawGeometry(StepPath(a, pl, a.fan, false, false).Get(), a.brush.Get(), 2.5f);

    // Live reading
    bool live = false;
    unsigned temp = 0, value = 0;
    if (a.nbfcMode && a.nbTempOk) {
        live = true;
        temp = a.nbTemp;
        if (static_cast<std::size_t>(a.fan) < a.nbRead.size()) value = static_cast<unsigned>(std::lround(a.nbRead[static_cast<std::size_t>(a.fan)]));
    } else if (!a.nbfcMode && a.tele.ok && a.laptopProfile == p.code) {
        live = true;
        temp = a.fan ? a.tele.gpuTemp : a.tele.cpuTemp;
        value = a.fan ? a.tele.gpuRpm : a.tele.cpuRpm;
    }
    if (live) {
        const float x = pl.X(std::clamp(float(temp), pl.tMin, pl.tMax));
        const float y = pl.Y(std::min(float(value), pl.yMax));
        Line(a, D2D1::Point2F(x, pl.r.top), D2D1::Point2F(x, pl.r.bottom), Rgb(0xFFFFFF, 0.35f), 1.f, a.dash.Get());
        Circle(a, D2D1::Point2F(x, y), 9, Rgb(0xFFFFFF, 0.15f));
        Circle(a, D2D1::Point2F(x, y), 4.5f, Rgb(0xFFFFFF));
        Text(a, L"Now " + std::to_wstring(temp) + L"\u00B0  " + ValueText(a, int(value)), R(x + 8, pl.r.top + 2, x + 200, pl.r.top + 22),
             a.fSmall.Get(), Rgb(0xFFFFFF, 0.8f));
    }

    // Points
    const int k = CPoints(a, a.fan, false);
    for (int i = 1; i <= k; ++i) {
        const D2D1_POINT_2F c = PointPos(a, pl, a.fan, i, false);
        const bool hot = i == a.hoverPoint || (a.dragging && i == a.selPoint);
        const bool sel = i == a.selPoint;
        if (hot || sel) Circle(a, c, hot ? 13.f : 11.f, Rgb(color, 0.22f));
        Circle(a, c, hot ? 7.5f : 6.f, Rgb(color));
        Circle(a, c, hot ? 3.5f : 2.5f, Rgb(kCard));
    }

    // Tooltip for the hovered or dragged point
    const int tipPoint = a.dragging ? a.selPoint : a.hoverPoint;
    if (tipPoint >= 1 && tipPoint <= k) {
        const D2D1_POINT_2F c = PointPos(a, pl, a.fan, tipPoint, false);
        const std::wstring s = std::to_wstring(CTemp(a, a.fan, tipPoint, false)) + L"\u00B0C  \u00B7  " +
                               ValueText(a, CValue(a, a.fan, tipPoint, false));
        float left = std::min(c.x - 80, pl.r.right - 160), top = c.y - 48;
        if (top < pl.r.top) top = c.y + 18;
        const D2D1_RECT_F tip = R(left, top, left + 160, top + 30);
        Fill(a, tip, 8, Rgb(0x2B313C));
        Stroke(a, tip, 8, Rgb(color, 0.6f));
        Text(a, s, tip, a.fBodyBold.Get(), Rgb(kText), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // Legend + selected point details
    const float ly = 562;
    const int sp = std::clamp(a.selPoint, 1, k);
    Circle(a, D2D1::Point2F(372, ly + 10), 4.5f, Rgb(color));
    Text(a, FanName(a, a.fan) + L" \u00B7 Stage " + std::to_wstring(sp) + L"   " + std::to_wstring(CTemp(a, a.fan, sp, false)) +
                L"\u00B0C  \u2192  " + ValueText(a, CValue(a, a.fan, sp, false)) + L"      " + ax.defaults + L": " +
                std::to_wstring(CTemp(a, a.fan, sp, true)) + L"\u00B0C  \u2192  " + ValueText(a, CValue(a, a.fan, sp, true)),
         R(384, ly, 1000, ly + 20), a.fBody.Get(), Rgb(kText));
    Text(a, L"Drag points \u00B7 arrow keys fine-tune", R(900, ly, 1160, ly + 20), a.fSmall.Get(), Rgb(kMuted),
         DWRITE_TEXT_ALIGNMENT_TRAILING);
}
// ===========================================================================
// Whole window
// ===========================================================================
void SensorCard(App& a, float top, const std::wstring& name, const std::wstring& big, bool hot, const std::wstring& right,
                const std::deque<unsigned>& hist, std::uint32_t color) {
    const D2D1_RECT_F card = R(20, top, 320, top + 150);
    Fill(a, card, 14, Rgb(kCard));
    Stroke(a, card, 14, Rgb(kBorder));
    Text(a, name, R(40, top + 14, 300, top + 34), a.fH.Get(), Rgb(kMuted));
    Text(a, big, R(38, top + 34, 200, top + 84), a.fBig.Get(), Rgb(hot ? 0xF43F5E : kText), DWRITE_TEXT_ALIGNMENT_LEADING);
    Text(a, right, R(150, top + 44, 300, top + 76), a.fNum.Get(), Rgb(color), DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (hist.size() >= 2) {
        const D2D1_RECT_F s = R(40, top + 96, 300, top + 136);
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        a.d2d->CreatePathGeometry(path.GetAddressOf());
        path->Open(sink.GetAddressOf());
        for (std::size_t i = 0; i < hist.size(); ++i) {
            const float x = s.left + (s.right - s.left) * float(i + 90 - hist.size()) / 89.f;
            const float v = std::clamp(float(hist[i]), 30.f, 100.f);
            const D2D1_POINT_2F pt = D2D1::Point2F(x, s.bottom - (v - 30.f) / 70.f * (s.bottom - s.top));
            if (i == 0) sink->BeginFigure(pt, D2D1_FIGURE_BEGIN_HOLLOW);
            else sink->AddLine(pt);
        }
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        a.brush->SetColor(Rgb(color, 0.9f));
        a.rt->DrawGeometry(path.Get(), a.brush.Get(), 1.8f);
        Text(a, L"3 min", R(240, top + 126, 300, top + 146), a.fSmall.Get(), Rgb(kMuted, 0.7f), DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
}

// ---------------------------------------------------------------------------
// Setup help: explains in plain words why the fan controller cannot be reached.
// ---------------------------------------------------------------------------
constexpr wchar_t kPawnIOPage[] = L"https://pawnio.eu";
struct SetupHelp {
    std::wstring title, text;
    bool offerDownload;
};
SetupHelp EcHelp(const App& a) {
    const std::wstring& e = a.ecError;
    auto has = [&](const wchar_t* s) { return e.find(s) != std::wstring::npos; };
    if (has(L"PawnIOLib.dll not found") || has(L"exports missing"))
        return {L"Setup needed: install PawnIO",
                L"Laptop Fan Curve uses PawnIO, a free signed driver, to talk to the fan controller. Download and install "
                L"it, then click Check again. The app also keeps checking by itself.",
                true};
    if (has(L"pawnio_open failed"))
        return {L"PawnIO is installed but its driver is not running",
                L"Restart Windows once after installing PawnIO. If this keeps happening, reinstall the latest PawnIO.", true};
    if (has(L"LpcACPIEC.bin"))
        return {L"LpcACPIEC.bin is missing",
                L"Keep LpcACPIEC.bin (it comes in the download zip) in the same folder as LaptopFanCurve.exe, then click "
                L"Check again.",
                false};
    if (has(L"rejected"))
        return {L"PawnIO did not accept its module", L"Update PawnIO to the latest version, then click Check again.", true};
    return {L"The fan controller cannot be reached", e, false};
}

void DrawSetupCard(App& a, const D2D1_RECT_F& r) {
    const SetupHelp h = EcHelp(a);
    Fill(a, r, 14, Rgb(0x2A2110));
    Stroke(a, r, 14, Rgb(0xF59E0B, 0.7f));
    Text(a, h.title, R(r.left + 24, r.top + 16, r.right - 24, r.top + 42), a.fBodyBold.Get(), Rgb(0xF59E0B));
    Text(a, h.text, R(r.left + 24, r.top + 46, r.right - 330, r.bottom - 12), a.fSmall.Get(), Rgb(kText, 0.9f),
         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    const float y = r.top + (r.bottom - r.top) / 2 - 18;
    if (h.offerDownload) ButtonW(a, R(r.right - 310, y, r.right - 150, y + 36), kGetPawnIO, L"Download PawnIO", true, true, 0xF59E0B);
    ButtonW(a, R(r.right - 140, y, r.right - 24, y + 36), kCheckAgain, L"Check again", false, true, 0);
}

// Identify screen: four steps, results summary, report.
void DrawIdentify(App& a) {
    const D2D1_RECT_F card = R(340, 100, 1180, 760);
    Fill(a, card, 14, Rgb(kCard));
    Stroke(a, card, 14, Rgb(kBorder));
    Text(a, L"Find where this laptop keeps its fan curves", R(364, 116, 1160, 144), a.fNum.Get(), Rgb(kText));
    Text(a, L"Acer laptops: Identify finds the fan tables in the manufacturer's BIOS update. Other laptops: import your "
            L"model's NoteBook FanControl (NBFC) config. Scanning never writes to the laptop. If neither works, save a "
            L"report and share it so your model can be added.",
         R(364, 148, 1160, 196), a.fSmall.Get(), Rgb(kMuted), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    const bool usingIdentified = a.hasModel && a.model.source != L"builtin";
    struct Step { const wchar_t* title; const wchar_t* text; int id; const wchar_t* button; bool done, enabled; };
    const Step steps[4] = {
        {L"Scan this laptop", L"Model, ACPI firmware tables and vendor WMI interfaces.", kScan, L"Scan", a.scanned, true},
        {L"Find the fan settings", L"Acer: BIOS update. Other brands: NBFC config.",
         kChooseBios, L"BIOS file...", !a.found.groups.empty() || a.found.hasNbfc, true},
        {L"Use the definition", L"Saves the fan tables found as this laptop's model definition.", kUseDraft, L"Use",
         usingIdentified, a.found.hasDraft},
        {usingIdentified ? L"Confirm with the laptop (experimental)" : L"Confirm with the laptop",
         L"Reads the live fan table once and checks it against the BIOS. Required before any curve is applied.",
         kConfirm, L"Confirm", a.hasModel && a.model.confirmed, a.hasModel && !a.model.confirmed && a.ecOk},
    };
    for (int i = 0; i < 4; ++i) {
        const float y = 208.f + 62.f * i;
        const Step& s = steps[i];
        Fill(a, R(364, y, 1160, y + 52), 10, Rgb(kCardHi));
        Circle(a, D2D1::Point2F(392, y + 26), 13, s.done ? Rgb(0x34D399) : Rgb(0x3A404C));
        Text(a, s.done ? std::wstring(L"\u2713") : std::to_wstring(i + 1), R(379, y + 13, 405, y + 39), a.fBodyBold.Get(),
             Rgb(s.done ? 0x0B0D10 : kText), DWRITE_TEXT_ALIGNMENT_CENTER);
        Text(a, s.title, R(420, y + 6, 980, y + 28), a.fBodyBold.Get(), Rgb(kText));
        Text(a, s.text, R(420, y + 27, 990, y + 48), a.fSmall.Get(), Rgb(kMuted));
        ButtonW(a, R(1004, y + 9, 1148, y + 43), s.id, s.button, !s.done && s.enabled, s.enabled, kIdentifyAccent);
        if (s.id == kChooseBios) {
            ButtonW(a, R(852, y + 9, 994, y + 43), kImportNbfc, L"NBFC config...", !s.done, true, kIdentifyAccent);
            ButtonW(a, R(784, y + 9, 842, y + 43), kNbfcHelp, L"How?", false, true, 0);
        }
    }

    Text(a, L"RESULTS", R(364, 462, 600, 482), a.fH.Get(), Rgb(kMuted));
    std::vector<std::wstring> lines;
    if (a.scanned) lines = identify::Summary(a.found);
    else lines.push_back(L"Click Scan to start.");
    for (std::size_t i = a.identifyLog.size() > 3 ? a.identifyLog.size() - 3 : 0; i < a.identifyLog.size(); ++i)
        lines.push_back(a.identifyLog[i]);
    for (std::size_t i = 0; i < lines.size() && i < 10; ++i)
        Text(a, lines[i], R(364, 488.f + 21.f * i, 1160, 508.f + 21.f * i), a.fSmall.Get(), Rgb(kText, 0.85f));

    ButtonW(a, R(364, 712, 520, 746), kSaveReport, L"Save report...", false, true, 0);
    if (!a.ecOk) {
        const SetupHelp h = EcHelp(a);
        Text(a, h.title, R(536, 712, 850, 746), a.fBodyBold.Get(), Rgb(0xF59E0B));
        if (h.offerDownload) ButtonW(a, R(860, 712, 1010, 746), kGetPawnIO, L"Download PawnIO", true, true, 0xF59E0B);
        ButtonW(a, R(1020, 712, 1148, 746), kCheckAgain, L"Check again", false, true, 0);
    } else {
        Text(a, a.status, R(536, 712, 1160, 746), a.fSmall.Get(), Rgb(kMuted));
    }
}

void Paint(App& a) {
    a.hits.clear();
    a.rt->BeginDraw();
    a.rt->Clear(Rgb(kBg));
    const bool curves = a.hasModel && !a.identifyView && !a.profiles.empty();
    const int active = IndexOfCode(a, a.laptopProfile);
    const std::uint32_t activeAccent = active >= 0 ? a.profiles[static_cast<std::size_t>(active)].accent : kIdentifyAccent;

    // Header
    Circle(a, D2D1::Point2F(42, 46), 20, Rgb(activeAccent, 0.16f));
    FanGlyph(a.rt.Get(), a.brush.Get(), D2D1::Point2F(42, 46), 15, Rgb(activeAccent), Rgb(kBg));
    Text(a, L"Laptop Fan Curve", R(74, 26, 330, 52), a.fTitle.Get(), Rgb(kText));
    Text(a, a.hasModel ? a.model.title + (!a.model.confirmed ? L" \u00B7 unconfirmed" : a.model.source != L"builtin" ? L" \u00B7 experimental" : L"")
                       : std::wstring(L"Laptop not identified yet"),
         R(75, 52, 330, 70), a.fSmall.Get(), Rgb(kMuted));

    // Profile tabs (or the Identify title bar)
    const D2D1_RECT_F seg = R(340, 30, 1180, 82);
    Fill(a, seg, 12, Rgb(kCard));
    Stroke(a, seg, 12, Rgb(kBorder));
    const int count = curves ? ProfileCount(a) : 0;
    if (!curves) {
        Text(a, L"Identify this laptop", R(364, 30, 900, 82), a.fBodyBold.Get(), Rgb(kText));
        if (a.hasModel) ButtonW(a, R(1010, 38, 1172, 74), kBack, L"Back to fan curves", false, true, 0);
    }
    const float tabW = (seg.right - seg.left - 8) / float(std::max(count, 1));
    for (int i = 0; i < count; ++i) {
        const Profile& q = a.profiles[static_cast<std::size_t>(i)];
        const D2D1_RECT_F r = R(seg.left + 4 + tabW * i, seg.top + 4, seg.left + 4 + tabW * (i + 1), seg.bottom - 4);
        if (a.tab == i) {
            Fill(a, r, 9, Rgb(q.accent, 0.16f));
            Stroke(a, r, 9, Rgb(q.accent, 0.7f));
        } else if (a.hoverId == kTab0 + i) {
            Fill(a, r, 9, Rgb(kCardHi));
        }
        Text(a, q.name, R(r.left, r.top, r.right, r.bottom - 12), a.fBodyBold.Get(),
             Rgb(a.tab == i ? q.accent : kText), DWRITE_TEXT_ALIGNMENT_CENTER);
        std::wstring sub = q.enabled ? L"Custom" : a.nbfcMode ? L"Firmware" : L"Acer";
        if (i == active) sub = L"\u25CF Active \u00B7 " + sub;
        Text(a, sub, R(r.left, r.top + 22, r.right, r.bottom), a.fSmall.Get(), Rgb(i == active ? q.accent : kMuted),
             DWRITE_TEXT_ALIGNMENT_CENTER);
        AddHit(a, r, kTab0 + i);
    }

    // Sidebar
    const std::uint32_t mainColor = curves ? a.profiles[static_cast<std::size_t>(a.tab)].accent : kIdentifyAccent;
    auto degrees = [](bool ok, unsigned t) { return ok ? std::to_wstring(t) + L"\u00B0" : std::wstring(L"\u2014"); };
    auto rpmText = [](bool ok, unsigned r) { return !ok ? std::wstring() : r ? std::to_wstring(r) + L" rpm" : std::wstring(L"fan off"); };
    if (a.nbfcMode) {
        auto pct = [&](std::size_t f) {
            return f < a.nbRead.size() ? std::to_wstring(std::lround(a.nbRead[f])) + L"%" : std::wstring(L"\u2014");
        };
        SensorCard(a, 100, a.tele.ok ? L"CPU" : L"TEMPERATURE (ACPI)", degrees(a.nbTempOk, a.nbTemp), a.nbTemp >= 90,
                   a.nb.fans.empty() ? L"" : pct(0), a.cpuHist, mainColor);
        const std::wstring second = a.nb.fans.size() > 1 ? a.nb.fans[1].name : std::wstring(L"FAN");
        SensorCard(a, 264, second, a.nb.fans.size() > 1 ? pct(1) : L"\u2014", false, L"", {}, kGpuColor);
    } else {
        SensorCard(a, 100, L"CPU", degrees(a.tele.ok, a.tele.cpuTemp), a.tele.cpuTemp >= 90, rpmText(a.tele.ok, a.tele.cpuRpm),
                   a.cpuHist, mainColor);
        SensorCard(a, 264, L"GPU", degrees(a.tele.ok, a.tele.gpuTemp), a.tele.gpuTemp >= 90, rpmText(a.tele.ok, a.tele.gpuRpm),
                   a.gpuHist, kGpuColor);
    }

    const D2D1_RECT_F pc = R(20, 428, 320, 540);
    Fill(a, pc, 14, Rgb(kCard));
    Stroke(a, pc, 14, Rgb(kBorder));
    Text(a, a.nbfcMode ? L"FAN CONTROL" : L"LAPTOP MODE", R(40, 442, 300, 462), a.fH.Get(), Rgb(kMuted));
    if (a.nbfcMode && !a.profiles.empty()) {
        const Profile& q = a.profiles[0];
        const bool custom = q.enabled && q.applied;
        Text(a, custom ? L"Your curve" : L"Firmware", R(40, 464, 300, 496), a.fNum.Get(), Rgb(custom ? q.accent : kMuted));
        Circle(a, D2D1::Point2F(46, 518), 4, Rgb(custom ? 0x34D399 : kMuted));
        Text(a, custom ? L"NBFC config is running" : (q.enabled ? L"Starting..." : L"The laptop controls its fans"),
             R(58, 508, 300, 528), a.fBody.Get(), Rgb(kText, 0.85f));
    } else if (active >= 0) {
        const Profile& q = a.profiles[static_cast<std::size_t>(active)];
        Text(a, q.name, R(40, 464, 300, 496), a.fNum.Get(), Rgb(q.accent));
        const bool custom = q.enabled && q.applied;
        Circle(a, D2D1::Point2F(46, 518), 4, Rgb(custom ? 0x34D399 : kMuted));
        Text(a, custom ? L"Your curve is running" : (q.enabled ? L"Applying your curve..." : L"Acer's curve is running"),
             R(58, 508, 300, 528), a.fBody.Get(), Rgb(kText, 0.85f));
    } else {
        Text(a, a.wmiOk ? L"Unknown" : L"Unavailable", R(40, 464, 300, 496), a.fNum.Get(), Rgb(kMuted));
    }

    const D2D1_RECT_F sc = R(20, 556, 320, 760);
    Fill(a, sc, 14, Rgb(kCard));
    Stroke(a, sc, 14, Rgb(kBorder));
    Text(a, L"SETTINGS", R(40, 570, 300, 590), a.fH.Get(), Rgb(kMuted));
    Switch(a, D2D1::Point2F(40, 602), a.startup, kToggleStartup, activeAccent == kMuted ? 0x60A5FA : activeAccent,
           L"Start with Windows");
    ButtonW(a, R(40, 636, 300, 668), kRestoreAll, a.nbfcMode ? L"Restore firmware control" : L"Restore all Acer curves", false,
            a.hasModel, 0);
    ButtonW(a, R(40, 676, 300, 708), kIdentifyView, L"Identify laptop...", false, !a.identifyView, 0);
    Text(a, a.nbfcMode ? L"Fans go to 100% above the critical temperature. Exiting returns them to the firmware."
                       : L"Acer's curve returns automatically if a quieter custom curve reaches 95\u00B0C.",
         R(40, 716, 300, 756), a.fSmall.Get(), Rgb(kMuted), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    if (!curves) {
        DrawIdentify(a);
        if (a.rt->EndDraw() == D2DERR_RECREATE_TARGET) {
            a.rt.Reset();
            a.brush.Reset();
        }
        return;
    }
    Profile& p = a.profiles[static_cast<std::size_t>(a.tab)];

    // Chart and actions
    DrawChart(a);
    if (!a.ecOk) {  // nothing below works without the fan controller: show why instead
        DrawSetupCard(a, R(340, 616, 1180, 760));
        if (a.rt->EndDraw() == D2DERR_RECREATE_TARGET) {
            a.rt.Reset();
            a.brush.Reset();
        }
        return;
    }

    const D2D1_RECT_F ac = R(340, 616, 1180, 760);
    Fill(a, ac, 14, Rgb(kCard));
    Stroke(a, ac, 14, Rgb(kBorder));
    const bool mismatch = p.verified == Verify::Mismatch;
    Switch(a, D2D1::Point2F(364, 640), p.enabled, kToggleCustom, p.accent,
           a.nbfcMode ? std::wstring(L"Use my curve") : std::wstring(L"Use my curve in ") + p.name + L" mode");
    const bool dirty = !DraftIsSaved(a);
    const bool canApply = !mismatch && a.ecOk && (dirty || !p.enabled);
    ButtonW(a, R(a.nbfcMode ? 892.f : 760.f, 632, a.nbfcMode ? 1030.f : 880.f, 672), kReset, std::wstring(CurveAxis(a).defaults) + L" values", false,
            !DraftIsDefault(a), 0);
    if (!a.nbfcMode)
        ButtonW(a, R(892, 632, 1030, 672), kSwitchTo, std::wstring(L"Use ") + p.name + L" now", false,
                a.wmiOk && a.laptopProfile != p.code, 0);
    ButtonW(a, R(1042, 632, 1160, 672), kApply, dirty || !p.enabled ? L"Apply" : L"Applied", true, canApply, p.accent);

    std::wstring info;
    if (!a.ecOk) info = L"Fan controller access unavailable: " + a.ecError;
    else if (mismatch) info = L"Acer's table for this mode did not match the laptop, so custom curves are disabled for it.";
    else if (!a.model.confirmed) info = L"This identified model is not confirmed yet. Open Identify and click Confirm.";
    else if (p.verified == Verify::Unknown)
        info = std::wstring(L"Acer's ") + p.name + L" table will be checked against the laptop the first time it is used.";
    else if (p.tripped) info = L"Watchdog restored Acer's curve. Click Apply to use yours again.";
    else if (dirty) info = L"Unsaved changes. Click Apply to use them.";
    else if (a.nbfcMode) info = p.enabled ? L"Your curve is saved and running." : L"The laptop's firmware controls the fans.";
    else info = std::wstring(p.enabled ? L"Your curve is saved and active for " : L"Acer's curve is used for ") + p.name + L".";
    Text(a, info, R(364, 684, 1160, 708), a.fBody.Get(), Rgb(mismatch || !a.ecOk ? 0xF59E0B : kText, 0.9f));
    Text(a, a.status, R(364, 712, 1160, 740), a.fSmall.Get(), Rgb(kMuted));

    if (a.rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        a.rt.Reset();
        a.brush.Reset();
    }
}

// ===========================================================================
// Setup
// ===========================================================================
void CreateFormats(App& a) {
    auto make = [&](const wchar_t* family, float size, DWRITE_FONT_WEIGHT w, Microsoft::WRL::ComPtr<IDWriteTextFormat>& out) {
        a.dw->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us",
                               out.GetAddressOf());
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    };
    make(L"Segoe UI Variable Display", 21.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, a.fTitle);
    make(L"Segoe UI Variable Display", 40.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, a.fBig);
    make(L"Segoe UI Variable Display", 20.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, a.fNum);
    make(L"Segoe UI", 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, a.fH);
    make(L"Segoe UI", 14.f, DWRITE_FONT_WEIGHT_NORMAL, a.fBody);
    make(L"Segoe UI", 14.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, a.fBodyBold);
    make(L"Segoe UI", 12.f, DWRITE_FONT_WEIGHT_NORMAL, a.fSmall);
    a.fSmall->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
}

void EnsureTarget(App& a) {
    if (a.rt) return;
    RECT rc;
    GetClientRect(a.hwnd, &rc);
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), float(a.dpi),
                                                    float(a.dpi));
    a.d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(a.hwnd, D2D1::SizeU(UINT32(rc.right), UINT32(rc.bottom))),
                                  a.rt.GetAddressOf());
    a.rt->CreateSolidColorBrush(Rgb(kText), a.brush.GetAddressOf());
}

HICON MakeIcon(App& a, int size, std::uint32_t accent) {
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, color);
    const auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dcrt;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> b;
    if (SUCCEEDED(a.d2d->CreateDCRenderTarget(&props, dcrt.GetAddressOf()))) {
        RECT rc{0, 0, size, size};
        dcrt->BindDC(dc, &rc);
        dcrt->BeginDraw();
        dcrt->Clear(ColorF(0, 0.f));
        dcrt->CreateSolidColorBrush(Rgb(accent), b.GetAddressOf());
        const float h = size / 2.f;
        dcrt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(h, h), h - 0.5f, h - 0.5f), b.Get());
        FanGlyph(dcrt.Get(), b.Get(), D2D1::Point2F(h, h), h * 0.78f, Rgb(0x0B0D10), Rgb(accent));
        dcrt->EndDraw();
    }
    SelectObject(dc, old);
    DeleteDC(dc);
    std::vector<BYTE> zeros(static_cast<std::size_t>(size * size / 8 + size), 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros.data());
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

void ShowFromTray(App& a) {
    ShowWindow(a.hwnd, SW_SHOW);
    ShowWindow(a.hwnd, SW_RESTORE);
    SetForegroundWindow(a.hwnd);
}

bool AnyEnabled(const App& a) {
    for (const Profile& p : a.profiles)
        if (p.enabled) return true;
    return false;
}

void UpdateTrayTip(App& a) {
    std::wstring tip = L"Laptop Fan Curve";
    const int idx = IndexOfCode(a, a.laptopProfile);
    if (idx >= 0) {
        const Profile& p = a.profiles[static_cast<std::size_t>(idx)];
        tip += std::wstring(L"\n") + p.name + (p.enabled && p.applied ? L" \u00B7 your curve" : L" \u00B7 Acer curve");
    }
    if (a.tele.ok) tip += L"\nCPU " + std::to_wstring(a.tele.cpuTemp) + L"\u00B0  GPU " + std::to_wstring(a.tele.gpuTemp) + L"\u00B0";
    a.tray.uFlags = NIF_TIP;
    wcsncpy_s(a.tray.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &a.tray);
}

D2D1_POINT_2F ToDip(App& a, LPARAM lp) {
    const float s = 96.f / float(a.dpi);
    return D2D1::Point2F(float(GET_X_LPARAM(lp)) * s, float(GET_Y_LPARAM(lp)) * s);
}

bool CurvesShown(const App& a) { return a.hasModel && !a.identifyView && !a.profiles.empty(); }

int PointAt(App& a, D2D1_POINT_2F m) {
    if (!CurvesShown(a)) return 0;
    const Plot pl = ChartPlot(a);
    int best = 0;
    float bestD = 12.f * 12.f;
    for (int i = 1; i <= CPoints(a, a.fan, false); ++i) {
        const D2D1_POINT_2F c = PointPos(a, pl, a.fan, i, false);
        const float d = (c.x - m.x) * (c.x - m.x) + (c.y - m.y) * (c.y - m.y);
        if (d <= bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

// Moves point i of the edited fan. Temperatures stay in order; the curve stays non-decreasing
// (points to the left are lowered, points to the right are raised).
void MovePoint(App& a, int i, int temp, int value) {
    const Plot pl = ChartPlot(a);
    const Axis ax = CurveAxis(a);
    const int k = CPoints(a, a.fan, false);
    int lo = int(pl.tMin), hi = int(pl.tMax);
    if (i > 1) lo = std::max(lo, CTemp(a, a.fan, i - 1, false) + 1);
    if (i < k) hi = std::min(hi, CTemp(a, a.fan, i + 1, false) - 1);
    // Leave the temperature alone on vertical-only edits (some vendor points are out of order).
    const int current = CTemp(a, a.fan, i, false);
    const int newTemp = (lo <= hi && temp != current) ? std::clamp(temp, lo, hi) : current;
    value = std::clamp(value, ax.minValue, ax.maxValue);
    CSet(a, a.fan, i, newTemp, value);
    for (int j = 1; j < i; ++j)
        if (CValue(a, a.fan, j, false) > value) CSet(a, a.fan, j, CTemp(a, a.fan, j, false), value);
    for (int j = i + 1; j <= k; ++j)
        if (CValue(a, a.fan, j, false) < value) CSet(a, a.fan, j, CTemp(a, a.fan, j, false), value);
}
void Command(App& a, int id) {
    if (id == kIdentifyView) {
        a.identifyView = true;
        a.status.clear();
    } else if (id == kBack) {
        a.identifyView = false;
        a.status.clear();
    } else if (id == kScan) {
        CmdScan(a);
    } else if (id == kChooseBios) {
        CmdChooseBios(a);
    } else if (id == kUseDraft) {
        CmdUseDraft(a);
    } else if (id == kConfirm) {
        CmdConfirm(a);
    } else if (id == kSaveReport) {
        CmdSaveReport(a);
    } else if (id == kGetPawnIO) {
        ShellExecuteW(a.hwnd, L"open", kPawnIOPage, nullptr, nullptr, SW_SHOWNORMAL);
    } else if (id == kCheckAgain) {
        ConnectEc(a);
        a.status = a.ecOk ? L"Connected to the fan controller." : L"Still not available.";
    } else if (id == kImportNbfc) {
        CmdImportNbfc(a);
    } else if (id == kNbfcHelp) {
        CmdNbfcHelp(a);
    } else if (id >= kTab0 && id < kTab0 + ProfileCount(a)) {
        a.tab = id - kTab0;
        a.selPoint = std::clamp(a.selPoint, 1, CPoints(a, a.fan, false));
        a.status.clear();
    } else if (id >= kFan0 && id < kFan0 + FanCount(a)) {
        a.fan = id - kFan0;
        a.selPoint = std::clamp(a.selPoint, 1, CPoints(a, a.fan, false));
    } else if (id == kApply) {
        CmdApply(a);
    } else if (id == kToggleCustom) {
        CmdToggleCustom(a);
    } else if (id == kReset) {
        if (a.nbfcMode) {
            a.nbDraft = ConfigCurves(a.nb);
        } else {
            Profile& p = a.profiles[static_cast<std::size_t>(a.tab)];
            p.draft = p.factory;
        }
        a.status = std::wstring(CurveAxis(a).defaults) + L" values loaded. Click Apply to use them.";
    } else if (id == kSwitchTo) {
        CmdSwitchTo(a);
    } else if (id == kRestoreAll) {
        CmdRestoreAll(a);
    } else if (id == kToggleStartup) {
        if (SetStartupTask(!a.startup)) a.startup = !a.startup;
        else a.status = L"Could not change the Windows startup task.";
    }
    InvalidateRect(a.hwnd, nullptr, FALSE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* a = g;
    if (!a) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_CREATE: {
        a->hwnd = hwnd;
        a->dpi = GetDpiForWindow(hwnd);
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
        const COLORREF caption = RGB((kBg >> 16) & 0xff, (kBg >> 8) & 0xff, kBg & 0xff);
        DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
        a->icon = MakeIcon(*a, GetSystemMetricsForDpi(SM_CXICON, a->dpi), 0x2DD4BF);
        a->smallIcon = MakeIcon(*a, GetSystemMetricsForDpi(SM_CXSMICON, a->dpi), 0x2DD4BF);
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(a->icon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(a->smallIcon));
        a->tray.cbSize = sizeof(a->tray);
        a->tray.hWnd = hwnd;
        a->tray.uID = 1;
        a->tray.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
        a->tray.uCallbackMessage = WM_TRAY;
        a->tray.hIcon = a->smallIcon;
        wcscpy_s(a->tray.szTip, L"Laptop Fan Curve");
        Shell_NotifyIconW(NIM_ADD, &a->tray);
        Tick(*a);
        const int idx = IndexOfCode(*a, a->laptopProfile);
        if (idx >= 0) a->tab = idx;
        SetTimer(hwnd, kTimer, 2000, nullptr);
        SetTimer(hwnd, kNbTimer, 500, nullptr);
        return 0;
    }
    case WM_TIMER:
        if (wp == kNbTimer) {
            NbTick(*a);  // keeps running while points are dragged
            return 0;
        }
        if (!a->dragging) {
            Tick(*a);
            UpdateTrayTip(*a);
            if (IsWindowVisible(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EnsureTarget(*a);
        if (a->rt) Paint(*a);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (a->rt) a->rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        return 0;
    case WM_DPICHANGED: {
        a->dpi = HIWORD(wp);
        const RECT* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        a->rt.Reset();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const D2D1_POINT_2F m = ToDip(*a, lp);
        if (a->dragging) {
            const Plot pl = ChartPlot(*a);
            const int step = CurveAxis(*a).step;
            const int temp = int(std::lround(pl.T(m.x)));
            const int value = int(std::lround(pl.V(m.y) / float(step))) * step;
            MovePoint(*a, a->selPoint, temp, value);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        int hp = PointAt(*a, m), hid = kNone;
        if (!hp)
            for (const Hit& h : a->hits)
                if (h.enabled && Inside(h.r, m)) hid = h.id;
        if (hp != a->hoverPoint || hid != a->hoverId) {
            a->hoverPoint = hp;
            a->hoverId = hid;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE:
        a->hoverPoint = 0;
        a->hoverId = kNone;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, a->dragging ? IDC_SIZEALL : (a->hoverPoint || a->hoverId) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        const D2D1_POINT_2F m = ToDip(*a, lp);
        SetFocus(hwnd);
        if (const int hp = PointAt(*a, m)) {
            a->selPoint = hp;
            a->dragging = true;
            SetCapture(hwnd);
        } else {
            a->pressedId = a->hoverId;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        if (a->dragging) {
            a->dragging = false;
            ReleaseCapture();
        } else if (a->pressedId != kNone && a->pressedId == a->hoverId) {
            const int id = a->pressedId;
            a->pressedId = kNone;
            Command(*a, id);
        }
        a->pressedId = kNone;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN: {
        if (!CurvesShown(*a)) return 0;
        const int k = CPoints(*a, a->fan, false);
        a->selPoint = std::clamp(a->selPoint, 1, k);
        const int step = CurveAxis(*a).step;
        int t = CTemp(*a, a->fan, a->selPoint, false), r = CValue(*a, a->fan, a->selPoint, false);
        switch (wp) {
        case VK_LEFT: --t; break;
        case VK_RIGHT: ++t; break;
        case VK_UP: r += step; break;
        case VK_DOWN: r -= step; break;
        case VK_TAB: a->selPoint = a->selPoint % k + 1; InvalidateRect(hwnd, nullptr, FALSE); return 0;
        default: return 0;
        }
        MovePoint(*a, a->selPoint, t, r);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
            a->lastFullVerify = 0;
            if (a->nbEngine.Running()) a->nbEngine.Start(a->nb, a->nbSaved);  // re-run the config's initialization
        }
        return TRUE;
    case WM_TRAY:
        if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) ShowFromTray(*a);
        if (lp == WM_RBUTTONUP) {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, kTrayOpen, L"Open");
            AppendMenuW(m, MF_STRING, kTrayRestoreExit, a->nbfcMode ? L"Restore firmware control and exit" : L"Restore Acer curves and exit");
            AppendMenuW(m, MF_STRING, kTrayExit, a->nbfcMode ? L"Exit (firmware takes over)" : L"Exit (keep current fan table)");
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(m);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == kTrayOpen) ShowFromTray(*a);
        if (LOWORD(wp) == kTrayRestoreExit) {
            CmdRestoreAll(*a);
            a->quitting = true;
            DestroyWindow(hwnd);
        }
        if (LOWORD(wp) == kTrayExit) {
            a->quitting = true;
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        if (AnyEnabled(*a) && !a->quitting) {
            ShowWindow(hwnd, SW_HIDE);
            if (!a->trayHintShown) {
                Notify(*a, L"Still running", L"Laptop Fan Curve keeps your curves applied from the tray. "
                                             L"Right-click the icon to exit.");
                a->trayHintShown = true;
            }
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kTimer);
        KillTimer(hwnd, kNbTimer);
        StopNbfc(*a, L"app closing");  // NBFC curves need the app running, so hand the fans back
        Shell_NotifyIconW(NIM_DELETE, &a->tray);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\LaptopFanCurveApp");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
            ShowWindow(existing, SW_SHOW);
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
                         EOAC_NONE, nullptr);
    int rc = 0;
    {
        App app;
        g = &app;
        app.startHidden = cmdLine && wcsstr(cmdLine, L"/tray");
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, app.d2d.GetAddressOf());
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(app.dw.GetAddressOf()));
        const float dashes[2] = {4.f, 4.f};
        app.d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                               D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_ROUND, 10.f,
                                                               D2D1_DASH_STYLE_CUSTOM),
                                   dashes, 2, app.dash.GetAddressOf());
        CreateFormats(app);
        app.wmiOk = app.wmi.Connect();
        ConnectEc(app);
        app.hasModel = FindModel(ModelDirs(), app.model);
        if (app.hasModel) app.hasModel = LoadModelIntoApp(app);
        if (!app.hasModel) app.identifyView = true;
        app.startup = StartupTaskExists();
        AppLog(std::wstring(L"Laptop Fan Curve started") + (app.startHidden ? L" (tray)" : L""));

        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = instance;
        wc.lpszClassName = kWindowClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        const UINT dpi = GetDpiForSystem();
        RECT r{0, 0, LONG(kWinW * dpi / 96.f), LONG(kWinH * dpi / 96.f)};
        AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
        HWND hwnd = CreateWindowExW(0, kWindowClass, L"Laptop Fan Curve 0.1.2 (beta)", style, CW_USEDEFAULT, CW_USEDEFAULT,
                                    r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance, nullptr);
        if (!hwnd) {
            rc = 1;
        } else {
            ShowWindow(hwnd, app.startHidden ? SW_HIDE : show);
            MSG m;
            while (GetMessageW(&m, nullptr, 0, 0) > 0) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
        AppLog(L"Laptop Fan Curve exited.");
        if (app.icon) DestroyIcon(app.icon);
        if (app.smallIcon) DestroyIcon(app.smallIcon);
        g = nullptr;
    }
    CoUninitialize();
    if (single) CloseHandle(single);
    return rc;
}
