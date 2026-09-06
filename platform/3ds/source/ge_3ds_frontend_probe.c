#include "ge_3ds_frontend_probe.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Live frontend input bridge mapping (see original_frontend_input_edges):
 * CONFIRM = next weapon (A) or fire, BACK = use (B), START = pause,
 * COPY = previous weapon (X), ERASE = reload (Y). */
#define PROBE_ACTION_CONFIRM GE_PORT_ACTION_NEXT_WEAPON
#define PROBE_ACTION_BACK GE_PORT_ACTION_USE
#define PROBE_ACTION_START GE_PORT_ACTION_PAUSE
#define PROBE_ACTION_COPY GE_PORT_ACTION_PREV_WEAPON
#define PROBE_ACTION_ERASE GE_PORT_ACTION_RELOAD

#define PROBE_DEFAULT_TIMEOUT 600U
#define PROBE_CURSOR_SETTLE 0.75f

static const char *const probe_menu_names[] = {
    "legal", "nintendo", "rareware", "eye-intro", "goldeneye-logo",
    "file-select", "mode-select", "mission-select", "difficulty",
    "007-options", "briefing", "run-stage", "mission-failed",
    "mission-complete", "mp-options", "mp-char-select", "mp-handicap",
    "mp-control-style", "mp-stage-select", "mp-scenario-select", "mp-teams",
    "cheat", "no-controllers", "switch-screens", "display-cast",
    "spectrum-emu"
};

const char *ge_3ds_frontend_probe_menu_name(int32_t menu)
{
    if (menu < 0
            || (size_t)menu >= sizeof(probe_menu_names) / sizeof(probe_menu_names[0]))
        return "unknown";
    return probe_menu_names[menu];
}

static int32_t probe_menu_by_name(const char *name)
{
    size_t index;
    char *end = NULL;
    long value;
    for (index = 0U;
            index < sizeof(probe_menu_names) / sizeof(probe_menu_names[0]);
            ++index)
        if (strcmp(name, probe_menu_names[index]) == 0) return (int32_t)index;
    value = strtol(name, &end, 10);
    if (end != NULL && *end == '\0' && end != name && value >= 0 && value < 64)
        return (int32_t)value;
    return -1;
}

void ge_3ds_frontend_probe_note(Ge3dsFrontendProbe *probe, const char *key,
                                uint64_t value)
{
    size_t index;
    if (probe == NULL || !probe->enabled || key == NULL) return;
    for (index = 0U; index < probe->note_count; ++index) {
        if (strcmp(probe->notes[index].key, key) == 0) {
            probe->notes[index].value = value;
            return;
        }
    }
    if (probe->note_count >= sizeof(probe->notes) / sizeof(probe->notes[0]))
        return;
    (void)snprintf(probe->notes[probe->note_count].key,
        sizeof(probe->notes[probe->note_count].key), "%s", key);
    probe->notes[probe->note_count].value = value;
    probe->note_count++;
}

static void probe_fail(Ge3dsFrontendProbe *probe, const char *message)
{
    if (probe->failed) return;
    probe->failed = true;
    probe->finished = true;
    (void)snprintf(probe->failure, sizeof(probe->failure), "%s", message);
}

