#pragma once
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

// Minimal, dependency-free JSON helpers used by Config::load/save.
namespace cfgjson {

inline std::string readAll(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Returns the text inside `"key": { ... }`.
inline bool findSection(const std::string& src, const char* key, std::string& out) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = src.find(pat);
    if (p == std::string::npos) return false;
    p = src.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    p = src.find('{', p);
    if (p == std::string::npos) return false;

    int depth = 0;
    const size_t start = p + 1;
    for (size_t i = p; i < src.size(); ++i) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}') {
            if (--depth == 0) { out = src.substr(start, i - start); return true; }
        }
    }
    return false;
}

// Returns the raw token (string body / number / true / false) for `"key": value`.
// Only quoted tokens that are actually object keys are accepted, so a value that
// happens to have the same text (e.g. "mode": "smooth" vs "smooth": 0.35) is skipped.
inline bool findValue(const std::string& src, const char* key, std::string& out) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t search = 0;
    size_t p = std::string::npos;
    for (;;) {
        p = src.find(pat, search);
        if (p == std::string::npos) return false;
        search = p + pat.size();

        size_t b = p;
        while (b > 0 && (src[b - 1] == ' ' || src[b - 1] == '\t' || src[b - 1] == '\r' || src[b - 1] == '\n')) --b;
        // b == 0 means this is the first key of the section body (the '{' was stripped).
        if (b != 0 && src[b - 1] != '{' && src[b - 1] != ',') continue;

        size_t c = search;
        while (c < src.size() && (src[c] == ' ' || src[c] == '\t' || src[c] == '\r' || src[c] == '\n')) ++c;
        if (c >= src.size() || src[c] != ':') continue;
        p = c;
        break;
    }
    ++p;
    while (p < src.size() && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) ++p;
    if (p >= src.size()) return false;

    if (src[p] == '"') {
        const size_t e = src.find('"', p + 1);
        if (e == std::string::npos) return false;
        out = src.substr(p + 1, e - p - 1);
        return true;
    }

    size_t e = p;
    while (e < src.size() && src[e] != ',' && src[e] != '}' && src[e] != '\n' && src[e] != '\r') ++e;
    const std::string tok = src.substr(p, e - p);
    const size_t b = tok.find_first_not_of(" \t");
    if (b == std::string::npos) return false;
    const size_t l = tok.find_last_not_of(" \t");
    out = tok.substr(b, l - b + 1);
    return true;
}

inline bool getBool(const std::string& s, const char* k, bool& v) {
    std::string t; if (!findValue(s, k, t)) return false;
    v = (t == "true" || t == "1"); return true;
}
inline bool getInt(const std::string& s, const char* k, int& v) {
    std::string t; if (!findValue(s, k, t)) return false;
    v = std::atoi(t.c_str()); return true;
}
inline bool getFloat(const std::string& s, const char* k, float& v) {
    std::string t; if (!findValue(s, k, t)) return false;
    v = (float)std::atof(t.c_str()); return true;
}
inline bool getString(const std::string& s, const char* k, std::string& v) {
    return findValue(s, k, v);
}

} // namespace cfgjson

struct AimConfig {
    bool enabled = true;
    std::string mode = "smooth";      // smooth | snap | prediction | trigger_snap | instant | silent
    std::string priority = "nearest"; // nearest | lowest_health | highest_threat
    int fovRadius = 300;
    float smooth = 0.35f;
    float headOffset = 0.20f;
    int predictionMs = 35;
    float deadZonePx = 1.0f;
    bool humanize = true;
    bool recoilCompensation = false;
};

struct TriggerConfig {
    bool enabled = false;
    int radiusPx = 40;
    int holdMs = 45;
    int fireRateMs = 90;
};

