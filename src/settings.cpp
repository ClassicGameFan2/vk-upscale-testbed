#include "settings.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <cstdlib>   // strtof

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Returns everything before the first '#' (comment character)
static std::string stripComment(const std::string& s)
{
    size_t p = s.find('#');
    return (p == std::string::npos) ? s : s.substr(0, p);
}

static std::string upperStr(std::string s)
{
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

static bool parseBool(const std::string& raw, bool def)
{
    std::string v = upperStr(trim(raw));
    if (v == "ON"  || v == "1" || v == "TRUE" || v == "YES") return true;
    if (v == "OFF" || v == "0" || v == "FALSE"|| v == "NO" ) return false;
    std::cout << "[Settings] Bad bool value '" << raw
              << "', using default " << (def?"ON":"OFF") << "\n";
    return def;
}

static float parseFloat(const std::string& raw, float def, float minV, float maxV)
{
    std::string v = trim(raw);
    if (v.empty()) return def;
    char* end = nullptr;
    float f = std::strtof(v.c_str(), &end);
    if (end == v.c_str() || std::isnan(f) || std::isinf(f) || f < minV || f > maxV) {
        std::cout << "[Settings] Bad float value '" << raw
                  << "' (expected " << minV << ".." << maxV
                  << "), using default " << def << "\n";
        return def;
    }
    return f;
}

static int parseInt(const std::string& raw, int def, int minV, int maxV)
{
    std::string v = trim(raw);
    if (v.empty()) return def;
    char* end = nullptr;
    long  l   = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str() || l < minV || l > maxV) {
        std::cout << "[Settings] Bad int value '" << raw
                  << "' (expected " << minV << ".." << maxV
                  << "), using default " << def << "\n";
        return def;
    }
    return (int)l;
}

// ── Public API ────────────────────────────────────────────────────────────────

