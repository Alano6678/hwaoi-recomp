# 2D layer canvas above 1080p (patch set hwaoi_ui_canvas)

## How the game shows its 2D layer

The game (KT "kids" engine, screen layouts) lays the 2D layer out on a 1920x1080 canvas, in coordinates centred on
the screen (quads of -960..960 x -540..540). Three kinds of draws reach the window image:

1. **Perspective canvas** (frames, panels, menus, title/splash logos, movies' quads, HUD frames, minimap outline,
   map image): a screen camera with fov 45 deg, near 0.1, far 20000, standing at `size.h/2 / tan(22.5 deg)` from the
   canvas. In the UBO (vertex stage, binding 0) the combined matrix row `0 0 -1.00001 -1 | 0 0 d' d` shows `d`:
   1303.68 at 1080p, 1738.23 at 1440p, 869.1 at 720p. With `size` = output the canvas covers 1080/output_h of the
   screen: centred 0.75 at 1440p, cropped 1.5x at 720p (the Map screen at 720p is zoomed and loses its frame).
2. **Text** (glyph quads built on the CPU, vertices in output pixels): anchor * (output / layer size) + output/2.
   With layer size = output they sit 1:1 in a centred 1920x1080 box above 1080p.
3. **2D primitives** (map icons/diamonds/tower/green square, minimap marker and enemy dots): quads in canvas units
   from the top-left, drawn with the pixel ortho of the target. Above 1080p they shrink towards the upper left (the
   minimap ones fall outside the minimap and are clipped away); at 720p they spread to the lower right. The UBO rows
   `1.2 0 0 tx | 0 1 0 ty` seen next to these draws are stale contents of the constant ring; the vertex shader of these
   draws (sh 0e099825d5f3bbf0) only reads the ortho vec4: `pos = v.xy * c3[0].xy + c3[0].zw`.

The pixel ortho of every 2D draw is computed on the render thread from the bound target's size:
`(2*s/W, -2*t/H, -1, 1)` with s, t at ctx+0x28f4/0x28f8 (1.0 observed), in six copies at main+7194c8, 71cea4,
71de6c, 71ede0, 71f400, 726090. Forcing it to 2/1920 for the window target (host experiment) fixes the icons but
moves the text (output pixels) - the two use different spaces, so it is not a fix by itself.

## The patch

The layer setup at main+576f80 reads the layer's screen size from its config (`ldr x25, [x8]` at 576ff8, x8 =
config+0x88; 1920x1080 when absent), derives the perspective camera (inlined at 5770d0..57712c, tanf at 57711c),
stores the size at layer+0x690 and passes it to main+add610(camera, mode, size) for its cameras. hwaoi_ui_canvas
loads {1920,1080} from patch page +0xa8 there instead, and in add610's perspective path (add844/add874) for its
other callers. Result: the perspective canvas fills the output at every size and text scales with it.
HWAOI_UI_CANVAS=0 (TOTK_UI_CANVAS) restores the original code.

## The 2D primitive recorder (fixed)

Widgets draw sprites through main+15f2ca4(widget, const f32 corners[8], uv, color): it calls
`widget+0x28 -> vtable[1]` = main+af38c0, a method of the game's 2D primitive recorder (vtable at main+275fb28;
methods af3788..af42d8 record typed 0x60-byte items into batch = renderer+8: 0 quad, 3/5/7 state, ...). af38c0
converts the corners with main+af42dc and appends a type-0 item. af42dc copies the corners unchanged when the batch
has a target (batch+0x428, always set here: both batches seen, the Map and the battle HUD/minimap one); the other
branch maps them through batch+0xf0 (s16 screen size) and the anchor at +0x10c. Over a whole battle-slow2 replay at
1440p the only geometry method called was af38c0 (via 15f2ca4); the rest were state methods (af4074, af414c).

hwaoi_ui_canvas replaces the copy at af42e4..af42f0 by: `ldp q0,q1,[x2]; adrp x9,page; ldr s2,[x9,#0xb8];
fmul v0.4s/v1.4s by v2.s[0]; stp q0,q1,[x1]` (the original ldp/stp pairs become nops). Page +0xb8 = f32
output_w / 1920 (res_init). The corners reach the vertex buffer in output pixels, consistent with text.

