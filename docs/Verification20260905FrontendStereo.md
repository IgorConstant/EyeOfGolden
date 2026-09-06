# Frontend traversal, menu budget and stereoscopic 3D — 2026-09-05

This pass adds a deterministic way to measure the original frontend (menus,
intros, cast roster) on the 3DS runtime, fixes the frontend defects it
exposed (including two orientation bugs that also affected gameplay
textures), moves frontend music synthesis to the New 3DS worker core, adds
stereoscopic rendering driven by the 3D slider, and switches live play to the
original two-controller control style on the New 3DS XL's two sticks. All
measurements are Azahar emulator CPU work on this Mac, not physical New 3DS XL
or original 3DS timing.

## Scripted frontend probe

`platform/3ds/source/ge_3ds_frontend_probe.c` substitutes the controller
sample fed to the unchanged frontend tick, records per-frame CPU work split
into five phases, and writes 400x240 PPM captures of the displayed image two
presentations after it was drawn. It is active only when
`sdmc:/3ds/goldeneye-3ds/frontend-probe.cfg` exists. Steps: `wait`, `press`,
`hold`, `cursor x y` (steers with the inverse of the exact cursor tick),
`wallet n`, `menu name`, `capture name`, `stage`, `end`; options
`auto_capture`, `gameplay_frames`, `gameplay_capture`, `exit`, `max_frames`.

- `scripts/run_frontend_probe.sh [cfg]` runs one isolated Azahar instance and
  collects `build/frontend-probe/<label>/{frontend-probe.result,
  gameplay.result, captures/}`.
- `scripts/analyze_3ds_frontend_probe.py` summarizes per-menu work, exact 60 Hz
  budget misses (`work * 60 > clock_hz`), start intervals above 1.5 VBlanks
  and the phase means (tick+music dispatch, intro/cast scene ticks, text
  layout, model publication, GPU submission + music join + audio pump).
- `scripts/analyze_stereo_captures.py` measures the horizontal disparity
  between left/right captures per row band; `scripts/ppm_to_png.py` converts.
- Scripts: `scripts/frontend-probes/solo-dam-start.cfg` (natural intro, then
  file/mode/mission/difficulty/briefing into Dam, 600 idle gameplay frames),
  `quick-dam-start.cfg` (skips intros), `intro-cast.cfg` (900 cast frames).
- Host tests: `scripts/test_3ds_frontend_diagnostics.sh` (step machine,
  steering against the cursor model, timing summaries, PPM layout, stereo
  math). Registered in `scripts/test_port.sh`.

`gameplay_frames` reuses the existing idle-controller input-probe timing path
after the menu requests a stage, so one run covers boot → menus → first
gameplay frames.

## Frontend fixes

- **File select footer.** `constructor_menu05_fileselect` draws only the
  SELECTFILE sprite at authored x=110 plus white, left-aligned Copy (x=247) and
  Erase (x=357) labels. The port also rendered a centered "Confirm" string on
  top of the sprite and centered the other two labels on the icon positions.
  `ge_3ds_original_frontend_build_draw_list` now renders only channels 1–2,
  left-aligned and vertically centered on the icon row; channel 0 keeps
  carrying the selected wallet in its status byte. Test updated.
- **Odd-width 4-bit textures sheared.** `tools/mktex/src/tex2png.c` expanded
  I4/IA4 texels as one continuous nibble stream, ignoring the unused nibble at
  the end of every odd-width row. Every 65-wide intensity texture (the Bond
  photograph quadrants `IMAGE_BROSNAN_*`, the `IMAGE_MI6_*` crest) shifted by
  one texel per row, which is the black-and-white diagonal pattern seen on the
  file-select wallets and the mode-select page. The converter now indexes
  `y * ((width + 1) / 2) + x / 2`. The N64 build does not consume tex2png
  output; the 3DS asset pack does and must be regenerated (`make 3ds-assets`).
- **Stale texture combiner state.** citro3d uploads a TexEnv only after
  `C3D_GetTexEnv`/`C3D_DirtyTexEnv` marked it dirty. `renderer_draw` fetched
  the stage pointer once per frame and then edited it in place for the sky,
  muzzle flash, beam, HUD and fade passes, so those edits were uploaded only
  when an intervening material application happened to dirty the stage. Every
  reconfiguration now re-fetches `C3D_GetTexEnv(0)`, and the shared font
  helper calls `C3D_DirtyTexEnv`. Mono output for the Dam spawn frame is
  pixel-identical before and after; the defect was visible only once a second
  render pass existed (see below).