AppSettings LoadSettings(const std::string& path)
{
    AppSettings s; // initialised with defaults

    std::ifstream f(path);
    if (!f.is_open()) {
        std::cout << "[Settings] '" << path
                  << "' not found – using all defaults.\n";
        return s;
    }

    std::cout << "[Settings] Loading '" << path << "'\n";

    std::string line;
    while (std::getline(f, line)) {
        line = stripComment(line);
        line = trim(line);
        if (line.empty()) continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = upperStr(trim(line.substr(0, eq)));
        std::string val = trim(line.substr(eq + 1));

        // Remove inline parenthetical descriptions that users might accidentally
        // leave in (everything from first '(' onwards is treated as comment).
        {
            size_t p = val.find('(');
            if (p != std::string::npos) val = trim(val.substr(0, p));
        }

        if      (key == "RUN_FSR1_3D_SCENE")        s.run_fsr1_3d    = parseBool(val, true);
        else if (key == "RUN_FSR2_3D_SCENE")        s.run_fsr2_3d    = parseBool(val, true);
        else if (key == "RUN_FSR3_3D_SCENE")        s.run_fsr3_3d    = parseBool(val, true);
        else if (key == "RUN_STATIC_PNG_PASS")      s.run_static     = parseBool(val, true);

        else if (key == "STATIC_INPUT_NAME") {
            if (!val.empty()) s.static_input_name = val;
        }
        else if (key == "STATIC_OUTPUT_NAME") {
            if (!val.empty()) s.static_output_name = val;
        }

        else if (key == "STATIC_INPUT_DYNAMIC_RANGE") {
            std::string v = upperStr(val);
            if      (v == "SDR") s.static_input_dr = DynamicRange::SDR;
            else if (v == "HDR") s.static_input_dr = DynamicRange::HDR;
            else {
                std::cout << "[Settings] Bad Static_input_dynamic_range '"
                          << val << "', using SDR\n";
                s.static_input_dr = DynamicRange::SDR;
            }
        }
        else if (key == "STATIC_OUTPUT_DYNAMIC_RANGE") {
            std::string v = upperStr(val);
            if      (v == "SDR") s.static_output_dr = DynamicRange::SDR;
            else if (v == "HDR") s.static_output_dr = DynamicRange::HDR;
            else {
                std::cout << "[Settings] Bad Static_output_dynamic_range '"
                          << val << "', using SDR\n";
                s.static_output_dr = DynamicRange::SDR;
            }
        }

        else if (key == "STATIC_ALGORITHM") {
            std::string v = upperStr(val);
            if      (v == "FSR1") s.static_algo = StaticAlgorithm::FSR1;
            else if (v == "FSR2") s.static_algo = StaticAlgorithm::FSR2;
            else if (v == "FSR3") s.static_algo = StaticAlgorithm::FSR3;
            else {
                std::cout << "[Settings] Bad Static_algorithm '"
                          << val << "', using FSR2\n";
                s.static_algo = StaticAlgorithm::FSR2;
            }
        }

        else if (key == "STATIC_UPSCALING")
            s.static_upscaling = parseBool(val, true);

        else if (key == "STATIC_SCALE") {
            // Min 1.0 – we do not support downscaling
            s.static_scale = parseFloat(val, 2.0f, 1.0f, 32.0f);
        }

        else if (key == "STATIC_RCAS")
            s.static_rcas = parseBool(val, true);

        else if (key == "STATIC_RCAS_SHARPNESS")
            s.static_sharpness = parseFloat(val, 0.8f, 0.0f, 1.0f);

        else if (key == "STATIC_JITTER")
            s.static_jitter = parseBool(val, true);

        else if (key == "STATIC_JITTER_FRAMES")
            s.static_jitter_frames = parseInt(val, 32, 1, 100000);

        else if (key == "STATIC_JITTER_INTERPOLATION") {
            std::string v = upperStr(val);
            if      (v == "BILINEAR") s.static_jitter_interp = JitterInterp::BILINEAR;
            else if (v == "BICUBIC" ) s.static_jitter_interp = JitterInterp::BICUBIC;
            else if (v == "LANCZOS3") s.static_jitter_interp = JitterInterp::LANCZOS3;
            else {
                std::cout << "[Settings] Bad Static_Jitter_Interpolation '"
                          << val << "', using bilinear\n";
                s.static_jitter_interp = JitterInterp::BILINEAR;
            }
        }

        else if (key == "STATIC_DEPTH")
            s.static_depth = parseFloat(val, 0.0f, 0.0f, 1.0f);

        else if (key == "STATIC_SET_JITTER_CANCEL_FLAG") {
            std::string v = upperStr(val);
            if      (v == "APP_CONTROLLED") s.static_jitter_cancel = JitterCancel::APP_CONTROLLED;
            else if (v == "ON"            ) s.static_jitter_cancel = JitterCancel::ON;
            else if (v == "OFF"           ) s.static_jitter_cancel = JitterCancel::OFF;
            else {
                std::cout << "[Settings] Bad Static_Set_Jitter_Cancel_Flag '"
                          << val << "', using APP_CONTROLLED\n";
                s.static_jitter_cancel = JitterCancel::APP_CONTROLLED;
            }
        }
        else {
            std::cout << "[Settings] Unknown key '" << key << "' – ignored.\n";
        }
    }

    // --- Post-validation / overrides ---

    // If upscaling is OFF, force scale to 1.0 (we cannot run RCAS in isolation
    // through the FSR1/2/3 host API without also running the upscaling pass).
    if (!s.static_upscaling) {
        if (s.static_scale != 1.0f) {
            std::cout << "[Settings] Static_upscaling=OFF -> overriding scale to 1.0\n";
            s.static_scale = 1.0f;
        }
    }

    // HDR output not yet implemented for static PNG pass.
    if (s.static_output_dr == DynamicRange::HDR) {
        std::cout << "[Settings] WARNING: HDR output not yet implemented for "
                     "static PNG pass. Falling back to SDR output.\n";
        s.static_output_dr = DynamicRange::SDR;
    }
    if (s.static_input_dr == DynamicRange::HDR) {
        std::cout << "[Settings] WARNING: HDR input not yet implemented for "
                     "static PNG pass. Will treat as SDR.\n";
        s.static_input_dr = DynamicRange::SDR;
    }

    // Print effective settings
    std::cout << "[Settings] Effective static PNG pass settings:\n"
              << "  input="       << s.static_input_name
              << "  output="      << s.static_output_name        << "\n"
              << "  algo="        << (s.static_algo == StaticAlgorithm::FSR1 ? "FSR1" :
                                      s.static_algo == StaticAlgorithm::FSR2 ? "FSR2" : "FSR3")
              << "  upscaling="   << (s.static_upscaling ? "ON" : "OFF")
              << "  scale="       << s.static_scale              << "\n"
              << "  RCAS="        << (s.static_rcas      ? "ON" : "OFF")
              << "  sharpness="   << s.static_sharpness          << "\n"
              << "  jitter="      << (s.static_jitter    ? "ON" : "OFF")
              << "  frames="      << s.static_jitter_frames
              << "  depth="       << s.static_depth              << "\n";

    return s;
}
