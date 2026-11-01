// Hardware-independent tests: NBFC config reader and engine (against a simulated EC), safety decisions
// under simulated failures (watchdog, lost readings, EC errors),
// the BIOS fan-table scanner, the Acer EC-family check and model-definition handling.
// All fixtures below are written for these tests (fictional models); no third-party configs.
// Build and run: tests\run_tests.cmd
#include "../src/Identify.h"
#include "../src/Safety.h"

#include <cstdio>
#include <map>

using namespace acer;

namespace {

int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

std::wstring TempFile(const wchar_t* name, const std::string& content) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    const std::wstring path = std::wstring(dir) + L"LaptopFanCurveTests_" + name;
    std::ofstream(path, std::ios::binary) << content;
    return path;
}

struct MockEc : nbfc::EcPort {
    std::map<int, int> mem;
    std::uint8_t Read(std::uint8_t r) override { return static_cast<std::uint8_t>(mem[r]); }
    void Write(std::uint8_t r, std::uint8_t v) override { mem[r] = v; }
};

const char* kXml = R"(<?xml version="1.0"?>
<FanControlConfigV2>
  <NotebookModel>Example Laptop 14 EX1400</NotebookModel>
  <Author>tests</Author>
  <EcPollInterval>500</EcPollInterval>
  <ReadWriteWords>false</ReadWriteWords>
  <CriticalTemperature>90</CriticalTemperature>
  <FanConfigurations>
    <FanConfiguration>
      <ReadRegister>40</ReadRegister>
      <WriteRegister>41</WriteRegister>
      <MinSpeedValue>10</MinSpeedValue>
      <MaxSpeedValue>110</MaxSpeedValue>
      <FanDisplayName>Main fan</FanDisplayName>
      <TemperatureThresholds>
        <TemperatureThreshold><UpThreshold>70</UpThreshold><DownThreshold>62</DownThreshold><FanSpeed>80</FanSpeed></TemperatureThreshold>
        <TemperatureThreshold><UpThreshold>0</UpThreshold><DownThreshold>0</DownThreshold><FanSpeed>0</FanSpeed></TemperatureThreshold>
        <TemperatureThreshold><UpThreshold>55</UpThreshold><DownThreshold>48</DownThreshold><FanSpeed>40</FanSpeed></TemperatureThreshold>
      </TemperatureThresholds>
      <FanSpeedPercentageOverrides>
        <FanSpeedPercentageOverride><FanSpeedPercentage>40</FanSpeedPercentage><FanSpeedValue>77</FanSpeedValue><TargetOperation>ReadWrite</TargetOperation></FanSpeedPercentageOverride>
      </FanSpeedPercentageOverrides>
    </FanConfiguration>
  </FanConfigurations>
  <RegisterWriteConfigurations>
    <RegisterWriteConfiguration>
      <WriteMode>Or</WriteMode><WriteOccasion>OnInitialization</WriteOccasion><Register>50</Register><Value>4</Value>
      <ResetRequired>true</ResetRequired><ResetValue>1</ResetValue><ResetWriteMode>Set</ResetWriteMode>
    </RegisterWriteConfiguration>
  </RegisterWriteConfigurations>
</FanControlConfigV2>)";

const char* kJson = R"({
 "NotebookModel": "Example Book 16", "Author": "tests", "EcPollInterval": 50, "ReadWriteWords": true, "CriticalTemperature": 85,
 "FanConfigurations": [
  { "ReadRegister": 100, "WriteRegister": 102, "MinSpeedValue": 0, "MaxSpeedValue": 1000, "ResetRequired": true,
    "FanSpeedResetValue": 513,
    "TemperatureThresholds": [ {"UpThreshold": 0, "DownThreshold": 0, "FanSpeed": 10},
                               {"UpThreshold": 60, "DownThreshold": 50, "FanSpeed": 50.5} ] }
 ],
 "RegisterWriteConfigurations": [
  { "WriteMode": "Set", "WriteOccasion": "OnWriteFanSpeed", "Register": 120, "Value": 9 }
 ]
})";

