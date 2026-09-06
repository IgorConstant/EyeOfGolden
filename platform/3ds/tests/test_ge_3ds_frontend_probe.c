/* Host test for the scripted frontend probe: parsing, controller synthesis,
 * step sequencing, cursor steering against the exact cursor model, timing
 * summaries and the result writer. */
#include "ge_3ds_frontend_probe.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mirror of ge_original_frontend_cursor_tick's magnitude model. */
static void cursor_tick(GeOriginalFrontendCursor *cursor, int8_t stick_x,
                        int8_t stick_y)
{
    int x = stick_x;
    int y = -(int)stick_y;
    if (x < -5) x += 5; else if (x >= 6) x -= 5; else x = 0;
    if (x > 70) x = 70; else if (x < -70) x = -70;
    if (y < -5) y += 5; else if (y >= 6) y -= 5; else y = 0;
    if (y > 70) y = 70; else if (y < -70) y = -70;
    if (x > 0) cursor->x += (float)x * 0.075f + 0.5f;
    else if (x < 0) cursor->x += (float)x * 0.075f - 0.5f;
    if (y > 0) cursor->y += (float)y * 0.075f + 0.5f;
    else if (y < 0) cursor->y += (float)y * 0.075f - 0.5f;
}

static FILE *script(const char *text)
{
    FILE *stream = tmpfile();
    assert(stream != NULL);
    fputs(text, stream);
    rewind(stream);
    return stream;
}

static void test_parse_and_reject(void)
{
    Ge3dsFrontendProbe probe;
    FILE *stream = script(
        "GE_FRONTEND_PROBE 1\n"
        "max_frames 100\n"
        "auto_capture 45\n"
        "gameplay_frames 60\n"
        "gameplay_capture 30\n"
        "exit 1\n"
        "# comment\n"
        "step menu file-select 300\n"
        "step wait 5\n"
        "step press START|CONFIRM\n"
        "step hold CONFIRM 3\n"
        "step cursor 100.5 200\n"
        "step wallet 2\n"
        "step capture shot\n"
        "step stage 120\n"
        "step end\n");
    assert(ge_3ds_frontend_probe_load(&probe, stream, 268111856ULL));
    fclose(stream);
    assert(probe.enabled && probe.max_frames == 100U
        && probe.auto_capture_delay == 45U && probe.gameplay_frames == 60U
        && probe.gameplay_capture_frame == 30U && probe.exit_when_done);
    assert(probe.step_count == 9U);
    assert(probe.steps[0].kind == GE_3DS_FRONTEND_PROBE_STEP_MENU
        && probe.steps[0].menu == 5 && probe.steps[0].frames == 300U);
    assert(probe.steps[2].buttons == (GE_3DS_FRONTEND_PROBE_BUTTON_START
        | GE_3DS_FRONTEND_PROBE_BUTTON_CONFIRM));
    assert(probe.steps[3].kind == GE_3DS_FRONTEND_PROBE_STEP_HOLD
        && probe.steps[3].frames == 3U);
    assert(probe.steps[4].x == 100.5f && probe.steps[4].y == 200.0f);
    assert(probe.steps[5].wallet == 2);
    assert(strcmp(probe.steps[6].name, "shot") == 0);
    assert(probe.steps[7].frames == 120U);
    ge_3ds_frontend_probe_free(&probe);

    stream = script("GE_FRONTEND_PROBE 1\nstep press NOPE\n");
    assert(!ge_3ds_frontend_probe_load(&probe, stream, 0U) && probe.failed);
    fclose(stream);
    stream = script("GE_INPUT_PROBE 1\n");
    assert(!ge_3ds_frontend_probe_load(&probe, stream, 0U) && !probe.enabled);
    fclose(stream);
    assert(!ge_3ds_frontend_probe_load(&probe, NULL, 0U) && !probe.enabled);
}

/* Instantaneous steps chain in the frame that completes their predecessor,
 * but a press reached that way must not be consumed before its controller
 * sample has been produced. */
