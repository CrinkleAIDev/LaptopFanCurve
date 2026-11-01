// NoteBook FanControl (NBFC) configuration support.
// Users import their own model's config (Windows NBFC .xml "FanControlConfigV2" or
// nbfc-linux .json); this file parses it and runs NBFC's fan-control rules:
//   - each fan has temperature thresholds (Up/Down/FanSpeed %) with hysteresis;
//   - percent -> register value is linear between Min/MaxSpeedValue, rounded,
//     unless a FanSpeedPercentageOverride matches (within 0.06 %);
//   - above CriticalTemperature the fan runs at 100 % until it drops below
//     CriticalTemperature - CriticalTemperatureOffset (default 15);
//   - ReadWriteWords stores 16-bit values little-endian at register and register+1;
//   - RegisterWriteConfigurations run OnInitialization or OnWriteFanSpeed (Set/And/Or);
//   - reset values are written when control stops, returning fans to the firmware.
// Configs that need ACPI methods or Lua code are not supported.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace nbfc {

// ===========================================================================
// Tiny document tree shared by the XML and JSON readers
// ===========================================================================
struct Node {
    std::string name, text;
    std::vector<std::unique_ptr<Node>> kids;
    const Node* Child(const char* n) const {
        for (const auto& k : kids)
            if (k->name == n) return k.get();
        return nullptr;
    }
    std::string Str(const char* n, const std::string& def = "") const {
        const Node* c = Child(n);
        return c ? c->text : def;
    }
    double Num(const char* n, double def) const {
        const Node* c = Child(n);
        if (!c || c->text.empty()) return def;
        try { return std::stod(c->text); } catch (...) { return def; }
    }
    bool Bool(const char* n, bool def) const {
        const Node* c = Child(n);
        if (!c) return def;
        return c->text == "true" || c->text == "True" || c->text == "1";
    }
};

inline std::string DecodeEntities(std::string s) {
    const std::pair<const char*, char> map[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'}};
    for (const auto& [from, to] : map)
        for (std::size_t at = 0; (at = s.find(from, at)) != std::string::npos; ++at) s.replace(at, strlen(from), 1, to);
    return s;
}

inline std::string Trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

// XML: elements, text, comments, declarations; attributes are ignored.
class XmlReader {
public:
    explicit XmlReader(const std::string& s) : s_(s) {}
    std::unique_ptr<Node> Parse() {
        auto root = std::make_unique<Node>();
        ParseChildren(*root, "");
        return root->kids.empty() ? nullptr : std::move(root->kids.front());
    }

private:
    void SkipMisc() {
        for (;;) {
            if (s_.compare(i_, 4, "<!--") == 0) {
                const auto e = s_.find("-->", i_);
                i_ = e == std::string::npos ? s_.size() : e + 3;
            } else if (s_.compare(i_, 2, "<?") == 0 || s_.compare(i_, 2, "<!") == 0) {
                const auto e = s_.find('>', i_);
                i_ = e == std::string::npos ? s_.size() : e + 1;
            } else {
                return;
            }
        }
    }
    void ParseChildren(Node& parent, const std::string& closing) {
        std::string text;
        while (i_ < s_.size()) {
            if (s_[i_] != '<') {
                text += s_[i_++];
                continue;
            }
            SkipMisc();
            if (i_ >= s_.size()) break;
            if (s_[i_] != '<') continue;
            if (s_.compare(i_, 2, "</") == 0) {
                const auto e = s_.find('>', i_);
                const std::string name = Trim(s_.substr(i_ + 2, e - i_ - 2));
                i_ = e == std::string::npos ? s_.size() : e + 1;
                if (name == closing) break;
                continue;
            }
            const auto e = s_.find('>', i_);
            if (e == std::string::npos) { i_ = s_.size(); break; }
            std::string tag = s_.substr(i_ + 1, e - i_ - 1);
            i_ = e + 1;
            const bool selfClosing = !tag.empty() && tag.back() == '/';
            if (selfClosing) tag.pop_back();
            auto node = std::make_unique<Node>();
            node->name = tag.substr(0, tag.find_first_of(" \t\r\n"));
            if (!selfClosing) ParseChildren(*node, node->name);
            parent.kids.push_back(std::move(node));
        }
        if (parent.kids.empty()) parent.text = DecodeEntities(Trim(text));
    }
    const std::string& s_;
    std::size_t i_ = 0;
};

// JSON: objects become named children, arrays become unnamed children.
class JsonReader {
public:
    explicit JsonReader(const std::string& s) : s_(s) {}
    std::unique_ptr<Node> Parse() {
        auto n = std::make_unique<Node>();
        if (!Value(*n)) return nullptr;
        return n;
    }

private:
    void Ws() { while (i_ < s_.size() && isspace(static_cast<unsigned char>(s_[i_]))) ++i_; }
    bool String(std::string& out) {
        if (s_[i_] != '"') return false;
        for (++i_; i_ < s_.size() && s_[i_] != '"'; ++i_) {
            if (s_[i_] == '\\' && i_ + 1 < s_.size()) {
                const char c = s_[++i_];
                out += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'u' ? '?' : c;
                if (c == 'u') i_ += 4;
            } else {
                out += s_[i_];
            }
        }
        ++i_;
        return true;
    }
    bool Value(Node& n) {
        Ws();
        if (i_ >= s_.size()) return false;
        const char c = s_[i_];
        if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            ++i_;
            for (;;) {
                Ws();
                if (i_ < s_.size() && s_[i_] == close) { ++i_; return true; }
                auto kid = std::make_unique<Node>();
                if (c == '{') {
                    if (!String(kid->name)) return false;
                    Ws();
                    if (i_ >= s_.size() || s_[i_++] != ':') return false;
                }
                if (!Value(*kid)) return false;
                n.kids.push_back(std::move(kid));
                Ws();
                if (i_ < s_.size() && s_[i_] == ',') ++i_;
            }
        }
        if (c == '"') return String(n.text);
        const auto b = i_;
        while (i_ < s_.size() && (isalnum(static_cast<unsigned char>(s_[i_])) || s_[i_] == '-' || s_[i_] == '+' || s_[i_] == '.')) ++i_;
        n.text = s_.substr(b, i_ - b);
        return i_ > b;
    }
    const std::string& s_;
    std::size_t i_ = 0;
};