- **Frontend models drawn upside down.** The frontend ortho space
  (`Mtx_OrthoTilt(0, 400, 0, 240)`) has y=0 at the bottom, but the pitem
  publication (`120 - y * scale`), the wallet projection and the wallet hit
  bounds all treated y as a top-down VI coordinate, and the pitem loop
  inverted its cull faces to compensate. The Nintendo, Rare and GoldenEye
  logos, the legal text and the file wallets were vertically mirrored (the
  folder row authored at +800 rendered at the bottom). Publication now uses
  `120 + y * scale`, wallets convert their bounds through `240 - y` once, and
  the cull inversion is gone. Verified with the `logo-120`, `legal` and
  `file-select` captures.
- **Catalog texture rows mapped upside down (all stages).** Rare stores
  texture rows bottom-up in the ROM: `tex2png` flips them to produce
  top-down PNGs, the original blits 2D sprites with `flipY = 1`, and the
  frontend's embedded (non-catalog) images decode ROM row 0 into the PICA's
  bottom row and display upright. So N64 `t = 0` is the visual bottom, yet
  `ge_3ds_scene_texture_map_uv` mapped `t = 0` to the top edge of each Tex3DS
  subtexture. Every catalog texture on world geometry, characters and
  frontend models was vertically mirrored; on small, roughly symmetric
  textures (bricks, faces at 40 px) this was not obvious. The mapper now
  interpolates from the atlas bottom edge (`test_row_orientation`). Before the
  fix the cast Bond's tuxedo showed the shirt at the waist and the face had
  its hairline mid-face; after it the bow tie, shirt, cuffs and face are
  upright (`castdense` vs `uvflip-cast` captures), and the Bond photograph,
  Dam briefing photo and the "FOR YOUR EYES ONLY" stamp read correctly.

## Controls (New 3DS XL)

Live gameplay now runs GoldenEye's own 2.2 "Galore" control style instead of
emulating 1.1 with a quantized C-Stick. The port presents two virtual N64
controllers exactly as the original two-controller styles expect: the
player's controller carries the C-Stick (turn/pitch) and every button, the
second controller carries the Circle Pad (walk/strafe) and the aim trigger
(`ge_original_input_set_dual_analog`, `ge_port_n64_buttons`,
`ge_original_bond_input_set_control_style`). Nothing in `bondviewProcessInput`
changes; the 2.2 body already provides analog strafe, analog look, aim +
Circle Pad down to crouch, aim + Circle Pad up/down to zoom, and remote-mine
detonation on A+B.