static bool probe_parse_buttons(const char *text, uint32_t *buttons)
{
    char copy[128];
    char *cursor;
    char *token;
    *buttons = 0U;
    if (strlen(text) >= sizeof(copy)) return false;
    strcpy(copy, text);
    cursor = copy;
    while ((token = strtok(cursor, "|+,")) != NULL) {
        cursor = NULL;
        if (strcmp(token, "CONFIRM") == 0 || strcmp(token, "A") == 0)
            *buttons |= GE_3DS_FRONTEND_PROBE_BUTTON_CONFIRM;
        else if (strcmp(token, "BACK") == 0 || strcmp(token, "B") == 0)
            *buttons |= GE_3DS_FRONTEND_PROBE_BUTTON_BACK;
        else if (strcmp(token, "START") == 0)
            *buttons |= GE_3DS_FRONTEND_PROBE_BUTTON_START;
        else if (strcmp(token, "COPY") == 0 || strcmp(token, "X") == 0)
            *buttons |= GE_3DS_FRONTEND_PROBE_BUTTON_COPY;
        else if (strcmp(token, "ERASE") == 0 || strcmp(token, "Y") == 0)
            *buttons |= GE_3DS_FRONTEND_PROBE_BUTTON_ERASE;
        else if (strcmp(token, "UP") == 0)
            *buttons |= GE_3DS_FRONTEND_PROBE_BUTTON_UP;
        else if (strcmp(token, "DOWN") == 0)
            *buttons |= GE_3DS_FRONTEND_PROBE_BUTTON_DOWN;
        else if (strcmp(token, "LEFT") == 0)
            *buttons |= GE_3DS_FRONTEND_PROBE_BUTTON_LEFT;
        else if (strcmp(token, "RIGHT") == 0)
            *buttons |= GE_3DS_FRONTEND_PROBE_BUTTON_RIGHT;
        else
            return false;
    }
    return *buttons != 0U;
}