struct EspConfig {
    bool enabled = true;
    bool box = true;
    bool snapline = true;
    bool health = true;
    bool distance = false;
    bool name = false;
    bool fovCircle = true;
    bool radar = false;
    bool targetHighlight = false;
    int radarSize = 180;
    std::string boxColor = "#00FF88";
    std::string snaplineColor = "#FFFFFF";
    std::string fovColor = "#00BFFF";
};

struct SilentConfig {
    bool enabled = false;
};

struct StealthConfig {
    bool randomizeNames = true;   // window class / title / log file carry no product string
    bool hideFromCapture = true;  // overlay is excluded from screen capture
    bool suppressConsole = true;  // no console window inside the game process
    bool panicOnDetect = false;   // unhook and quit when an anti-cheat module shows up
};

struct HotkeyConfig {
    int toggleAim = 0x70;     // F1
    int toggleTrigger = 0x71; // F2
    int toggleEsp = 0x72;     // F3
    int toggleSilent = 0x73;  // F4
    int panic = 0x23;         // END
};

class Config {
public:
    AimConfig aim;
    TriggerConfig trigger;
    EspConfig esp;
    SilentConfig silent;
    StealthConfig stealth;
    HotkeyConfig hotkeys;

    bool load(const char* path) {
        const std::string src = cfgjson::readAll(path);
        if (src.empty()) return false;

        std::string s;
        if (cfgjson::findSection(src, "aim", s)) {
            cfgjson::getBool(s, "enabled", aim.enabled);
            cfgjson::getString(s, "mode", aim.mode);
            cfgjson::getString(s, "priority", aim.priority);
            cfgjson::getInt(s, "fovRadius", aim.fovRadius);
            cfgjson::getFloat(s, "smooth", aim.smooth);
            cfgjson::getFloat(s, "headOffset", aim.headOffset);
            cfgjson::getInt(s, "predictionMs", aim.predictionMs);
            cfgjson::getFloat(s, "deadZonePx", aim.deadZonePx);
            cfgjson::getBool(s, "humanize", aim.humanize);
            cfgjson::getBool(s, "recoilCompensation", aim.recoilCompensation);
        }
        if (cfgjson::findSection(src, "trigger", s)) {
            cfgjson::getBool(s, "enabled", trigger.enabled);
            cfgjson::getInt(s, "radiusPx", trigger.radiusPx);
            cfgjson::getInt(s, "holdMs", trigger.holdMs);
            cfgjson::getInt(s, "fireRateMs", trigger.fireRateMs);
        }
        if (cfgjson::findSection(src, "esp", s)) {
            cfgjson::getBool(s, "enabled", esp.enabled);
            cfgjson::getBool(s, "box", esp.box);
            cfgjson::getBool(s, "snapline", esp.snapline);
            cfgjson::getBool(s, "health", esp.health);
            cfgjson::getBool(s, "distance", esp.distance);
            cfgjson::getBool(s, "name", esp.name);
            cfgjson::getBool(s, "fovCircle", esp.fovCircle);
            cfgjson::getBool(s, "radar", esp.radar);
            cfgjson::getBool(s, "targetHighlight", esp.targetHighlight);
            cfgjson::getInt(s, "radarSize", esp.radarSize);
            cfgjson::getString(s, "boxColor", esp.boxColor);
            cfgjson::getString(s, "snaplineColor", esp.snaplineColor);
            cfgjson::getString(s, "fovColor", esp.fovColor);
        }
        if (cfgjson::findSection(src, "silent", s)) {
            cfgjson::getBool(s, "enabled", silent.enabled);
        }
        if (cfgjson::findSection(src, "stealth", s)) {
            cfgjson::getBool(s, "randomizeNames", stealth.randomizeNames);
            cfgjson::getBool(s, "hideFromCapture", stealth.hideFromCapture);
            cfgjson::getBool(s, "suppressConsole", stealth.suppressConsole);
            cfgjson::getBool(s, "panicOnDetect", stealth.panicOnDetect);
        }
        if (cfgjson::findSection(src, "hotkeys", s)) {
            cfgjson::getInt(s, "toggleAim", hotkeys.toggleAim);
            cfgjson::getInt(s, "toggleTrigger", hotkeys.toggleTrigger);
            cfgjson::getInt(s, "toggleEsp", hotkeys.toggleEsp);
            cfgjson::getInt(s, "toggleSilent", hotkeys.toggleSilent);
            cfgjson::getInt(s, "panic", hotkeys.panic);
        }
        return true;
    }