// ===========================================================================
// Config model
// ===========================================================================
struct Threshold {
    int up = 0, down = 0;
    double speed = 0;  // percent
};
struct Override {
    double percent = 0;
    int value = 0;
    bool read = true, write = true;
};
struct Fan {
    std::wstring name;
    int readRegister = 0, writeRegister = 0;
    int minWrite = 0, maxWrite = 255, minRead = 0, maxRead = 255;
    bool readWords = false, writeWords = false;
    bool resetRequired = false;
    int resetValue = 0;
    int critical = 0, criticalOffset = 15;
    std::vector<Threshold> thresholds;  // sorted by up, ascending
    std::vector<Override> overrides;
};
enum class WriteMode { Set, And, Or };
struct RegisterWrite {
    WriteMode mode = WriteMode::Set;
    bool onInit = true;  // false = OnWriteFanSpeed
    int reg = 0, value = 0;
    bool resetRequired = false;
    int resetValue = 0;
    WriteMode resetMode = WriteMode::Set;
    std::wstring description;
};
struct Config {
    std::wstring model, author, file;
    int pollMs = 1000;
    std::vector<Fan> fans;
    std::vector<RegisterWrite> registers;
};

inline std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

inline WriteMode ParseMode(const std::string& s) {
    return s == "And" ? WriteMode::And : s == "Or" ? WriteMode::Or : WriteMode::Set;
}

