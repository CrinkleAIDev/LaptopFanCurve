// Live-hardware layer: embedded-controller access through PawnIO and Acer WMI.
// - EC XRAM access through the official signed PawnIO LpcACPIEC module (0x92 read, 0x93 write).
// - Acer WMI: profile read, factory-profile reload (APGeAction.SetFunction), telemetry.
// This code is not affiliated with PawnIO or Acer.
//
// Port-collision notes (from the Acer Swift X 16 SFX16-61G DSDT):
// ACPI.sys only uses ports 0x62/0x66 for EC event queries (0x84) after an SCI;
// every other AML EC access goes through the 0xFE708xxx memory window. Each
// transaction therefore waits for IBF, OBF and SCI_EVT to be clear first, every
// write is verified by readback, and callers restore the factory table through
// WMI if anything fails.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Wbemidl.h>
#include <combaseapi.h>
#include <oleauto.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "advapi32.lib")

namespace acer {

using Microsoft::WRL::ComPtr;

constexpr std::uint16_t kTableBase = 0x0800;
constexpr std::size_t kTableSize = 120;
constexpr std::size_t kRowSize = 10;
using Table = std::array<std::uint8_t, kTableSize>;

// Factory Silent table: BIOS V1.56 EC image, matched 120/120 on this machine
// (Ubuntu and Windows reads).
constexpr Table kFactorySilent = {
    0x00, 0xff, 0x30, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x32, 0x2d, 0x32, 0x2d, 0x05, 0xdc, 0x06, 0xcc,
    0x02, 0x05, 0x34, 0x2f, 0x34, 0x2f, 0x06, 0x72, 0x07, 0x6c,
    0x03, 0x05, 0x37, 0x31, 0x37, 0x31, 0x07, 0x08, 0x08, 0x34,
    0x04, 0xff, 0x3c, 0x33, 0x3c, 0x33, 0x07, 0xd0, 0x08, 0x98,
    0x05, 0xff, 0x7f, 0x36, 0x7f, 0x36, 0x08, 0xfc, 0x09, 0x60,
    0x06, 0xff, 0x7f, 0x7f, 0x7f, 0x7f, 0x1f, 0x40, 0x1f, 0x40,
    0x07, 0xff, 0x7f, 0x7f, 0x7f, 0x7f, 0x1f, 0x40, 0x1f, 0x40,
    0x08, 0xff, 0x7f, 0x7f, 0x7f, 0x7f, 0x1f, 0x40, 0x1f, 0x40,
    0x09, 0xff, 0x7f, 0x7f, 0x7f, 0x7f, 0x1f, 0x40, 0x1f, 0x40,
    0x0a, 0xff, 0x7f, 0x7f, 0x7f, 0x7f, 0x1f, 0x40, 0x1f, 0x40,
    0x0b, 0xff, 0x7f, 0x7f, 0x7f, 0x7f, 0x1f, 0x40, 0x1f, 0x40};

// Firmware profile codes reported by GetFunction(7) bits 8..15 and accepted by
// SetFunction function 7 byte 2 (AcerSense's Silent mode is code 2).
constexpr unsigned kProfileNormal = 0;
constexpr unsigned kProfileSilent = 2;

inline std::uint16_t RowRpm(const Table& t, std::size_t row, bool gpu) {
    const std::size_t at = row * kRowSize + (gpu ? 8 : 6);
    return static_cast<std::uint16_t>((t[at] << 8) | t[at + 1]);
}

inline void SetRowRpm(Table& t, std::size_t row, bool gpu, std::uint16_t rpm) {
    const std::size_t at = row * kRowSize + (gpu ? 8 : 6);
    t[at] = static_cast<std::uint8_t>(rpm >> 8);
    t[at + 1] = static_cast<std::uint8_t>(rpm & 0xff);
}

// ---------------------------------------------------------------------------
// Machine identity
// ---------------------------------------------------------------------------
inline std::wstring BiosString(const wchar_t* name) {
    const wchar_t* key = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
    DWORD cb = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ, nullptr, nullptr, &cb) !=
            ERROR_SUCCESS || cb < sizeof(wchar_t))
        return L"";
    std::vector<wchar_t> v(cb / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ, nullptr, v.data(), &cb) !=
        ERROR_SUCCESS)
        return L"";
    return v.data();
}

