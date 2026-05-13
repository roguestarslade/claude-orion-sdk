#include "../src/fov_math.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;
static int checks   = 0;

#define M_PI_F 3.14159265358979323846f

static float rad2deg_f(float r) { return r * (180.0f / M_PI_F); }
static float deg2rad_f(float d) { return d * (M_PI_F / 180.0f); }

static void check_near(const char *label, float got, float expected, float tol)
{
    checks++;
    float d = fabsf(got - expected);
    int ok = d <= tol;
    if (!ok) failures++;
    printf("  %-50s got=%.6f want=%.6f delta=%.2e tol=%.2e  %s\n",
           label, got, expected, d, tol, ok ? "ok" : "FAIL");
}

static void check_true(const char *label, int cond)
{
    checks++;
    if (!cond) failures++;
    printf("  %-50s  %s\n", label, cond ? "ok" : "FAIL");
}

typedef struct {
    const char *name;
    float pitch_m;
    float array_w;
    float f_min_m;
    float max_zoom;
    float expected_base_fov_rad;
} slot_t;

static void run_slot(const slot_t *s)
{
    printf("slot: %s  (pitch=%.2e m, w=%.0f, f_min=%.4f m, max_zoom=%.2f)\n",
           s->name, s->pitch_m, s->array_w, s->f_min_m, s->max_zoom);

    float base = fov_from_zoom(s->pitch_m, s->array_w, s->f_min_m, 1.0f);
    check_near("  fov_from_zoom(1.0) matches expected base",
               base, s->expected_base_fov_rad, 5e-4f);
    printf("    base_fov_deg = %.4f\n", rad2deg_f(base));

    float zooms[] = {1.0f, 1.5f, 2.0f, s->max_zoom};
    for (size_t i = 0; i < sizeof(zooms)/sizeof(zooms[0]); i++) {
        float K = zooms[i];
        float fov = fov_from_zoom(s->pitch_m, s->array_w, s->f_min_m, K);
        float Z   = zoom_from_fov(s->pitch_m, s->array_w, s->f_min_m, fov);
        char label[80];
        snprintf(label, sizeof(label), "  round-trip K=%.3f -> fov -> zoom", K);
        check_near(label, Z, K, 1e-4f);
    }

    float mz = effective_max_zoom(s->f_min_m, s->f_min_m * s->max_zoom);
    check_near("  effective_max_zoom matches expected",
               mz, s->max_zoom, 1e-3f);
}

static void test_clamp(void)
{
    printf("clamp tests:\n");
    clamp_result_t r;

    r = clamp_zoom(0.5f, 9.6f);
    check_true("clamp(0.5, 9.6): was_clamped", r.was_clamped == 1);
    check_near("clamp(0.5, 9.6): result == 1.0", r.clamped, 1.0f, 1e-6f);

    r = clamp_zoom(2.5f, 9.6f);
    check_true("clamp(2.5, 9.6): not clamped", r.was_clamped == 0);
    check_near("clamp(2.5, 9.6): result == 2.5", r.clamped, 2.5f, 1e-6f);

    r = clamp_zoom(20.0f, 9.6f);
    check_true("clamp(20, 9.6): was_clamped", r.was_clamped == 1);
    check_near("clamp(20, 9.6): result == 9.6", r.clamped, 9.6f, 1e-6f);

    r = clamp_zoom(50.0f, 0.0f);
    check_true("clamp(50, fallback): was_clamped", r.was_clamped == 1);
    check_near("clamp(50, fallback): result == 30",
               r.clamped, 30.0f, 1e-6f);
}

static void test_degenerate(void)
{
    printf("degenerate inputs:\n");
    check_near("fov_from_zoom(neg pitch) == 0",
               fov_from_zoom(-1e-6f, 1920, 0.01f, 1.0f), 0.0f, 1e-9f);
    check_near("zoom_from_fov(zero fov) == 0",
               zoom_from_fov(3.3e-6f, 1920, 0.0078f, 0.0f), 0.0f, 1e-9f);
}

int main(void)
{
    slot_t slots[] = {
        { "0 omnivis spotter", 3.3e-6f, 1920.0f, 0.050f,  9.60f, deg2rad_f(7.249f)  },
        { "1 flir mwir",       15.0e-6f, 640.0f, 0.025f, 10.00f, deg2rad_f(21.748f) },
        { "2 omnivis wide",    3.3e-6f, 1920.0f, 0.0078f, 6.41f, deg2rad_f(44.206f) },
    };
    for (size_t i = 0; i < sizeof(slots)/sizeof(slots[0]); i++) run_slot(&slots[i]);

    test_clamp();
    test_degenerate();

    printf("\n%d/%d checks passed%s\n", checks - failures, checks,
           failures ? "" : "  ALL GREEN");
    return failures ? 1 : 0;
}
