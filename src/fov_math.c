#include "fov_math.h"

#include <math.h>

#define FOV_MIN_ZOOM_FALLBACK  1.0f
#define FOV_MAX_ZOOM_FALLBACK 30.0f

float fov_from_zoom(float pixel_pitch_m,
                    float array_width_px,
                    float min_focal_m,
                    float zoom)
{
    if (pixel_pitch_m <= 0.0f || array_width_px <= 0.0f ||
        min_focal_m <= 0.0f || zoom <= 0.0f) {
        return 0.0f;
    }
    float num = pixel_pitch_m * array_width_px;
    float den = 2.0f * min_focal_m * zoom;
    return 2.0f * atanf(num / den);
}

float zoom_from_fov(float pixel_pitch_m,
                    float array_width_px,
                    float min_focal_m,
                    float fov_rad)
{
    if (pixel_pitch_m <= 0.0f || array_width_px <= 0.0f ||
        min_focal_m <= 0.0f || fov_rad <= 0.0f) {
        return 0.0f;
    }
    float num = pixel_pitch_m * array_width_px;
    float den = 2.0f * min_focal_m * tanf(fov_rad * 0.5f);
    if (den <= 0.0f) return 0.0f;
    return num / den;
}

float effective_max_zoom(float min_focal_m, float max_focal_m)
{
    if (min_focal_m > 0.0f && max_focal_m > 0.0f) {
        return max_focal_m / min_focal_m;
    }
    return FOV_MAX_ZOOM_FALLBACK;
}

clamp_result_t clamp_zoom(float zoom, float max_zoom)
{
    clamp_result_t r;
    float lo = FOV_MIN_ZOOM_FALLBACK;
    float hi = (max_zoom > lo) ? max_zoom : FOV_MAX_ZOOM_FALLBACK;
    if (zoom < lo)      { r.clamped = lo; r.was_clamped = 1; }
    else if (zoom > hi) { r.clamped = hi; r.was_clamped = 1; }
    else                { r.clamped = zoom; r.was_clamped = 0; }
    return r;
}
