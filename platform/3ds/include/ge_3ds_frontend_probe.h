#ifndef GE_3DS_FRONTEND_PROBE_H
#define GE_3DS_FRONTEND_PROBE_H

/* Scripted, deterministic frontend (menu) traversal, per-frame CPU timing and
 * framebuffer captures. The probe never changes original menu logic: it only
 * substitutes the controller sample fed into the unchanged frontend tick and
 * records what the live loop did. Everything here is diagnostic-only and
 * inactive unless the configuration file exists. */

#include "ge_original_frontend_cursor.h"
#include "ge_port.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GE_3DS_FRONTEND_PROBE_STEP_CAPACITY 96U
#define GE_3DS_FRONTEND_PROBE_NAME_CAPACITY 32U
#define GE_3DS_FRONTEND_PROBE_MAX_FRAMES 36000U
#define GE_3DS_FRONTEND_PROBE_MENU_CAPACITY 32U

typedef enum Ge3dsFrontendProbeStepKind {
    GE_3DS_FRONTEND_PROBE_STEP_WAIT = 0,
    GE_3DS_FRONTEND_PROBE_STEP_PRESS,
    GE_3DS_FRONTEND_PROBE_STEP_HOLD,
    GE_3DS_FRONTEND_PROBE_STEP_CURSOR,
    GE_3DS_FRONTEND_PROBE_STEP_WALLET,
    GE_3DS_FRONTEND_PROBE_STEP_MENU,
    GE_3DS_FRONTEND_PROBE_STEP_CAPTURE,
    GE_3DS_FRONTEND_PROBE_STEP_STAGE,
    GE_3DS_FRONTEND_PROBE_STEP_END
} Ge3dsFrontendProbeStepKind;

/* Script button names map onto the live port actions consumed by the
 * frontend input bridge (CONFIRM = A/next weapon, BACK = B/use, START,
 * COPY = X, ERASE = Y). UP/DOWN/LEFT/RIGHT are one-frame stick edges. */
typedef enum Ge3dsFrontendProbeButton {
    GE_3DS_FRONTEND_PROBE_BUTTON_CONFIRM = 1U << 0,
    GE_3DS_FRONTEND_PROBE_BUTTON_BACK = 1U << 1,
    GE_3DS_FRONTEND_PROBE_BUTTON_START = 1U << 2,
    GE_3DS_FRONTEND_PROBE_BUTTON_COPY = 1U << 3,
    GE_3DS_FRONTEND_PROBE_BUTTON_ERASE = 1U << 4,
    GE_3DS_FRONTEND_PROBE_BUTTON_UP = 1U << 5,
    GE_3DS_FRONTEND_PROBE_BUTTON_DOWN = 1U << 6,
    GE_3DS_FRONTEND_PROBE_BUTTON_LEFT = 1U << 7,
    GE_3DS_FRONTEND_PROBE_BUTTON_RIGHT = 1U << 8
} Ge3dsFrontendProbeButton;

typedef struct Ge3dsFrontendProbeStep {
    uint8_t kind;
    uint32_t frames;   /* wait/hold length, or timeout for cursor/menu/stage */
    uint32_t buttons;
    float x;
    float y;
    int32_t menu;
    int32_t wallet;
    char name[GE_3DS_FRONTEND_PROBE_NAME_CAPACITY];
} Ge3dsFrontendProbeStep;

typedef struct Ge3dsFrontendProbeFrame {
    uint32_t work_ticks;       /* CPU work excluding frame-begin/VBlank waits */
    uint32_t begin_wait_ticks; /* C3D_FrameBegin blocking on the prior queue */
    uint32_t vblank_wait_ticks;
    uint32_t interval_ticks;   /* start-to-start interval */
    /* Breakdown of work: original tick+snapshot+music dispatch, intro/cast
     * scene ticks, text/sprite layout, model publication, GPU submission
     * plus music join and audio pump (minus the frame-begin wait). */
    uint32_t phase_ticks[5];
    int32_t menu;
} Ge3dsFrontendProbeFrame;

typedef struct Ge3dsFrontendProbeMenuSummary {
    int32_t menu;
    uint32_t frames;
    uint32_t over_budget;
    uint32_t peak_work_ticks;
    uint64_t total_work_ticks;
    uint32_t multi_interval_frames; /* start intervals of two or more VBlanks */
} Ge3dsFrontendProbeMenuSummary;

typedef struct Ge3dsFrontendProbeCapture {
    char name[GE_3DS_FRONTEND_PROBE_NAME_CAPACITY];
    uint32_t frame;
    uint32_t written;
} Ge3dsFrontendProbeCapture;