void TestNbfc() {
    nbfc::Config x, j, bad;
    std::wstring err;
    Check(nbfc::Load(TempFile(L"x.xml", kXml), x, err), "NBFC XML loads");
    Check(x.model == L"Example Laptop 14 EX1400" && x.fans.size() == 1 && x.registers.size() == 1, "XML fields");
    Check(x.fans[0].thresholds[0].up == 0 && x.fans[0].thresholds[2].up == 70, "thresholds sorted by Up");
    Check(nbfc::PercentToValue(x.fans[0], 40) == 77, "override takes precedence");
    Check(nbfc::PercentToValue(x.fans[0], 50) == 60, "linear 50% of 10..110 = 60");

    MockEc ec;
    ec.mem[50] = 1;
    nbfc::Engine e;
    e.Start(x, {x.fans[0].thresholds});
    e.Step(ec, 40);
    Check(ec.mem[50] == 5, "OnInitialization Or write");
    Check(e.Targets()[0] == 0 && ec.mem[41] == 10, "40C -> 0% -> min value");
    e.Step(ec, 56);
    Check(e.Targets()[0] == 40 && ec.mem[41] == 77, "56C -> 40% (override value)");
    e.Step(ec, 50);
    Check(e.Targets()[0] == 40, "50C holds (hysteresis)");
    e.Step(ec, 47);
    Check(e.Targets()[0] == 0, "47C drops below Down 48");
    e.Step(ec, 91);
    Check(e.Targets()[0] == 100 && ec.mem[41] == 110, "91C critical -> 100%");
    e.Step(ec, 80);
    Check(e.Targets()[0] == 100, "80C still critical (until < 75)");
    e.Step(ec, 74);
    Check(e.Targets()[0] == 80, "74C leaves critical");
    e.Stop(ec);
    Check(ec.mem[50] == 1, "register reset value written on stop");

    Check(nbfc::Load(TempFile(L"j.json", kJson), j, err), "NBFC JSON loads");
    Check(j.pollMs == 100 && j.fans[0].writeWords && !j.registers[0].onInit, "JSON fields (poll clamped to 100)");
    MockEc ec2;
    nbfc::Engine e2;
    e2.Start(j, {j.fans[0].thresholds});
    e2.Step(ec2, 65);
    Check(ec2.mem[102] == (505 & 0xff) && ec2.mem[103] == (505 >> 8), "16-bit little-endian write (50.5% of 1000)");
    Check(ec2.mem[120] == 9, "OnWriteFanSpeed register write");
    e2.Stop(ec2);
    Check(ec2.mem[102] == (513 & 0xff) && ec2.mem[103] == (513 >> 8), "fan reset value written as word");

    std::string acpi = kJson;
    acpi.insert(acpi.find("\"ReadRegister\""), "\"ReadAcpiMethod\": \"\\\\_SB.FAN\", ");
    Check(!nbfc::Load(TempFile(L"acpi.json", acpi), bad, err), "configs using ACPI methods are rejected");
}

void TestIdentify() {
    for (const Table& t : kFactoryTables) Check(identify::IsFanTable(t.data()), "factory table recognised");
    Table broken = kFactoryTables[0];
    broken[22] = 0x80;  // row 2 CPU 'up' out of range
    Check(!identify::IsFanTable(broken.data()), "corrupted table rejected");

    std::vector<std::uint8_t> image(5000, 0xA5);
    for (std::size_t i = 0; i < 4; ++i)
        std::copy(kFactoryTables[i].begin(), kFactoryTables[i].end(), image.begin() + 777 + static_cast<std::ptrdiff_t>(i * kTableSize));
    const auto groups = identify::ScanBytes(image, L"image");
    Check(groups.size() == 1 && groups[0].offset == 777 && groups[0].tables.size() == 4, "scanner finds one group of 4 tables");

    identify::Aml aml;
    aml.hasEcDevice = true;
    aml.regions.push_back({"ERAM", 0, 0xFE708500, 0x100, true});
    aml.fields.push_back({"ERAM", "FNLV", 0x54 * 8, 8});
    Check(identify::AcerEcFamily(aml), "Acer EC family recognised (FNLV in 0xFE70xxxx window)");
    aml.regions[0].base = 0xFED80000;
    Check(!identify::AcerEcFamily(aml), "different EC window rejected");

    Check(identify::ModelNameMatches(L"Example Laptop 14 EX1400", L"Laptop 14 EX1400"), "model names match");
    Check(!identify::ModelNameMatches(L"Example Laptop 14 EX1400", L"Other 15 ZZ900"), "model names differ");
}

void TestModelDefs() {
    ModelDef m = BuiltinModel();
    m.product = L"TEST-MODEL";
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    const std::filesystem::path file = std::filesystem::path(dir) / L"LaptopFanCurveTests_model.ini";
    Check(SaveModel(file, m), "definition saves");
    WritePrivateProfileStringW(L"model", L"confirmed", L"1", file.c_str());  // a shared/edited file claiming confirmation
    ModelDef back;
    Check(LoadModel(file, back) && back.profiles.size() == 4 && back.profiles[3].factory == kFactoryTables[3], "definition round-trips");
    Check(!back.confirmed, "confirmed flag in a file is ignored");
    const std::wstring fp = ModelFingerprint(back);
    back.profiles[0].factory[30] ^= 1;
    Check(ModelFingerprint(back) != fp, "fingerprint changes with the tables");
}