bool ge_3ds_frontend_probe_load(Ge3dsFrontendProbe *probe, FILE *stream,
                                uint64_t clock_hz)
{
    char line[256];
    char magic[32];
    unsigned version = 0U;
    bool header_ok;
    if (probe == NULL) return false;
    memset(probe, 0, sizeof(*probe));
    probe->clock_hz = clock_hz != 0U ? clock_hz : 268111856ULL;
    probe->max_frames = 7200U;
    probe->last_menu = INT32_MIN;
    probe->applied_step_index = SIZE_MAX;
    if (stream == NULL) return false;
    header_ok = fgets(line, sizeof(line), stream) != NULL
        && sscanf(line, "%31s %u", magic, &version) == 2
        && strcmp(magic, "GE_FRONTEND_PROBE") == 0 && version == 1U;
    if (!header_ok) {
        probe_fail(probe, "bad header");
        return false;
    }
    while (fgets(line, sizeof(line), stream) != NULL) {
        char keyword[32];
        char argument[64];
        char extra[64];
        unsigned value = 0U;
        Ge3dsFrontendProbeStep step;
        int fields;
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;
        if (sscanf(line, "%31s", keyword) != 1) continue;
        if (strcmp(keyword, "max_frames") == 0) {
            if (sscanf(line, "%*s %u", &value) != 1 || value == 0U
                    || value > GE_3DS_FRONTEND_PROBE_MAX_FRAMES) {
                probe_fail(probe, "bad max_frames");
                return false;
            }
            probe->max_frames = value;
            continue;
        }
        if (strcmp(keyword, "exit") == 0) {
            probe->exit_when_done = sscanf(line, "%*s %u", &value) == 1
                && value != 0U;
            continue;
        }
        if (strcmp(keyword, "auto_capture") == 0) {
            if (sscanf(line, "%*s %u", &value) != 1) {
                probe_fail(probe, "bad auto_capture");
                return false;
            }
            probe->auto_capture_delay = value;
            continue;
        }
        if (strcmp(keyword, "gameplay_frames") == 0) {
            if (sscanf(line, "%*s %u", &value) != 1
                    || value > GE_3DS_FRONTEND_PROBE_MAX_FRAMES) {
                probe_fail(probe, "bad gameplay_frames");
                return false;
            }
            probe->gameplay_frames = value;
            continue;
        }
        if (strcmp(keyword, "gameplay_capture") == 0) {
            if (sscanf(line, "%*s %u", &value) != 1 || value == 0U) {
                probe_fail(probe, "bad gameplay_capture");
                return false;
            }
            probe->gameplay_capture_frame = value;
            continue;
        }
        if (strcmp(keyword, "step") != 0) {
            probe_fail(probe, "unknown keyword");
            return false;
        }
        if (probe->step_count >= GE_3DS_FRONTEND_PROBE_STEP_CAPACITY) {
            probe_fail(probe, "too many steps");
            return false;
        }
        memset(&step, 0, sizeof(step));
        step.menu = -1;
        step.wallet = -1;
        fields = sscanf(line, "%*s %31s %63s %63s", keyword, argument, extra);
        if (fields < 1) {
            probe_fail(probe, "bad step");
            return false;
        }
        if (strcmp(keyword, "wait") == 0) {
            step.kind = GE_3DS_FRONTEND_PROBE_STEP_WAIT;
            if (fields < 2 || sscanf(argument, "%u", &value) != 1
                    || value == 0U) {
                probe_fail(probe, "bad wait");
                return false;
            }
            step.frames = value;
        } else if (strcmp(keyword, "press") == 0) {
            step.kind = GE_3DS_FRONTEND_PROBE_STEP_PRESS;
            if (fields < 2 || !probe_parse_buttons(argument, &step.buttons)) {
                probe_fail(probe, "bad press");
                return false;
            }
            step.frames = 1U;
        } else if (strcmp(keyword, "hold") == 0) {
            step.kind = GE_3DS_FRONTEND_PROBE_STEP_HOLD;
            if (fields < 3 || !probe_parse_buttons(argument, &step.buttons)
                    || sscanf(extra, "%u", &value) != 1 || value == 0U) {
                probe_fail(probe, "bad hold");
                return false;
            }
            step.frames = value;
        } else if (strcmp(keyword, "cursor") == 0) {
            step.kind = GE_3DS_FRONTEND_PROBE_STEP_CURSOR;
            if (fields < 3 || sscanf(argument, "%f", &step.x) != 1
                    || sscanf(extra, "%f", &step.y) != 1
                    || !isfinite(step.x) || !isfinite(step.y)) {
                probe_fail(probe, "bad cursor");
                return false;
            }
            step.frames = PROBE_DEFAULT_TIMEOUT;
        } else if (strcmp(keyword, "wallet") == 0) {
            step.kind = GE_3DS_FRONTEND_PROBE_STEP_WALLET;
            if (fields < 2 || sscanf(argument, "%u", &value) != 1
                    || value >= 4U) {
                probe_fail(probe, "bad wallet");
                return false;
            }
            step.wallet = (int32_t)value;
            step.frames = PROBE_DEFAULT_TIMEOUT;
        } else if (strcmp(keyword, "menu") == 0) {
            step.kind = GE_3DS_FRONTEND_PROBE_STEP_MENU;
            if (fields < 2 || (step.menu = probe_menu_by_name(argument)) < 0) {
                probe_fail(probe, "bad menu");
                return false;
            }
            step.frames = PROBE_DEFAULT_TIMEOUT;
            if (fields >= 3 && sscanf(extra, "%u", &value) == 1 && value != 0U)
                step.frames = value;
        } else if (strcmp(keyword, "capture") == 0) {
            step.kind = GE_3DS_FRONTEND_PROBE_STEP_CAPTURE;
            if (fields < 2
                    || strlen(argument) >= GE_3DS_FRONTEND_PROBE_NAME_CAPACITY) {
                probe_fail(probe, "bad capture");
                return false;
            }
            strcpy(step.name, argument);
        } else if (strcmp(keyword, "stage") == 0) {
            step.kind = GE_3DS_FRONTEND_PROBE_STEP_STAGE;
            step.frames = PROBE_DEFAULT_TIMEOUT;
            if (fields >= 2 && sscanf(argument, "%u", &value) == 1
                    && value != 0U)
                step.frames = value;
        } else if (strcmp(keyword, "end") == 0) {
            step.kind = GE_3DS_FRONTEND_PROBE_STEP_END;
        } else {
            probe_fail(probe, "unknown step");
            return false;
        }
        probe->steps[probe->step_count++] = step;
    }
    probe->frames = calloc(probe->max_frames, sizeof(*probe->frames));
    if (probe->frames == NULL) {
        probe_fail(probe, "timing allocation failed");
        return false;
    }
    probe->frame_capacity = probe->max_frames;
    probe->enabled = true;
    return true;
}