typedef struct Ge3dsFrontendProbe {
    bool enabled;
    bool finished;
    bool failed;
    bool exit_when_done;
    bool result_written;
    char failure[96];
    uint32_t max_frames;
    uint32_t auto_capture_delay; /* 0 disables per-menu automatic captures */
    uint32_t gameplay_frames;    /* post-frontend gameplay timing request */
    uint32_t gameplay_capture_frame; /* rendered gameplay frame to capture */
    Ge3dsFrontendProbeStep steps[GE_3DS_FRONTEND_PROBE_STEP_CAPACITY];
    size_t step_count;
    size_t step_index;
    size_t applied_step_index; /* step whose input was produced this frame */
    uint32_t step_elapsed;
    uint32_t step_done_frame[GE_3DS_FRONTEND_PROBE_STEP_CAPACITY];
    int32_t step_done_menu[GE_3DS_FRONTEND_PROBE_STEP_CAPACITY];
    uint32_t frame_index;      /* frames observed so far */
    Ge3dsFrontendProbeFrame *frames;
    size_t frame_capacity;
    size_t frame_count;
    uint64_t previous_start_ticks;
    uint64_t clock_hz;
    int32_t last_menu;
    uint32_t last_menu_frame;
    /* Pending capture: the image drawn during frame `capture_frame` is read
     * from the back buffer two iterations later, once the queue has swapped. */
    bool capture_pending;
    uint32_t capture_frame;
    char capture_name[GE_3DS_FRONTEND_PROBE_NAME_CAPACITY];
    Ge3dsFrontendProbeCapture captures[64];
    size_t capture_count;
    size_t capture_written;
    uint32_t auto_capture_frame;
    bool auto_capture_pending;
    char auto_capture_name[GE_3DS_FRONTEND_PROBE_NAME_CAPACITY];
    Ge3dsFrontendProbeMenuSummary menus[GE_3DS_FRONTEND_PROBE_MENU_CAPACITY];
    size_t menu_count;
    uint32_t menu_transitions;
    /* Free-form diagnostic values (model sizes, cache counts) for the result. */
    struct {
        char key[GE_3DS_FRONTEND_PROBE_NAME_CAPACITY];
        uint64_t value;
    } notes[24];
    size_t note_count;
} Ge3dsFrontendProbe;

/* Observed frontend state for the step machine. */
typedef struct Ge3dsFrontendProbeObservation {
    int32_t menu;
    GeOriginalFrontendCursor cursor;
    const GeOriginalFrontendWalletBounds *wallet_bounds; /* 4 or NULL */
    bool wallet_bounds_ready;
    bool stage_requested;
} Ge3dsFrontendProbeObservation;

/* Parses the script. Missing file leaves the probe disabled and returns
 * false; a malformed file disables the probe and records the failure. */
bool ge_3ds_frontend_probe_load(Ge3dsFrontendProbe *probe, FILE *stream,
                                uint64_t clock_hz);
bool ge_3ds_frontend_probe_load_path(Ge3dsFrontendProbe *probe,
                                     const char *path, uint64_t clock_hz);
void ge_3ds_frontend_probe_free(Ge3dsFrontendProbe *probe);

/* Substitutes the scripted controller sample for this frame. Steering uses
 * the exact cursor tick model so a step settles within one authored pixel. */
void ge_3ds_frontend_probe_apply_input(
    Ge3dsFrontendProbe *probe, const Ge3dsFrontendProbeObservation *observed,
    GePortInput *input);

/* Advances the step machine after the unchanged frontend tick. */
void ge_3ds_frontend_probe_observe(
    Ge3dsFrontendProbe *probe, const Ge3dsFrontendProbeObservation *observed);

/* Records one displayed frame's timing. */
void ge_3ds_frontend_probe_record_frame(
    Ge3dsFrontendProbe *probe, uint64_t start_ticks, uint64_t work_ticks,
    uint64_t begin_wait_ticks, uint64_t vblank_wait_ticks,
    const uint64_t phase_ticks[5], int32_t menu);

/* True when a scheduled capture should be read this frame (two frames after
 * its draw). Returns the capture name; the caller writes the image. */
bool ge_3ds_frontend_probe_capture_due(Ge3dsFrontendProbe *probe,
                                       char name[GE_3DS_FRONTEND_PROBE_NAME_CAPACITY]);
void ge_3ds_frontend_probe_capture_written(Ge3dsFrontendProbe *probe,
                                           const char *name, bool ok);

/* Writes a 400x240 binary PPM from a rotated 240x400 BGR8 3DS framebuffer. */
bool ge_3ds_frontend_probe_write_ppm(const char *path,
                                     const uint8_t *framebuffer_bgr8);

bool ge_3ds_frontend_probe_write_result(
    const Ge3dsFrontendProbe *probe, FILE *stream, int32_t final_menu,
    bool stage_requested, int32_t level_id);

const char *ge_3ds_frontend_probe_menu_name(int32_t menu);

/* Records or updates a named diagnostic value; a no-op when disabled. */
void ge_3ds_frontend_probe_note(Ge3dsFrontendProbe *probe, const char *key,
                                uint64_t value);

#ifdef __cplusplus
}
#endif

#endif