inline bool IsSupportedMachine() {
    auto has = [](const std::wstring& s, const wchar_t* n) { return s.find(n) != std::wstring::npos; };
    return has(BiosString(L"SystemManufacturer"), L"Acer") &&
           has(BiosString(L"SystemProductName"), L"SFX16-61G") &&
           has(BiosString(L"BaseBoardProduct"), L"Ariya_PEH") &&
           has(BiosString(L"BIOSVersion"), L"1.56");
}

inline bool IsElevated() {
    BOOL admin = FALSE;
    PSID group = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &group)) {
        CheckTokenMembership(nullptr, group, &admin);
        FreeSid(group);
    }
    return admin != FALSE;
}

inline std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path, n);
    const auto slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

// ---------------------------------------------------------------------------
// EC access through PawnIO
// ---------------------------------------------------------------------------
class EcLock {
public:
    // Global\Access_EC is the convention shared by LibreHardwareMonitor/FanControl.
    // It does not coordinate with ACPI.sys; it only keeps cooperating tools out.
    explicit EcLock(DWORD timeoutMs = 2000) {
        mutex_ = CreateMutexW(nullptr, FALSE, L"Global\\Access_EC");
        if (!mutex_) throw std::runtime_error("cannot open Global\\Access_EC mutex");
        const DWORD r = WaitForSingleObject(mutex_, timeoutMs);
        if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED) {
            CloseHandle(mutex_);
            throw std::runtime_error("Global\\Access_EC busy (another EC tool is running)");
        }
    }
    ~EcLock() {
        ReleaseMutex(mutex_);
        CloseHandle(mutex_);
    }
    EcLock(const EcLock&) = delete;
    EcLock& operator=(const EcLock&) = delete;

private:
    HANDLE mutex_ = nullptr;
};

class PawnEc {
public:
    PawnEc() = default;
    PawnEc(const PawnEc&) = delete;
    PawnEc& operator=(const PawnEc&) = delete;
    ~PawnEc() {
        if (handle_ && close_) close_(handle_);
        if (dll_) FreeLibrary(dll_);
    }

    // Safe to call again after a failure (releases any previous handle first).
    void Connect() {
        if (handle_ && close_) close_(handle_);
        handle_ = nullptr;
        if (dll_) FreeLibrary(dll_);
        dll_ = nullptr;
        dll_ = LoadLibraryW(L"C:\\Program Files\\PawnIO\\PawnIOLib.dll");
        if (!dll_) throw std::runtime_error("PawnIOLib.dll not found (install PawnIO)");
        open_ = reinterpret_cast<Open>(GetProcAddress(dll_, "pawnio_open"));
        load_ = reinterpret_cast<Load>(GetProcAddress(dll_, "pawnio_load"));
        exec_ = reinterpret_cast<Exec>(GetProcAddress(dll_, "pawnio_execute"));
        close_ = reinterpret_cast<Close>(GetProcAddress(dll_, "pawnio_close"));
        if (!open_ || !load_ || !exec_ || !close_)
            throw std::runtime_error("PawnIOLib exports missing");
        if (FAILED(open_(&handle_)) || !handle_)
            throw std::runtime_error("pawnio_open failed (driver missing, or not elevated)");
        const std::wstring module = ExeDirectory() + L"\\LpcACPIEC.bin";
        std::ifstream f(module, std::ios::binary);
        if (!f) throw std::runtime_error("LpcACPIEC.bin missing beside the executable");
        std::vector<unsigned char> blob((std::istreambuf_iterator<char>(f)), {});
        if (blob.size() < 128 || blob.size() > 1024 * 1024)
            throw std::runtime_error("LpcACPIEC.bin has unexpected size");
        if (FAILED(load_(handle_, blob.data(), blob.size())))
            throw std::runtime_error("PawnIO rejected LpcACPIEC.bin");
    }