bool ge_3ds_frontend_probe_load_path(Ge3dsFrontendProbe *probe,
                                     const char *path, uint64_t clock_hz)
{
    FILE *stream;
    bool ok;
    if (probe == NULL) return false;
    memset(probe, 0, sizeof(*probe));
    if (path == NULL || (stream = fopen(path, "rb")) == NULL) return false;
    ok = ge_3ds_frontend_probe_load(probe, stream, clock_hz);
    fclose(stream);
    return ok;
}

void ge_3ds_frontend_probe_free(Ge3dsFrontendProbe *probe)
{
    if (probe == NULL) return;
    free(probe->frames);
    probe->frames = NULL;
    probe->frame_capacity = 0U;
}

/* Inverse of ge_original_frontend_cursor_tick's magnitude model: a stick
 * value s with |s| >= 6 moves the cursor (|s| - 5) * 0.075 + 0.5 authored
 * pixels per tick, saturating at 70 after the deadzone. */
static float probe_steer_axis(float distance)
{
    float magnitude = fabsf(distance);
    float stick;
    if (magnitude < PROBE_CURSOR_SETTLE) return 0.0f;
    stick = (magnitude - 0.5f) / 0.075f + 5.0f;
    if (stick < 6.0f) stick = 6.0f;
    if (stick > 75.0f) stick = 75.0f;
    stick = floorf(stick);
    return (distance < 0.0f ? -stick : stick) / 80.0f;
}

static bool probe_step_target(const Ge3dsFrontendProbe *probe,
    const Ge3dsFrontendProbeStep *step,
    const Ge3dsFrontendProbeObservation *observed, float *x, float *y)
{
    (void)probe;
    if (step->kind == GE_3DS_FRONTEND_PROBE_STEP_CURSOR) {
        *x = step->x;
        *y = step->y;
        return true;
    }
    if (step->kind == GE_3DS_FRONTEND_PROBE_STEP_WALLET
            && observed->wallet_bounds_ready
            && observed->wallet_bounds != NULL) {
        const GeOriginalFrontendWalletBounds *bounds =
            &observed->wallet_bounds[step->wallet];
        *x = (bounds->left + bounds->right) * 0.5f;
        *y = (bounds->top + bounds->bottom) * 0.5f;
        return true;
    }
    return false;
}

void ge_3ds_frontend_probe_apply_input(
    Ge3dsFrontendProbe *probe, const Ge3dsFrontendProbeObservation *observed,
    GePortInput *input)
{
    const Ge3dsFrontendProbeStep *step;
    uint32_t buttons = 0U;
    uint32_t actions = 0U;
    if (probe == NULL || !probe->enabled || probe->finished || input == NULL
            || probe->step_index >= probe->step_count)
        return;
    memset(input, 0, sizeof(*input));
    step = &probe->steps[probe->step_index];
    probe->applied_step_index = probe->step_index;
    switch (step->kind) {
    case GE_3DS_FRONTEND_PROBE_STEP_PRESS:
    case GE_3DS_FRONTEND_PROBE_STEP_HOLD:
        buttons = step->buttons;
        break;
    case GE_3DS_FRONTEND_PROBE_STEP_CURSOR:
    case GE_3DS_FRONTEND_PROBE_STEP_WALLET: {
        float target_x;
        float target_y;
        if (observed != NULL
                && probe_step_target(probe, step, observed, &target_x, &target_y)) {
            input->move_x = probe_steer_axis(target_x - observed->cursor.x);
            /* Stick up (positive move_y) decreases authored cursor y. */
            input->move_y = -probe_steer_axis(target_y - observed->cursor.y);
        }
        return;
    }
    default:
        return;
    }
    if (buttons & GE_3DS_FRONTEND_PROBE_BUTTON_CONFIRM) actions |= PROBE_ACTION_CONFIRM;
    if (buttons & GE_3DS_FRONTEND_PROBE_BUTTON_BACK) actions |= PROBE_ACTION_BACK;
    if (buttons & GE_3DS_FRONTEND_PROBE_BUTTON_START) actions |= PROBE_ACTION_START;
    if (buttons & GE_3DS_FRONTEND_PROBE_BUTTON_COPY) actions |= PROBE_ACTION_COPY;
    if (buttons & GE_3DS_FRONTEND_PROBE_BUTTON_ERASE) actions |= PROBE_ACTION_ERASE;
    if (buttons & GE_3DS_FRONTEND_PROBE_BUTTON_UP) input->move_y = 1.0f;
    if (buttons & GE_3DS_FRONTEND_PROBE_BUTTON_DOWN) input->move_y = -1.0f;
    if (buttons & GE_3DS_FRONTEND_PROBE_BUTTON_LEFT) input->move_x = -1.0f;
    if (buttons & GE_3DS_FRONTEND_PROBE_BUTTON_RIGHT) input->move_x = 1.0f;
    input->held = actions;
    /* Only the first frame of a hold produces button edges, like a real
     * controller sample; the frontend bridge derives edges from `pressed`. */
    input->pressed = probe->step_elapsed == 0U ? actions : 0U;
}

