#pragma once
#include <string>

// ============================================================================
// Settings parsed from settings.txt
// ============================================================================

enum class StaticAlgorithm { FSR1, FSR2, FSR3 };
enum class JitterCancel     { APP_CONTROLLED, ON, OFF };
enum class DynamicRange     { SDR, HDR };
enum class JitterInterp     { BILINEAR, BICUBIC, LANCZOS3 };

struct AppSettings
{
    // 3-D scene passes
    bool  run_fsr1_3d   = true;
    bool  run_fsr2_3d   = true;
    bool  run_fsr3_3d   = true;

    // Static PNG pass master switch
    bool  run_static    = true;

    // --- Static PNG pass knobs ---
    std::string static_input_name  = "input.png";
    std::string static_output_name = "output.png";

    DynamicRange static_input_dr  = DynamicRange::SDR;
    DynamicRange static_output_dr = DynamicRange::SDR;

    StaticAlgorithm static_algo       = StaticAlgorithm::FSR2;
    bool            static_upscaling  = true;
    float           static_scale      = 2.0f;   // display = render * scale
    bool            static_rcas       = true;
    float           static_sharpness  = 0.8f;   // 0..1

    bool            static_jitter     = true;
    int             static_jitter_frames = 32;
    JitterInterp    static_jitter_interp = JitterInterp::BILINEAR;

    float           static_depth      = 0.0f;   // 0=background, 1=nearest

    JitterCancel    static_jitter_cancel = JitterCancel::APP_CONTROLLED;
};

// Loads settings from file; missing keys get defaults; bad values get defaults.
AppSettings LoadSettings(const std::string& path = "settings.txt");