inline bool Load(const std::wstring& path, Config& out, std::wstring& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { error = L"The file could not be opened."; return false; }
    const std::string text((std::istreambuf_iterator<char>(f)), {});
    const auto first = text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    if (first == std::string::npos) { error = L"The file is empty."; return false; }
    std::unique_ptr<Node> root = text[first] == '{' ? JsonReader(text).Parse() : XmlReader(text).Parse();
    if (!root) { error = L"The file is not a valid NBFC XML or JSON config."; return false; }
    if (text.find("AcpiMethod") != std::string::npos || text.find("LuaCode") != std::string::npos) {
        error = L"This config uses ACPI methods or Lua code, which this app does not support.";
        return false;
    }
    Config c;
    c.file = path;
    c.model = Wide(root->Str("NotebookModel"));
    c.author = Wide(root->Str("Author"));
    c.pollMs = std::max(100, static_cast<int>(root->Num("EcPollInterval", 1000)));
    const bool words = root->Bool("ReadWriteWords", false);
    const int critical = static_cast<int>(root->Num("CriticalTemperature", 75));
    const int criticalOffset = static_cast<int>(root->Num("CriticalTemperatureOffset", 15));
    const Node* fans = root->Child("FanConfigurations");
    if (!fans || fans->kids.empty()) { error = L"The config has no FanConfigurations."; return false; }
    for (const auto& fn : fans->kids) {
        Fan fan;
        fan.name = Wide(fn->Str("FanDisplayName"));
        if (fan.name.empty()) fan.name = L"Fan " + std::to_wstring(c.fans.size() + 1);
        fan.readRegister = static_cast<int>(fn->Num("ReadRegister", -1));
        fan.writeRegister = static_cast<int>(fn->Num("WriteRegister", -1));
        fan.minWrite = static_cast<int>(fn->Num("MinSpeedValue", 0));
        fan.maxWrite = static_cast<int>(fn->Num("MaxSpeedValue", 255));
        const bool independent = fn->Bool("IndependentReadMinMaxValues", false);
        fan.minRead = independent ? static_cast<int>(fn->Num("MinSpeedValueRead", 0)) : fan.minWrite;
        fan.maxRead = independent ? static_cast<int>(fn->Num("MaxSpeedValueRead", 255)) : fan.maxWrite;
        fan.readWords = fn->Bool("ReadWords", words);
        fan.writeWords = fn->Bool("WriteWords", words);
        fan.resetRequired = fn->Bool("ResetRequired", false);
        fan.resetValue = static_cast<int>(fn->Num("FanSpeedResetValue", 0));
        fan.critical = static_cast<int>(fn->Num("CriticalTemperature", critical));
        fan.criticalOffset = static_cast<int>(fn->Num("CriticalTemperatureOffset", criticalOffset));
        if (const Node* th = fn->Child("TemperatureThresholds"))
            for (const auto& t : th->kids)
                fan.thresholds.push_back({static_cast<int>(t->Num("UpThreshold", 0)), static_cast<int>(t->Num("DownThreshold", 0)),
                                          std::clamp(t->Num("FanSpeed", 0), 0.0, 100.0)});
        if (const Node* ov = fn->Child("FanSpeedPercentageOverrides"))
            for (const auto& o : ov->kids) {
                const std::string target = o->Str("TargetOperation", "ReadWrite");
                fan.overrides.push_back({o->Num("FanSpeedPercentage", 0), static_cast<int>(o->Num("FanSpeedValue", 0)),
                                         target != "Write", target != "Read"});
            }
        std::sort(fan.thresholds.begin(), fan.thresholds.end(), [](const Threshold& a, const Threshold& b) { return a.up < b.up; });
        const int limit = fan.writeWords ? 254 : 255;
        if (fan.readRegister < 0 || fan.readRegister > limit || fan.writeRegister < 0 || fan.writeRegister > limit) {
            error = L"A fan register is outside the EC's 0-255 range.";
            return false;
        }
        if (fan.thresholds.size() < 2) { error = L"Each fan needs at least two temperature thresholds."; return false; }
        c.fans.push_back(fan);
    }
    if (const Node* regs = root->Child("RegisterWriteConfigurations"))
        for (const auto& r : regs->kids) {
            RegisterWrite w;
            w.mode = ParseMode(r->Str("WriteMode", "Set"));
            w.onInit = r->Str("WriteOccasion", "OnInitialization") != "OnWriteFanSpeed";
            w.reg = static_cast<int>(r->Num("Register", -1));
            w.value = static_cast<int>(r->Num("Value", 0));
            w.resetRequired = r->Bool("ResetRequired", false);
            w.resetValue = static_cast<int>(r->Num("ResetValue", 0));
            w.resetMode = ParseMode(r->Str("ResetWriteMode", "Set"));
            w.description = Wide(r->Str("Description"));
            if (w.reg < 0 || w.reg > 255 || w.value < 0 || w.value > 255) {
                error = L"A register write is outside the EC's 0-255 range.";
                return false;
            }
            c.registers.push_back(w);
        }
    out = c;
    return true;
}

// ===========================================================================
// Engine
// ===========================================================================
struct EcPort {  // byte access to the standard ACPI EC space
    virtual ~EcPort() = default;
    virtual std::uint8_t Read(std::uint8_t reg) = 0;
    virtual void Write(std::uint8_t reg, std::uint8_t value) = 0;
};