    std::uint8_t ReadXram(std::uint16_t address) {
        for (int attempt = 0;; ++attempt) {
            try {
                return ReadOnce(address);
            } catch (const std::exception&) {
                if (attempt >= 2) throw;
                Sleep(20);
            }
        }
    }

    // Writes one byte and confirms it by readback. Retries a collided or
    // unconfirmed write up to three times in total.
    void WriteXramVerified(std::uint16_t address, std::uint8_t value) {
        for (int attempt = 0;; ++attempt) {
            try {
                WriteOnce(address, value);
                if (ReadXram(address) == value) return;
            } catch (const std::exception&) {
                if (attempt >= 2) throw;
            }
            if (attempt >= 2)
                throw std::runtime_error("EC write not confirmed by readback at 0x" + Hex(address));
            Sleep(20);
        }
    }

    // Model-specific XRAM commands and table address (defaults: SFX16-61G).
    void Configure(std::uint16_t tableBase, std::uint8_t readCmd, std::uint8_t writeCmd) {
        base_ = tableBase;
        readCmd_ = readCmd;
        writeCmd_ = writeCmd;
    }
    std::uint16_t TableBase() const { return base_; }

    // Standard ACPI EC space (commands 0x80 read / 0x81 write), as used by NBFC configs.
    std::uint8_t ReadEc(std::uint8_t reg) {
        for (int attempt = 0;; ++attempt) {
            try {
                WaitIdle();
                const ULONGLONG deadline = GetTickCount64() + 100;
                Out(0x66, 0x80);
                WaitStatus(kIbf, false, deadline, "after 0x80");
                Out(0x62, reg);
                WaitStatus(kObf, true, deadline, "waiting for data");
                return In(0x62);
            } catch (const std::exception&) {
                if (attempt >= 2) throw;
                Sleep(20);
            }
        }
    }
    void WriteEc(std::uint8_t reg, std::uint8_t value) {
        for (int attempt = 0;; ++attempt) {
            try {
                WaitIdle();
                const ULONGLONG deadline = GetTickCount64() + 100;
                Out(0x66, 0x81);
                WaitStatus(kIbf, false, deadline, "after 0x81");
                Out(0x62, reg);
                WaitStatus(kIbf, false, deadline, "after register");
                Out(0x62, value);
                WaitStatus(kIbf, false, deadline, "after data");
                return;
            } catch (const std::exception&) {
                if (attempt >= 2) throw;
                Sleep(20);
            }
        }
    }

    Table ReadTable() {
        Table t{};
        for (std::size_t i = 0; i < kTableSize; ++i)
            t[i] = ReadXram(static_cast<std::uint16_t>(base_ + i));
        return t;
    }

    // Writes only the bytes that differ. RPM fields are big-endian pairs; the
    // byte order is chosen so the transient value is never below both the old
    // and the new value (more cooling, never less, while half-written).
    void WriteTableDiff(const Table& current, const Table& target) {
        for (std::size_t row = 0; row < kTableSize / kRowSize; ++row) {
            const std::size_t base = row * kRowSize;
            for (std::size_t b = 0; b < 6; ++b) WriteIfDifferent(current, target, base + b);
            for (std::size_t field : {std::size_t{6}, std::size_t{8}}) {
                const std::size_t hi = base + field, lo = hi + 1;
                const unsigned oldV = (current[hi] << 8) | current[lo];
                const unsigned newV = (target[hi] << 8) | target[lo];
                if (newV >= oldV) {
                    WriteIfDifferent(current, target, hi);
                    WriteIfDifferent(current, target, lo);
                } else {
                    WriteIfDifferent(current, target, lo);
                    WriteIfDifferent(current, target, hi);
                }
            }
        }
    }

    static std::string Hex(unsigned v, int width = 4) {
        static const char* digits = "0123456789ABCDEF";
        std::string s(static_cast<std::size_t>(width), '0');
        for (int i = width - 1; i >= 0; --i, v >>= 4) s[static_cast<std::size_t>(i)] = digits[v & 0xf];
        return s;
    }

private:
    using Open = HRESULT(WINAPI*)(HANDLE*);
    using Load = HRESULT(WINAPI*)(HANDLE, const unsigned char*, SIZE_T);
    using Exec = HRESULT(WINAPI*)(HANDLE, const char*, const unsigned long long*, SIZE_T,
                                  unsigned long long*, SIZE_T, SIZE_T*);
    using Close = HRESULT(WINAPI*)(HANDLE);

