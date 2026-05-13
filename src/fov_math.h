#ifndef ORIONCTL_FOV_MATH_H
#define ORIONCTL_FOV_MATH_H

typedef struct {
    float clamped;
    int   was_clamped;
} clamp_result_t;

float fov_from_zoom(float pixel_pitch_m,
                    float array_width_px,
                    float min_focal_m,
                    float zoom);

float zoom_from_fov(float pixel_pitch_m,
                    float array_width_px,
                    float min_focal_m,
                    float fov_rad);

float effective_max_zoom(float min_focal_m, float max_focal_m);

clamp_result_t clamp_zoom(float zoom, float max_zoom);

#endif