void TestSafety() {
    using safety::AcerAction;
    int missing = 0;
    // Acer watchdog: only a quieter-than-Acer curve is guarded.
    Check(safety::AcerWatchdog(missing, true, true, true, 90, 80) == AcerAction::None, "quieter curve at 90C: keep");
    Check(safety::AcerWatchdog(missing, true, true, true, 70, 95) == AcerAction::RestoreHot, "GPU reaches 95C: restore");
    Check(safety::AcerWatchdog(missing, true, false, true, 99, 99) == AcerAction::None, "curve cooling at least as much as Acer's: not guarded");
    Check(safety::AcerWatchdog(missing, false, true, true, 99, 99) == AcerAction::None, "curve not applied: nothing to restore");

    missing = 0;
    AcerAction last = AcerAction::None;
    for (int tick = 1; tick <= 4; ++tick) last = safety::AcerWatchdog(missing, true, true, false, 0, 0);
    Check(last == AcerAction::None, "readings missing for 4 ticks: wait");
    Check(safety::AcerWatchdog(missing, true, true, false, 0, 0) == AcerAction::RestoreNoReadings, "missing for 5 ticks: restore");
    missing = 0;
    for (int tick = 1; tick <= 3; ++tick) safety::AcerWatchdog(missing, true, true, false, 0, 0);
    safety::AcerWatchdog(missing, true, true, true, 60, 60);  // a reading comes back
    for (int tick = 1; tick <= 4; ++tick) last = safety::AcerWatchdog(missing, true, true, false, 0, 0);
    Check(last == AcerAction::None, "a returning reading resets the count");

    // NBFC: lost temperature hands the fans back.
    missing = 0;
    Check(safety::NbfcTemperature(missing, true) == safety::NbfcAction::Run, "NBFC with a reading: run");
    for (int tick = 1; tick <= 4; ++tick) Check(safety::NbfcTemperature(missing, false) == safety::NbfcAction::Wait, "NBFC without a reading: wait");
    Check(safety::NbfcTemperature(missing, false) == safety::NbfcAction::HandBack, "NBFC without a reading for 5 ticks: hand back");

    // EC errors: five in a row turn the custom curve off; a success in between resets the count.
    int errors = 0;
    bool reached = false;
    for (int i = 1; i <= 4; ++i) reached = safety::EcErrorLimitReached(errors, true);
    Check(!reached, "4 EC errors: keep going");
    safety::EcErrorLimitReached(errors, false);
    for (int i = 1; i <= 4; ++i) reached = safety::EcErrorLimitReached(errors, true);
    Check(!reached, "a success resets the EC error count");
    Check(safety::EcErrorLimitReached(errors, true), "5th consecutive EC error: turn the curve off");
    Check(errors == 0, "error count starts over afterwards");

    // Re-apply and Confirm's first-row probe.
    Check(safety::NeedsReapply(false, 0x41, 0x41), "not applied: write the table");
    Check(safety::NeedsReapply(true, 0x40, 0x41), "EC reloaded its table (canary changed): write again");
    Check(!safety::NeedsReapply(true, 0x41, 0x41), "canary intact: nothing to do");
    for (const Table& t : kFactoryTables) Check(safety::LooksLikeFanTableRow0(t.data()), "factory row 0 passes the probe");
    const std::uint8_t zeros[10] = {}, ones[10] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    Check(!safety::LooksLikeFanTableRow0(zeros) && !safety::LooksLikeFanTableRow0(ones), "non-table bytes stop Confirm");

    // Scenario: an NBFC curve is running and the temperature reading disappears.
    nbfc::Config c;
    std::wstring err;
    nbfc::Load(TempFile(L"scenario.xml", kXml), c, err);
    MockEc ec;
    nbfc::Engine engine;
    engine.Start(c, {c.fans[0].thresholds});
    missing = 0;
    const int temps[] = {60, 60, -1, -1, -1, -1, -1};  // -1 = no reading
    for (int t : temps) {
        const safety::NbfcAction act = safety::NbfcTemperature(missing, t >= 0);
        if (act == safety::NbfcAction::Run) engine.Step(ec, t);
        else if (act == safety::NbfcAction::HandBack && engine.Running()) engine.Stop(ec);
    }
    Check(!engine.Running() && ec.mem[50] == 1, "scenario: fans handed back with reset values after the reading was lost");
}

}  // namespace

int main() {
    TestNbfc();
    TestIdentify();
    TestModelDefs();
    TestSafety();
    std::printf("\n%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