| New 3DS XL | Action |
| --- | --- |
| Circle Pad | Walk / strafe |
| C-Stick | Turn / look up-down |
| R or ZR | Fire (N64 Z on the player's controller) |
| L or ZL | Aim, hold (N64 Z on the second controller); + Circle Pad down crouches |
| B / Y | Use / reload (N64 B) |
| A / X | Next / previous weapon (N64 A / D-pad down) |
| START | Bond watch / pause |

Menus are unchanged: Circle Pad moves the cursor, A or R confirms, B backs
out. `sdmc:/3ds/goldeneye-3ds/controls.cfg` with `dual_analog 0` restores the
previous single-stick layout (Circle Pad moves and turns, C-Stick as
C-buttons, 1.1 body). RAMROM attract replays and input-probe timing routes
were recorded against the 1.1 layout and keep it automatically. Host tests:
`test_ge_port` (controller placement, no C-buttons in dual mode),
`test_ge_original_bond_input_frame`, `test_ge_original_bond_input_provider`
(style survives stage resets).

## Frontend performance

- **Music on the worker core.** The frontend rendered its music frame inline
  on the main thread. It now uses the same `begin_tick_60hz` / `finish`
  split as gameplay: the New 3DS worker synthesizes while the page is
  prepared and drawn, and the audio pump runs after the join. Original 3DS
  keeps the synchronous fallback inside those calls.
- **Frontend model draw merging.** Authored pitem display lists split the
  Nintendo logo into 288 batches (3,054 vertices) and the GoldenEye logo into
  94. Contiguous batches with identical material and texture are now one
  `C3D_DrawArrays` in both the logo and wallet loops (`dam_batches_compatible`).

Measured per-menu mean/p95 CPU work (Azahar, ms), natural intro route
(`build/frontend-probe/{run2,music2,final1}`):

| Page | Before | Music on worker | + draw merging | Notes |
| --- | ---: | ---: | ---: | --- |
| Legal | 1.28 / 1.30 | 1.29 / 1.30 | 1.29 / 1.30 | no music |
| Nintendo logo | 11.78 / 14.79 | 9.26 / 9.19 | 6.57 / 6.48 | 5.3 ms lit-vertex publication remains; draws 3.4 → 1.0 ms |
| Rare logo | 5.41 / 8.34 | 4.68 / 7.99 | 4.63 / 7.81 | |
| Eye intro | 7.23 / 14.78 | 6.62 / 11.90 | 6.62 / 11.89 | 3.3 ms gunbarrel/blood decode tick |
| GoldenEye logo | 5.40 / 7.58 | 4.16 / 6.65 | 3.85 / 6.65 | |
| File select | 6.64 / 7.09 | 6.75 / 8.24 | 6.66 / 7.93 | 2.5 ms music request/first frames |
| Mode select | 3.61 / 6.17 | 3.83 / 7.90 | 3.59 / 7.25 | |
| Mission select | 5.13 / 6.63 | 4.93 / 7.46 | 4.74 / 7.69 | |
| Difficulty | 3.10 / 5.72 | 3.19 / 7.52 | 3.02 / 7.19 | |
| Briefing | 3.39 / 6.34 | 3.46 / 7.44 | 3.48 / 7.66 | |
| Cast roster | 13.22 / 15.29 | 9.96 / 11.08 | not re-run | 7.2 ms cast model tick + CPU projection |

The p95 of pages with music includes the main thread joining the worker,
which the emulator serializes; the tick phase itself fell from 1.5–6 ms to
0.1–0.3 ms. Transition hitches remain: 133 ms (Nintendo logo load), 98 ms
(eye intro), 84 ms (mission select), 250 ms (file select wallets), 150–166 ms
(cast actor swaps). They are one-time asset decodes, not steady-state cost.
Gameplay after the menu route (600 idle Dam frames, mono): work mean
10.46 ms, p95 12.99 ms, no 60 Hz budget misses. The same route with the 3D
slider at maximum: mean 12.56 ms, p95 15.01 ms, peak 17.64 ms, 5 misses in
540 warm frames.

## Stereoscopic 3D

The gameplay renderer projects on the GPU with clip w equal to the camera
depth, so an off-axis eye is a horizontal screen shift that is affine in
depth: for far parallax P pixels and convergence D, the left eye moves a
vertex at depth W by `P * (D / W - 1)` pixels. The vertex shader adds
`stereo[1] + stereo[0] * w` to clip xy after the projection matrix
(`platform/3ds/source/vshader.v.pica`); the CPU maps the pixel shift through
the tilted orthographic screen-x column (`ge_3ds_stereo.c`). This is exactly
the projection an off-axis camera produces, so depth and rasterization are
unchanged and no second CPU projection runs.

- A right-eye render target is created at startup; each gameplay frame reads
  `osGet3DSliderState()`. Slider zero keeps the exact mono path
  (`gfxSet3D(false)`, one pass) and was verified pixel-identical.
- `renderer_draw` prepares vertex data and ticks render-owned state once, then
  runs its submission body per eye. The right eye replays the left eye's world
  draw runs (`world_run_cache`) with the same materials instead of repeating
  visibility and merging. CPU frustum culling widens its clip-x test by the
  maximum stereo shift (bounded at a third of the convergence distance).
- Layers: world/runtime geometry and first-person weapon use separate
  parallax/convergence (defaults 8 px @ 180 units, 3 px @ 45 units), HUD text
  and sight pop out 2 px, the cloud layer sits at infinity, the environment
  fill and fade overlay do not shift. `sdmc:/3ds/goldeneye-3ds/stereo.cfg`
  overrides `parallax_px`, `convergence`, `first_person_parallax_px`,
  `first_person_convergence`, `hud_parallax_px`, `cull_margin_per_px`,
  `enabled`.
- Frontend pages remain 2D (`gfxSet3D(false)` while the frontend owns the
  screen).

Verification (Azahar `factor_3d=100`, Dam spawn, 300 frames after the menu
route): right-eye disparity +15 px for distant walls, +12 px mid-distance,
−2 to −4 px for the near floor; both eyes complete after the combiner fix.
Emulator CPU cost of the second pass: renderer 2.97 → 4.98 ms mean, total
work 10.46 → 12.78 ms mean, p95 12.99 → 15.13 ms (GPU time is emulated and
not representative). Physical New 3DS XL GPU cost, comfort tuning of the
defaults and the original-3DS budget are unmeasured.

## Remaining frontend work

- Cast model tick and CPU projection (7 ms) and per-frame lit-vertex
  publication of the Nintendo logo (5.6 ms) are the largest steady costs;
  both could move to GPU transformation.
- Transition hitches above should preload during the legal screen or stream.
- Every remaining frontend/watch/result screen still needs a matched original
  comparison; only the pages on the solo Dam route were captured here.