    static constexpr std::uint8_t kObf = 0x01, kIbf = 0x02, kSciEvt = 0x20;

    void WriteIfDifferent(const Table& current, const Table& target, std::size_t i) {
        if (current[i] != target[i])
            WriteXramVerified(static_cast<std::uint16_t>(base_ + i), target[i]);
    }

    std::uint8_t In(std::uint16_t port) {
        const unsigned long long in[] = {port};
        unsigned long long out[] = {0};
        SIZE_T returned = 0;
        if (FAILED(exec_(handle_, "ioctl_pio_read", in, 1, out, 1, &returned)) || returned != 1)
            throw std::runtime_error("EC port read failed");
        return static_cast<std::uint8_t>(out[0]);
    }

    void Out(std::uint16_t port, std::uint8_t value) {
        const unsigned long long in[] = {port, value};
        SIZE_T returned = 0;
        if (FAILED(exec_(handle_, "ioctl_pio_write", in, 2, nullptr, 0, &returned)))
            throw std::runtime_error("EC port write failed");
    }

    // Waits until neither the host (IBF/OBF) nor a pending EC event (SCI_EVT)
    // owns the interface, so ACPI.sys gets to finish its query first.
    void WaitIdle() {
        const ULONGLONG deadline = GetTickCount64() + 250;
        while (In(0x66) & (kObf | kIbf | kSciEvt)) {
            if (GetTickCount64() >= deadline) throw std::runtime_error("EC busy; no command sent");
            Sleep(1);
        }
    }

    void WaitStatus(std::uint8_t mask, bool wantSet, ULONGLONG deadline, const char* stage) {
        while (((In(0x66) & mask) != 0) != wantSet) {
            if (GetTickCount64() >= deadline) throw std::runtime_error(std::string("EC timeout ") + stage);
            Sleep(0);
        }
    }

    std::uint8_t ReadOnce(std::uint16_t address) {
        WaitIdle();
        const ULONGLONG deadline = GetTickCount64() + 100;
        Out(0x66, readCmd_);
        WaitStatus(kIbf, false, deadline, "after 0x92");
        Out(0x62, static_cast<std::uint8_t>(address >> 8));
        WaitStatus(kIbf, false, deadline, "after address high");
        Out(0x62, static_cast<std::uint8_t>(address & 0xff));
        WaitStatus(kObf, true, deadline, "waiting for data");
        return In(0x62);
    }

    void WriteOnce(std::uint16_t address, std::uint8_t value) {
        WaitIdle();
        const ULONGLONG deadline = GetTickCount64() + 100;
        Out(0x66, writeCmd_);
        WaitStatus(kIbf, false, deadline, "after 0x93");
        Out(0x62, static_cast<std::uint8_t>(address >> 8));
        WaitStatus(kIbf, false, deadline, "after address high");
        Out(0x62, static_cast<std::uint8_t>(address & 0xff));
        WaitStatus(kIbf, false, deadline, "after address low");
        Out(0x62, value);
        WaitStatus(kIbf, false, deadline, "after data");
    }

    std::uint16_t base_ = kTableBase;
    std::uint8_t readCmd_ = 0x92, writeCmd_ = 0x93;
    HMODULE dll_ = nullptr;
    HANDLE handle_ = nullptr;
    Open open_ = nullptr;
    Load load_ = nullptr;
    Exec exec_ = nullptr;
    Close close_ = nullptr;
};

// ---------------------------------------------------------------------------
// Acer WMI
// ---------------------------------------------------------------------------
struct Telemetry {
    bool ok = false;
    unsigned cpuTemp = 0, gpuTemp = 0, cpuRpm = 0, gpuRpm = 0;
};