static void probe_schedule_capture(Ge3dsFrontendProbe *probe, const char *name)
{
    if (probe->capture_pending) return;
    probe->capture_pending = true;
    probe->capture_frame = probe->frame_index;
    (void)snprintf(probe->capture_name, sizeof(probe->capture_name), "%s", name);
    if (probe->capture_count < sizeof(probe->captures) / sizeof(probe->captures[0])) {
        Ge3dsFrontendProbeCapture *record = &probe->captures[probe->capture_count++];
        (void)snprintf(record->name, sizeof(record->name), "%s", name);
        record->frame = probe->frame_index;
        record->written = 0U;
    }
}

static void probe_complete_step(Ge3dsFrontendProbe *probe, int32_t menu)
{
    probe->step_done_frame[probe->step_index] = probe->frame_index;
    probe->step_done_menu[probe->step_index] = menu;
    probe->step_index++;
    probe->step_elapsed = 0U;
    if (probe->step_index >= probe->step_count) probe->finished = true;
}

void ge_3ds_frontend_probe_observe(
    Ge3dsFrontendProbe *probe, const Ge3dsFrontendProbeObservation *observed)
{
    if (probe == NULL || !probe->enabled || observed == NULL) return;
    if (observed->menu != probe->last_menu) {
        probe->last_menu = observed->menu;
        probe->last_menu_frame = probe->frame_index;
        probe->menu_transitions++;
        if (probe->auto_capture_delay != 0U) {
            probe->auto_capture_pending = true;
            probe->auto_capture_frame =
                probe->frame_index + probe->auto_capture_delay;
            (void)snprintf(probe->auto_capture_name,
                sizeof(probe->auto_capture_name), "menu%02ld-%s",
                (long)observed->menu,
                ge_3ds_frontend_probe_menu_name(observed->menu));
        }
    }
    if (probe->auto_capture_pending
            && probe->frame_index >= probe->auto_capture_frame) {
        probe->auto_capture_pending = false;
        probe_schedule_capture(probe, probe->auto_capture_name);
    }
    if (probe->frame_index >= probe->max_frames && !probe->finished) {
        probe_fail(probe, "max_frames reached");
        return;
    }
    if (probe->step_index < probe->step_count
            && (probe->steps[probe->step_index].kind
                    == GE_3DS_FRONTEND_PROBE_STEP_CURSOR
                || probe->steps[probe->step_index].kind
                    == GE_3DS_FRONTEND_PROBE_STEP_WALLET)
            && probe->applied_step_index != probe->step_index) {
        /* Steering needs at least one applied frame before its position
         * test can complete a step that another step chained into. */
        probe->applied_step_index = SIZE_MAX;
        return;
    }
    /* Instantaneous steps chain within one frame; every other kind consumes
     * exactly the frame whose input it produced. */
    while (!probe->finished && probe->step_index < probe->step_count) {
        const Ge3dsFrontendProbeStep *step = &probe->steps[probe->step_index];
        switch (step->kind) {
        case GE_3DS_FRONTEND_PROBE_STEP_CAPTURE:
            probe_schedule_capture(probe, step->name);
            probe_complete_step(probe, observed->menu);
            continue;
        case GE_3DS_FRONTEND_PROBE_STEP_END:
            probe_complete_step(probe, observed->menu);
            probe->finished = true;
            return;
        case GE_3DS_FRONTEND_PROBE_STEP_WAIT:
        case GE_3DS_FRONTEND_PROBE_STEP_PRESS:
        case GE_3DS_FRONTEND_PROBE_STEP_HOLD:
            /* A step reached through an instantaneous predecessor in this
             * same frame has not produced its controller sample yet. */
            if (probe->applied_step_index != probe->step_index) return;
            probe->step_elapsed++;
            if (probe->step_elapsed >= step->frames) {
                /* Following instantaneous steps run in this same frame. */
                probe_complete_step(probe, observed->menu);
                continue;
            }
            return;
        case GE_3DS_FRONTEND_PROBE_STEP_MENU:
            if (observed->menu == step->menu) {
                probe_complete_step(probe, observed->menu);
                continue;
            }
            if (++probe->step_elapsed >= step->frames) {
                probe_fail(probe, "menu step timed out");
            }
            return;
        case GE_3DS_FRONTEND_PROBE_STEP_STAGE:
            if (observed->stage_requested) {
                probe_complete_step(probe, observed->menu);
                continue;
            }
            if (++probe->step_elapsed >= step->frames)
                probe_fail(probe, "stage step timed out");
            return;
        case GE_3DS_FRONTEND_PROBE_STEP_CURSOR:
        case GE_3DS_FRONTEND_PROBE_STEP_WALLET: {
            float target_x;
            float target_y;
            if (probe_step_target(probe, step, observed, &target_x, &target_y)
                    && fabsf(target_x - observed->cursor.x) < PROBE_CURSOR_SETTLE
                    && fabsf(target_y - observed->cursor.y) < PROBE_CURSOR_SETTLE) {
                probe_complete_step(probe, observed->menu);
                continue;
            }
            if (++probe->step_elapsed >= step->frames)
                probe_fail(probe, "cursor step timed out");
            return;
        }
        default:
            probe_fail(probe, "invalid step");
            return;
        }
    }
}