inline int PercentToValue(const Fan& f, double percent) {
    percent = std::clamp(percent, 0.0, 100.0);
    for (const Override& o : f.overrides)
        if (o.write && std::fabs(o.percent - percent) < 0.06) return o.value;
    return static_cast<int>(std::lround(f.minWrite + (f.maxWrite - f.minWrite) * percent / 100.0));
}

inline double ValueToPercent(const Fan& f, int value) {
    for (const Override& o : f.overrides)
        if (o.read && o.value == value) return o.percent;
    if (f.maxRead == f.minRead) return 0;
    return std::clamp((value - f.minRead) * 100.0 / (f.maxRead - f.minRead), 0.0, 100.0);
}

class Engine {
public:
    void Start(const Config& cfg, const std::vector<std::vector<Threshold>>& curves) {
        cfg_ = cfg;
        for (std::size_t i = 0; i < cfg_.fans.size() && i < curves.size(); ++i) cfg_.fans[i].thresholds = curves[i];
        index_.assign(cfg_.fans.size(), 0);
        critical_.assign(cfg_.fans.size(), false);
        target_.assign(cfg_.fans.size(), 0);
        initialized_ = false;
        running_ = true;
    }
    bool Running() const { return running_; }
    const std::vector<double>& Targets() const { return target_; }

    // One control step: select thresholds, write fan values and OnWriteFanSpeed registers.
    void Step(EcPort& ec, int temperature) {
        if (!running_) return;
        if (!initialized_) {
            for (const RegisterWrite& r : cfg_.registers)
                if (r.onInit) WriteRegister(ec, r.reg, r.value, r.mode);
            initialized_ = true;
        }
        for (std::size_t i = 0; i < cfg_.fans.size(); ++i) {
            const Fan& f = cfg_.fans[i];
            if (temperature > f.critical) critical_[i] = true;
            else if (temperature < f.critical - f.criticalOffset) critical_[i] = false;
            std::size_t& k = index_[i];
            while (k + 1 < f.thresholds.size() && temperature >= f.thresholds[k + 1].up) ++k;
            while (k > 0 && temperature < f.thresholds[k].down) --k;
            target_[i] = critical_[i] ? 100.0 : f.thresholds[k].speed;
            WriteValue(ec, f.writeRegister, PercentToValue(f, target_[i]), f.writeWords);
        }
        for (const RegisterWrite& r : cfg_.registers)
            if (!r.onInit) WriteRegister(ec, r.reg, r.value, r.mode);
    }

    // Returns control to the firmware.
    void Stop(EcPort& ec) {
        if (!running_) return;
        running_ = false;
        for (const Fan& f : cfg_.fans)
            if (f.resetRequired) WriteValue(ec, f.writeRegister, f.resetValue, f.writeWords);
        for (const RegisterWrite& r : cfg_.registers)
            if (r.resetRequired) WriteRegister(ec, r.reg, r.resetValue, r.resetMode);
    }

    static double ReadPercent(EcPort& ec, const Fan& f) {
        int v = ec.Read(static_cast<std::uint8_t>(f.readRegister));
        if (f.readWords) v |= ec.Read(static_cast<std::uint8_t>(f.readRegister + 1)) << 8;
        return ValueToPercent(f, v);
    }

private:
    static void WriteValue(EcPort& ec, int reg, int value, bool word) {
        ec.Write(static_cast<std::uint8_t>(reg), static_cast<std::uint8_t>(value & 0xff));
        if (word) ec.Write(static_cast<std::uint8_t>(reg + 1), static_cast<std::uint8_t>((value >> 8) & 0xff));
    }
    static void WriteRegister(EcPort& ec, int reg, int value, WriteMode mode) {
        std::uint8_t v = static_cast<std::uint8_t>(value);
        if (mode != WriteMode::Set) {
            const std::uint8_t cur = ec.Read(static_cast<std::uint8_t>(reg));
            v = mode == WriteMode::And ? static_cast<std::uint8_t>(cur & v) : static_cast<std::uint8_t>(cur | v);
        }
        ec.Write(static_cast<std::uint8_t>(reg), v);
    }

    Config cfg_;
    std::vector<std::size_t> index_;
    std::vector<bool> critical_;
    std::vector<double> target_;
    bool initialized_ = false, running_ = false;
};

}  // namespace nbfc