    bool save(const char* path) const {
        std::ofstream o(path, std::ios::binary | std::ios::trunc);
        if (!o) return false;
        auto b = [](bool v) { return v ? "true" : "false"; };

        o << "{\n";
        o << "  \"aim\": {\n"
          << "    \"enabled\": " << b(aim.enabled) << ",\n"
          << "    \"mode\": \"" << aim.mode << "\",\n"
          << "    \"priority\": \"" << aim.priority << "\",\n"
          << "    \"fovRadius\": " << aim.fovRadius << ",\n"
          << "    \"smooth\": " << aim.smooth << ",\n"
          << "    \"headOffset\": " << aim.headOffset << ",\n"
          << "    \"predictionMs\": " << aim.predictionMs << ",\n"
          << "    \"deadZonePx\": " << aim.deadZonePx << ",\n"
          << "    \"humanize\": " << b(aim.humanize) << ",\n"
          << "    \"recoilCompensation\": " << b(aim.recoilCompensation) << "\n"
          << "  },\n";
        o << "  \"trigger\": {\n"
          << "    \"enabled\": " << b(trigger.enabled) << ",\n"
          << "    \"radiusPx\": " << trigger.radiusPx << ",\n"
          << "    \"holdMs\": " << trigger.holdMs << ",\n"
          << "    \"fireRateMs\": " << trigger.fireRateMs << "\n"
          << "  },\n";
        o << "  \"esp\": {\n"
          << "    \"enabled\": " << b(esp.enabled) << ",\n"
          << "    \"box\": " << b(esp.box) << ",\n"
          << "    \"snapline\": " << b(esp.snapline) << ",\n"
          << "    \"health\": " << b(esp.health) << ",\n"
          << "    \"distance\": " << b(esp.distance) << ",\n"
          << "    \"name\": " << b(esp.name) << ",\n"
          << "    \"fovCircle\": " << b(esp.fovCircle) << ",\n"
          << "    \"radar\": " << b(esp.radar) << ",\n"
          << "    \"targetHighlight\": " << b(esp.targetHighlight) << ",\n"
          << "    \"radarSize\": " << esp.radarSize << ",\n"
          << "    \"boxColor\": \"" << esp.boxColor << "\",\n"
          << "    \"snaplineColor\": \"" << esp.snaplineColor << "\",\n"
          << "    \"fovColor\": \"" << esp.fovColor << "\"\n"
          << "  },\n";
        o << "  \"silent\": {\n"
          << "    \"enabled\": " << b(silent.enabled) << "\n"
          << "  },\n";
        o << "  \"stealth\": {\n"
          << "    \"randomizeNames\": " << b(stealth.randomizeNames) << ",\n"
          << "    \"hideFromCapture\": " << b(stealth.hideFromCapture) << ",\n"
          << "    \"suppressConsole\": " << b(stealth.suppressConsole) << ",\n"
          << "    \"panicOnDetect\": " << b(stealth.panicOnDetect) << "\n"
          << "  },\n";
        o << "  \"hotkeys\": {\n"
          << "    \"toggleAim\": " << hotkeys.toggleAim << ",\n"
          << "    \"toggleTrigger\": " << hotkeys.toggleTrigger << ",\n"
          << "    \"toggleEsp\": " << hotkeys.toggleEsp << ",\n"
          << "    \"toggleSilent\": " << hotkeys.toggleSilent << ",\n"
          << "    \"panic\": " << hotkeys.panic << "\n"
          << "  }\n";
        o << "}\n";
        return true;
    }
};