static Ge3dsFrontendProbeMenuSummary *probe_menu_summary(
    Ge3dsFrontendProbe *probe, int32_t menu)
{
    size_t index;
    for (index = 0U; index < probe->menu_count; ++index)
        if (probe->menus[index].menu == menu) return &probe->menus[index];
    if (probe->menu_count >= GE_3DS_FRONTEND_PROBE_MENU_CAPACITY) return NULL;
    probe->menus[probe->menu_count].menu = menu;
    return &probe->menus[probe->menu_count++];
}

void ge_3ds_frontend_probe_record_frame(
    Ge3dsFrontendProbe *probe, uint64_t start_ticks, uint64_t work_ticks,
    uint64_t begin_wait_ticks, uint64_t vblank_wait_ticks,
    const uint64_t phase_ticks[5], int32_t menu)
{
    Ge3dsFrontendProbeMenuSummary *summary;
    uint64_t interval = 0U;
    size_t phase;
    if (probe == NULL || !probe->enabled) return;
    if (probe->previous_start_ticks != 0U && start_ticks > probe->previous_start_ticks)
        interval = start_ticks - probe->previous_start_ticks;
    probe->previous_start_ticks = start_ticks;
    if (probe->frames != NULL && probe->frame_count < probe->frame_capacity) {
        Ge3dsFrontendProbeFrame *frame = &probe->frames[probe->frame_count++];
        frame->work_ticks = work_ticks > UINT32_MAX ? UINT32_MAX : (uint32_t)work_ticks;
        frame->begin_wait_ticks = begin_wait_ticks > UINT32_MAX
            ? UINT32_MAX : (uint32_t)begin_wait_ticks;
        frame->vblank_wait_ticks = vblank_wait_ticks > UINT32_MAX
            ? UINT32_MAX : (uint32_t)vblank_wait_ticks;
        frame->interval_ticks = interval > UINT32_MAX ? UINT32_MAX : (uint32_t)interval;
        for (phase = 0U; phase < 5U; ++phase)
            frame->phase_ticks[phase] = phase_ticks == NULL ? 0U
                : phase_ticks[phase] > UINT32_MAX ? UINT32_MAX
                : (uint32_t)phase_ticks[phase];
        frame->menu = menu;
    }
    summary = probe_menu_summary(probe, menu);
    if (summary != NULL) {
        summary->frames++;
        summary->total_work_ticks += work_ticks;
        if (work_ticks > summary->peak_work_ticks)
            summary->peak_work_ticks = work_ticks > UINT32_MAX
                ? UINT32_MAX : (uint32_t)work_ticks;
        if (work_ticks * 60U > probe->clock_hz) summary->over_budget++;
        /* One and a half display intervals separates a dropped frame from
         * scheduling jitter around a single VBlank. */
        if (interval * 40U > probe->clock_hz) summary->multi_interval_frames++;
    }
    probe->frame_index++;
    probe->applied_step_index = SIZE_MAX;
}