class AcerWmi {
public:
    // Requires COM initialised on the calling thread.
    bool Connect() {
        ComPtr<IWbemLocator> locator;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(locator.GetAddressOf()))))
            return false;
        BSTR ns = SysAllocString(L"ROOT\\WMI");
        HRESULT hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr,
                                            services_.GetAddressOf());
        SysFreeString(ns);
        if (FAILED(hr)) return false;
        return SUCCEEDED(CoSetProxyBlanket(services_.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                                           nullptr, RPC_C_AUTHN_LEVEL_CALL,
                                           RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE));
    }

    bool GetProfile(unsigned& code) {
        std::uint64_t raw = 0;
        if (!CallU32(L"APGeAction", L"GetFunction", L"uiInput", 7, L"uiOutput", raw)) return false;
        code = static_cast<unsigned>((raw >> 8) & 0xff);
        return true;
    }

    // APGeAction.SetFunction function 7, operation 0: the same call AcerSense
    // makes. Firmware sets FNLV and sends EC command 0x83, which reloads the
    // built-in table for that profile.
    bool SetFactoryProfile(unsigned code, std::uint64_t& rawOut) {
        const unsigned long long input = (static_cast<unsigned long long>(code) << 16) | 0x07;
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_BSTR;  // CIM_UINT64 inputs are passed as decimal strings.
        v.bstrVal = SysAllocString(std::to_wstring(input).c_str());
        const bool ok = Call(L"APGeAction", L"SetFunction", L"uiInput", v, L"uiOutput", rawOut);
        VariantClear(&v);
        return ok;
    }

    // Names of vendor WMI classes in ROOT\WMI (system and Microsoft classes skipped).
    std::vector<std::wstring> ListVendorClasses() {
        std::vector<std::wstring> names;
        if (!services_) return names;
        ComPtr<IEnumWbemClassObject> it;
        if (FAILED(services_->CreateClassEnum(nullptr, WBEM_FLAG_DEEP | WBEM_FLAG_FORWARD_ONLY |
                                                           WBEM_FLAG_RETURN_IMMEDIATELY,
                                              nullptr, it.GetAddressOf())))
            return names;
        for (;;) {
            ComPtr<IWbemClassObject> cls;
            ULONG n = 0;
            if (FAILED(it->Next(5000, 1, cls.GetAddressOf(), &n)) || n == 0) break;
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(cls->Get(L"__CLASS", 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR) {
                const std::wstring name = v.bstrVal;
                static const wchar_t* skip[] = {L"__", L"MS", L"Ms", L"Wmi", L"WMI", L"CIM", L"Win32", L"Kernel",
                                                L"Rt", L"Sys", L"Event", L"Hid", L"Network", L"Disk", L"Usb"};
                bool system = false;
                for (const wchar_t* p : skip) system = system || name.rfind(p, 0) == 0;
                if (!system) names.push_back(name);
            }
            VariantClear(&v);
        }
        return names;
    }

    // Highest ACPI thermal zone temperature in C (generic, works without vendor WMI).
    bool ThermalZoneC(unsigned& celsius) {
        if (!services_) return false;
        BSTR cls = SysAllocString(L"MSAcpi_ThermalZoneTemperature");
        ComPtr<IEnumWbemClassObject> it;
        const HRESULT hr = services_->CreateInstanceEnum(cls, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                                         nullptr, it.GetAddressOf());
        SysFreeString(cls);
        if (FAILED(hr)) return false;
        bool found = false;
        for (;;) {
            ComPtr<IWbemClassObject> obj;
            ULONG n = 0;
            if (FAILED(it->Next(3000, 1, obj.GetAddressOf(), &n)) || n == 0) break;
            VARIANT v;
            VariantInit(&v);
            std::uint64_t tenthsKelvin = 0;
            if (SUCCEEDED(obj->Get(L"CurrentTemperature", 0, &v, nullptr, nullptr)) && ToU64(v, tenthsKelvin) &&
                tenthsKelvin > 2732 && tenthsKelvin < 2732 + 1500) {
                const unsigned c = static_cast<unsigned>((tenthsKelvin - 2732 + 5) / 10);
                if (!found || c > celsius) celsius = c;
                found = true;
            }
            VariantClear(&v);
        }
        return found;
    }

    Telemetry Read() {
        Telemetry t;
        t.ok = SysInfo(0x0101, t.cpuTemp) && SysInfo(0x0201, t.cpuRpm) &&
               SysInfo(0x0601, t.gpuRpm) && SysInfo(0x0A01, t.gpuTemp);
        return t;
    }

private:
    bool SysInfo(std::uint32_t query, unsigned& value) {
        std::uint64_t raw = 0;
        if (!CallU32(L"AcerGamingFunction", L"GetGamingSysInfo", L"gmInput", query, L"gmOutput", raw) ||
            (raw & 0xff) != 0)
            return false;
        value = static_cast<unsigned>((raw >> 8) & 0xffff);
        return true;
    }

    bool CallU32(const wchar_t* cls, const wchar_t* method, const wchar_t* arg, std::uint32_t value,
                 const wchar_t* outName, std::uint64_t& out) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;  // Verified on this hardware: CIM_UINT32 input must be VT_I4.
        v.lVal = static_cast<LONG>(value);
        return Call(cls, method, arg, v, outName, out);
    }

    bool Call(const wchar_t* cls, const wchar_t* method, const wchar_t* arg, VARIANT& value,
              const wchar_t* outName, std::uint64_t& out) {
        if (!services_) return false;
        BSTR clsB = SysAllocString(cls), methodB = SysAllocString(method);
        bool ok = false;
        do {
            ComPtr<IEnumWbemClassObject> it;
            if (FAILED(services_->CreateInstanceEnum(
                    clsB, WBEM_FLAG_RETURN_IMMEDIATELY | WBEM_FLAG_FORWARD_ONLY, nullptr,
                    it.GetAddressOf())))
                break;
            ComPtr<IWbemClassObject> target;
            ULONG count = 0;
            if (FAILED(it->Next(5000, 1, target.GetAddressOf(), &count)) || count != 1) break;
            VARIANT path;
            VariantInit(&path);
            if (FAILED(target->Get(L"__PATH", 0, &path, nullptr, nullptr)) || path.vt != VT_BSTR) {
                VariantClear(&path);
                break;
            }
            ComPtr<IWbemClassObject> klass, sig, params, result;
            HRESULT hr = services_->GetObject(clsB, 0, nullptr, klass.GetAddressOf(), nullptr);
            if (SUCCEEDED(hr)) hr = klass->GetMethod(methodB, 0, sig.GetAddressOf(), nullptr);
            if (SUCCEEDED(hr) && sig) hr = sig->SpawnInstance(0, params.GetAddressOf());
            if (SUCCEEDED(hr) && params) hr = params->Put(arg, 0, &value, 0);
            if (SUCCEEDED(hr))
                hr = services_->ExecMethod(path.bstrVal, methodB, 0, nullptr, params.Get(),
                                           result.GetAddressOf(), nullptr);
            VariantClear(&path);
            if (FAILED(hr) || !result) break;
            VARIANT r;
            VariantInit(&r);
            if (SUCCEEDED(result->Get(outName, 0, &r, nullptr, nullptr))) ok = ToU64(r, out);
            VariantClear(&r);
        } while (false);
        SysFreeString(clsB);
        SysFreeString(methodB);
        return ok;
    }

    static bool ToU64(const VARIANT& v, std::uint64_t& out) {
        switch (v.vt) {
        case VT_I4: out = static_cast<std::uint32_t>(v.lVal); return true;
        case VT_UI4: out = v.ulVal; return true;
        case VT_I8: out = static_cast<std::uint64_t>(v.llVal); return true;
        case VT_UI8: out = v.ullVal; return true;
        case VT_BSTR: {
            if (!v.bstrVal) return false;
            wchar_t* end = nullptr;
            out = std::wcstoull(v.bstrVal, &end, 10);
            return end != v.bstrVal && *end == 0;
        }
        default: return false;
        }
    }

    ComPtr<IWbemServices> services_;
};

}  // namespace acer
