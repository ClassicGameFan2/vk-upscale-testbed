#pragma once
#include <vector>

// ============================================================================
// Sub-pixel image resampling for jitter offsets.
//
// shiftX / shiftY are fractional pixel offsets to apply to the source image.
// Positive shiftX shifts content to the RIGHT (i.e. moves the camera left).
// Data is RGBA float, linear.
// ============================================================================

enum class ResampleMode { BILINEAR, BICUBIC, LANCZOS3 };

// Resample `src` (srcW x srcH, RGBA float) with a sub-pixel shift (shiftX,
// shiftY) into `dst` (same dimensions).  The shift is in pixels.
void ResampleWithShift(
    const float* src, int srcW, int srcH,
    float shiftX, float shiftY,
    float* dst,
    ResampleMode mode);