bool ge_3ds_frontend_probe_capture_due(Ge3dsFrontendProbe *probe,
                                       char name[GE_3DS_FRONTEND_PROBE_NAME_CAPACITY])
{
    if (probe == NULL || !probe->enabled || !probe->capture_pending
            || probe->frame_index < probe->capture_frame + 2U)
        return false;
    memcpy(name, probe->capture_name, GE_3DS_FRONTEND_PROBE_NAME_CAPACITY);
    probe->capture_pending = false;
    return true;
}

void ge_3ds_frontend_probe_capture_written(Ge3dsFrontendProbe *probe,
                                           const char *name, bool ok)
{
    size_t index;
    if (probe == NULL) return;
    for (index = probe->capture_count; index > 0U; --index) {
        Ge3dsFrontendProbeCapture *record = &probe->captures[index - 1U];
        if (strcmp(record->name, name) == 0) {
            record->written = ok ? 1U : 2U;
            break;
        }
    }
    if (ok) probe->capture_written++;
}

bool ge_3ds_frontend_probe_write_ppm(const char *path,
                                     const uint8_t *framebuffer_bgr8)
{
    enum { WIDTH = 400, HEIGHT = 240 };
    uint8_t *pixels;
    FILE *stream;
    size_t x;
    size_t y;
    bool ok;
    if (path == NULL || framebuffer_bgr8 == NULL) return false;
    pixels = malloc((size_t)WIDTH * HEIGHT * 3U);
    if (pixels == NULL) return false;
    /* The top framebuffer is stored as 240 rows of 400 pixels rotated by 90
     * degrees: screen column x is framebuffer row x, and screen row y runs
     * from the end of that row back toward its start. */
    for (y = 0U; y < HEIGHT; ++y) {
        uint8_t *row = pixels + y * WIDTH * 3U;
        for (x = 0U; x < WIDTH; ++x) {
            const uint8_t *source =
                framebuffer_bgr8 + (x * HEIGHT + (HEIGHT - 1U - y)) * 3U;
            row[x * 3U + 0U] = source[2];
            row[x * 3U + 1U] = source[1];
            row[x * 3U + 2U] = source[0];
        }
    }
    stream = fopen(path, "wb");
    if (stream == NULL) {
        free(pixels);
        return false;
    }
    ok = fprintf(stream, "P6\n%d %d\n255\n", WIDTH, HEIGHT) > 0
        && fwrite(pixels, 1U, (size_t)WIDTH * HEIGHT * 3U, stream)
            == (size_t)WIDTH * HEIGHT * 3U;
    ok = fclose(stream) == 0 && ok;
    free(pixels);
    return ok;
}