### The recorder's other shapes (2026-11-23, Age of Decomp; not yet seen in a battle)

A report from 2560x1440: the enemies' dots are missing in the zoomed minimap and there in the whole-map view. The
recorder's geometry methods, read in the generated C (items of 0x60 bytes in batch = renderer+8):

| Method | Item | Positions | Above 1080p |
|---|---|---|---|
| main+af38c0, af3ea4 | 0: quad, four corners | through main+af42dc | scaled (the sites above) |
| main+af3788 (one colour), af3d7c (item 6) | quad from a rectangle x0 y0 x1 y1 | through main+af43d0 | was not scaled |
| main+af3b24 | 1: line, two points, colour, width (s0..s4) | stored as given for a batch with a target | was not scaled |
| main+af39f0 | 2: triangle, three points | stored as given, no branch on the target | not scaled, left alone |

main+af43d0, for a batch with a target, copies the rectangle's four numbers into four corners with eight `ldr s0` /
`str s0` pairs (af43d8..af4414). hwaoi_ui_canvas now follows each load with `fmul s0, s0, s2`, s2 = page +0xb8 loaded
before the first one (x9 and s2 are free there: both callers load them again after the call). main+af3b24 gets its
five kept arguments s12..s8 multiplied at af3b74, the first instruction of the path for a batch with a target (the
width too). The world map and the training field's HUD at 2560x1440 look the same with and without the nine sites.
If the zoomed minimap still lacks something, the triangles are next. resolution.c also writes the canvas (1920x1080)
and a scale of 1.0 to the page on the game's own 3D path (`RENDER_SCALE=game`), where the page was left at zero.

How it was found: hardware write breakpoints (perf_event_open PERF_TYPE_BREAKPOINT, one event per thread, signal
handler logs host rip and the guest frame-pointer chain via kern_cpu()) on the corner values of one icon found in
memory (scan of rw mappings for the vertex buffer's first x,y), which pointed at main+15f2ca4; then a temporary hook in
rt_dispatch logging indirect calls by return address (c->x[30]) gave the target af38c0 and the batch fields.
(PROT_NONE + single-step watchpoints are too slow on the hot frame-allocator pages.)

### Earlier findings (kept for reference)

- The scale-policy sites of hwaoi_display (01432eb4...) are dynamic resolution, not UI. The 1920/1080 float pairs near
  1e9xxxx are DRS UV clamps. add610 mode 1 (ortho camera) forced to 1920x1080 changes nothing visible.
- Forcing the window draws' ortho to 2/1920 fixes the primitives but breaks text (output pixels): one producer has to
  change, which is what the af42dc patch does.

## Method

Temporary probe in nvn_api_cmd.c: on draws into the window image (SetRenderTargets' color 0 one of the window's textures)
log the guest frame-pointer chain, the first 256 bytes of each bound UBO and vertex buffer 0; a host-side rewrite of
the UBO's distance/ortho confirmed each hypothesis before patching. A scan of rw mappings for the camera distance
found the camera objects (near 0.1, far 20000, fov 0.785398), whose constants led to the 577xxx setup.

## Host experiment: origin-scaling instanced window draws (reverted)

With the canvas patch on, draws into the window image with more than one instance were given a viewport and scissor
scaled about the TOP-LEFT by pass_w / 1920 (record_fixed), to test the idea that the icons are instanced sprites in
canvas units. Window captures of the Map screen and the minimap at 3840x2160 against 1920x1080:
- The tower glyph on the Map screen landed exactly where it is at 1080p.
- The orange diamond markers and the green square still land at about 0.5-0.8 of their 1080p positions (up-left), so they
  are not drawn through the same instanced path (or are scaled by a second factor).
- The minimap objective marker and the orange enemy dots were not visible inside the minimap at 3840x2160 with or without
  the experiment; at 1080p they are. So at least two icon classes exist and the host cannot tell them apart by instance
  count. The fix has to be at the game-thread 2D batcher that writes the icon rows (see the notes above), not in the
  window pass. Capture method used: a temporary swapchain capture in present_image (menu_shot path), every 150th frame,
  with battle-slow2.txt (the Map screen appears when the replay loses sync at 4K).