static void test_press_after_capture(void)
{
    Ge3dsFrontendProbe probe;
    Ge3dsFrontendProbeObservation observed = {0};
    GePortInput input;
    FILE *stream = script(
        "GE_FRONTEND_PROBE 1\n"
        "step wait 2\nstep capture a\nstep press START\nstep end\n");
    assert(ge_3ds_frontend_probe_load(&probe, stream, 268111856ULL));
    fclose(stream);
    observed.menu = 4;
    /* Frames 0-1: wait. The capture chains as the wait completes and the
     * press is reached but stays unapplied. */
    for (int frame = 0; frame < 2; ++frame) {
        memset(&input, 0x7f, sizeof(input));
        ge_3ds_frontend_probe_apply_input(&probe, &observed, &input);
        assert(input.pressed == 0U && input.held == 0U);
        ge_3ds_frontend_probe_observe(&probe, &observed);
        ge_3ds_frontend_probe_record_frame(&probe, 1000U * (frame + 1U),
            10U, 0U, 0U, NULL, observed.menu);
    }
    assert(probe.step_index == 2U && probe.capture_pending
        && probe.capture_frame == 1U && !probe.finished);
    /* Frame 2: the press produces a START edge and completes; END chains. */
    ge_3ds_frontend_probe_apply_input(&probe, &observed, &input);
    assert(input.pressed == GE_PORT_ACTION_PAUSE && input.held == GE_PORT_ACTION_PAUSE);
    ge_3ds_frontend_probe_observe(&probe, &observed);
    assert(probe.finished && !probe.failed && probe.step_index == 4U);
    assert(probe.step_done_frame[2] == 2U);
    /* Frame 3 start: the image drawn at frame 1 sits in the back buffer. */
    {
        char name[GE_3DS_FRONTEND_PROBE_NAME_CAPACITY];
        assert(!ge_3ds_frontend_probe_capture_due(&probe, name));
        ge_3ds_frontend_probe_record_frame(&probe, 3000U, 10U, 0U, 0U, NULL, 4);
        assert(ge_3ds_frontend_probe_capture_due(&probe, name));
        assert(strcmp(name, "a") == 0 && !probe.capture_pending);
        ge_3ds_frontend_probe_capture_written(&probe, name, true);
        assert(probe.capture_written == 1U && probe.captures[0].written == 1U);
    }
    ge_3ds_frontend_probe_free(&probe);
}

static void test_cursor_steering(void)
{
    Ge3dsFrontendProbe probe;
    Ge3dsFrontendProbeObservation observed = {0};
    GePortInput input;
    FILE *stream = script(
        "GE_FRONTEND_PROBE 1\nstep cursor 73 270\nstep press CONFIRM\nstep end\n");
    int frames = 0;
    assert(ge_3ds_frontend_probe_load(&probe, stream, 268111856ULL));
    fclose(stream);
    observed.menu = 7;
    observed.cursor.x = 220.0f;
    observed.cursor.y = 165.0f;
    while (probe.step_index == 0U && frames < 200) {
        ge_3ds_frontend_probe_apply_input(&probe, &observed, &input);
        assert(input.pressed == 0U);
        cursor_tick(&observed.cursor, (int8_t)lrintf(input.move_x * 80.0f),
                    (int8_t)lrintf(input.move_y * 80.0f));
        ge_3ds_frontend_probe_observe(&probe, &observed);
        ge_3ds_frontend_probe_record_frame(&probe, 1000U * (frames + 1U),
            10U, 0U, 0U, NULL, observed.menu);
        ++frames;
    }
    assert(probe.step_index == 1U && !probe.failed);
    assert(fabsf(observed.cursor.x - 73.0f) < 0.75f
        && fabsf(observed.cursor.y - 270.0f) < 0.75f);
    /* Steering the full distance takes tens of frames at the clamped speed,
     * never a single teleporting frame. */
    assert(frames > 15 && frames < 60);
    ge_3ds_frontend_probe_free(&probe);
}

static void test_menu_timeout_and_wallet(void)
{
    Ge3dsFrontendProbe probe;
    Ge3dsFrontendProbeObservation observed = {0};
    GePortInput input;
    GeOriginalFrontendWalletBounds bounds[4] = {
        {50.0f, 60.0f, 150.0f, 160.0f}, {250.0f, 60.0f, 350.0f, 160.0f},
        {50.0f, 200.0f, 150.0f, 300.0f}, {250.0f, 200.0f, 350.0f, 300.0f},
    };
    FILE *stream = script(
        "GE_FRONTEND_PROBE 1\nstep wallet 3\nstep menu mode-select 3\nstep end\n");
    assert(ge_3ds_frontend_probe_load(&probe, stream, 268111856ULL));
    fclose(stream);
    observed.menu = 5;
    observed.cursor.x = 300.0f;
    observed.cursor.y = 250.0f; /* already at wallet 3's center */
    observed.wallet_bounds = bounds;
    observed.wallet_bounds_ready = true;
    ge_3ds_frontend_probe_apply_input(&probe, &observed, &input);
    assert(input.move_x == 0.0f && input.move_y == 0.0f);
    ge_3ds_frontend_probe_observe(&probe, &observed);
    assert(probe.step_index == 1U);
    for (int frame = 0; frame < 3; ++frame) {
        ge_3ds_frontend_probe_record_frame(&probe, 1000U * (frame + 1U),
            10U, 0U, 0U, NULL, 5);
        ge_3ds_frontend_probe_apply_input(&probe, &observed, &input);
        ge_3ds_frontend_probe_observe(&probe, &observed);
    }
    assert(probe.failed && strstr(probe.failure, "timed out") != NULL);
    ge_3ds_frontend_probe_free(&probe);
}