bool ge_3ds_frontend_probe_write_result(
    const Ge3dsFrontendProbe *probe, FILE *stream, int32_t final_menu,
    bool stage_requested, int32_t level_id)
{
    size_t index;
    uint32_t over_budget = 0U;
    uint32_t multi_interval = 0U;
    uint64_t total_work = 0U;
    uint32_t peak_work = 0U;
    if (probe == NULL || stream == NULL) return false;
    for (index = 0U; index < probe->menu_count; ++index) {
        over_budget += probe->menus[index].over_budget;
        multi_interval += probe->menus[index].multi_interval_frames;
        total_work += probe->menus[index].total_work_ticks;
        if (probe->menus[index].peak_work_ticks > peak_work)
            peak_work = probe->menus[index].peak_work_ticks;
    }
    /* Reaching a stage request is the traversal's goal even when trailing
     * scripted presses were never needed. */
    fprintf(stream, "status=%s\n",
        probe->failed ? "failed"
        : (probe->finished || stage_requested) ? "complete" : "incomplete");
    if (probe->failed) fprintf(stream, "failure=%s\n", probe->failure);
    fprintf(stream, "clock_hz=%llu\n", (unsigned long long)probe->clock_hz);
    fprintf(stream, "frames=%lu\n", (unsigned long)probe->frame_index);
    fprintf(stream, "steps=%lu/%lu\n", (unsigned long)probe->step_index,
        (unsigned long)probe->step_count);
    fprintf(stream, "final_menu=%ld,%s\n", (long)final_menu,
        ge_3ds_frontend_probe_menu_name(final_menu));
    fprintf(stream, "stage_requested=%d,%ld\n", stage_requested ? 1 : 0,
        (long)level_id);
    fprintf(stream, "menu_transitions=%lu\n", (unsigned long)probe->menu_transitions);
    fprintf(stream, "captures=%lu,%lu\n", (unsigned long)probe->capture_count,
        (unsigned long)probe->capture_written);
    fprintf(stream, "work_summary=%lu,%lu,%lu,%llu\n",
        (unsigned long)over_budget, (unsigned long)multi_interval,
        (unsigned long)peak_work,
        (unsigned long long)(probe->frame_index != 0U
            ? total_work / probe->frame_index : 0U));
    for (index = 0U; index < probe->step_index && index < probe->step_count; ++index)
        fprintf(stream, "step_done=%lu,%u,%lu,%ld\n", (unsigned long)index,
            (unsigned)probe->steps[index].kind,
            (unsigned long)probe->step_done_frame[index],
            (long)probe->step_done_menu[index]);
    for (index = 0U; index < probe->capture_count; ++index)
        fprintf(stream, "capture=%s,%lu,%u\n", probe->captures[index].name,
            (unsigned long)probe->captures[index].frame,
            (unsigned)probe->captures[index].written);
    for (index = 0U; index < probe->menu_count; ++index) {
        const Ge3dsFrontendProbeMenuSummary *summary = &probe->menus[index];
        fprintf(stream, "menu_summary=%ld,%s,%lu,%lu,%lu,%lu,%llu\n",
            (long)summary->menu, ge_3ds_frontend_probe_menu_name(summary->menu),
            (unsigned long)summary->frames, (unsigned long)summary->over_budget,
            (unsigned long)summary->multi_interval_frames,
            (unsigned long)summary->peak_work_ticks,
            (unsigned long long)(summary->frames != 0U
                ? summary->total_work_ticks / summary->frames : 0U));
    }
    for (index = 0U; index < probe->note_count; ++index)
        fprintf(stream, "note=%s,%llu\n", probe->notes[index].key,
            (unsigned long long)probe->notes[index].value);
    fprintf(stream, "frontend_frame_count=%lu\n", (unsigned long)probe->frame_count);
    for (index = 0U; index < probe->frame_count; ++index) {
        const Ge3dsFrontendProbeFrame *frame = &probe->frames[index];
        fprintf(stream, "frontend_frame=%lu,%ld,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
            (unsigned long)(index + 1U), (long)frame->menu,
            (unsigned long)frame->work_ticks,
            (unsigned long)frame->begin_wait_ticks,
            (unsigned long)frame->vblank_wait_ticks,
            (unsigned long)frame->interval_ticks,
            (unsigned long)frame->phase_ticks[0],
            (unsigned long)frame->phase_ticks[1],
            (unsigned long)frame->phase_ticks[2],
            (unsigned long)frame->phase_ticks[3],
            (unsigned long)frame->phase_ticks[4]);
    }
    return true;
}