static void test_timing_summary_and_result(void)
{
    Ge3dsFrontendProbe probe;
    Ge3dsFrontendProbeObservation observed = {0};
    FILE *stream = script("GE_FRONTEND_PROBE 1\nauto_capture 1\nstep wait 4\nstep end\n");
    const uint64_t hz = 268111856ULL;
    const uint64_t budget = hz / 60U;
    uint64_t phases[5] = {1U, 2U, 3U, 4U, 5U};
    char *buffer = NULL;
    size_t size = 0U;
    assert(ge_3ds_frontend_probe_load(&probe, stream, hz));
    fclose(stream);
    observed.menu = 1;
    ge_3ds_frontend_probe_observe(&probe, &observed);
    ge_3ds_frontend_probe_record_frame(&probe, 10U, budget / 2U, 0U, 0U, phases, 1);
    ge_3ds_frontend_probe_observe(&probe, &observed);
    /* One over-budget frame and one two-VBlank start interval. */
    ge_3ds_frontend_probe_record_frame(&probe, 10U + 2U * budget, budget + 1U,
        0U, 0U, phases, 1);
    observed.menu = 2;
    ge_3ds_frontend_probe_observe(&probe, &observed);
    ge_3ds_frontend_probe_record_frame(&probe, 10U + 3U * budget, 5U, 0U, 0U,
        phases, 2);
    ge_3ds_frontend_probe_note(&probe, "vertices", 42U);
    ge_3ds_frontend_probe_note(&probe, "vertices", 43U);
    assert(probe.note_count == 1U && probe.notes[0].value == 43U);
    assert(probe.menu_count == 2U && probe.menus[0].menu == 1
        && probe.menus[0].frames == 2U && probe.menus[0].over_budget == 1U
        && probe.menus[0].multi_interval_frames == 1U
        && probe.menus[1].frames == 1U);
    assert(probe.menu_transitions == 2U);
    /* Auto capture scheduled one frame after the second menu appeared. */
    assert(probe.auto_capture_pending && probe.auto_capture_frame == 3U);
    {
        FILE *result = tmpfile();
        long length;
        assert(result != NULL);
        assert(ge_3ds_frontend_probe_write_result(&probe, result, 2, false, -1));
        length = ftell(result);
        assert(length > 0);
        buffer = calloc((size_t)length + 1U, 1U);
        size = (size_t)length;
        rewind(result);
        assert(fread(buffer, 1U, size, result) == size);
        fclose(result);
    }
    assert(strstr(buffer, "status=incomplete\n") != NULL);
    assert(strstr(buffer, "menu_summary=1,nintendo,2,1,1,") != NULL);
    assert(strstr(buffer, "menu_summary=2,rareware,1,0,0,") != NULL);
    assert(strstr(buffer, "note=vertices,43\n") != NULL);
    assert(strstr(buffer, "frontend_frame_count=3\n") != NULL);
    assert(strstr(buffer, "frontend_frame=3,2,5,0,0,") != NULL);
    assert(strstr(buffer, ",1,2,3,4,5\n") != NULL);
    free(buffer);
    ge_3ds_frontend_probe_free(&probe);
}

static void test_ppm_writer(void)
{
    uint8_t *framebuffer = calloc(240U * 400U * 3U, 1U);
    char path[64];
    FILE *stream;
    unsigned char header[16];
    long size;
    assert(framebuffer != NULL);
    /* Screen pixel (x=3, y=1) is framebuffer row x, entry 239 - y. */
    framebuffer[(3U * 240U + 238U) * 3U + 0U] = 0x10; /* B */
    framebuffer[(3U * 240U + 238U) * 3U + 1U] = 0x20; /* G */
    framebuffer[(3U * 240U + 238U) * 3U + 2U] = 0x30; /* R */
    snprintf(path, sizeof(path), "%s", "ge_probe_ppm_test.ppm");
    assert(ge_3ds_frontend_probe_write_ppm(path, framebuffer));
    stream = fopen(path, "rb");
    assert(stream != NULL);
    assert(fread(header, 1U, 15U, stream) == 15U);
    assert(memcmp(header, "P6\n400 240\n255\n", 15U) == 0);
    fseek(stream, 0, SEEK_END);
    size = ftell(stream);
    assert(size == 15L + 400L * 240L * 3L);
    fseek(stream, 15L + (1L * 400L + 3L) * 3L, SEEK_SET);
    assert(fread(header, 1U, 3U, stream) == 3U);
    assert(header[0] == 0x30 && header[1] == 0x20 && header[2] == 0x10);
    fclose(stream);
    remove(path);
    free(framebuffer);
}

int main(void)
{
    test_parse_and_reject();
    test_press_after_capture();
    test_cursor_steering();
    test_menu_timeout_and_wallet();
    test_timing_summary_and_result();
    test_ppm_writer();
    puts("3DS frontend probe: script parsing, step sequencing, steering, timing and captures");
    return 0;
}
