# Reproducing the working Guide bootstrap

Everything below is a `xenia-canary.config.toml` setting. Xenia writes that
file on first run with source defaults; only these six values differ from
those defaults, and with all six set the Guide bootstrap runs end to end.

Launch is just the dashboard - the harness passes no flags:

```
xenia_canary.exe dashroot\dash.xex
```

| Setting | Value | Why |
|---|---|---|
| `lle_xam` | `"GAME:\xam.xex"` | Load the real xam instead of Xenia's HLE xam. Nothing else here matters without it. |
| `guide_hud_path` | `"GAME:\hud.xex"` | The Guide itself. Left blank, the button logs `hud=not loaded` and stops. |
| `xbox_hardware_info_flags` | `0x220` | xam gates D3D device creation on bit `0x200`. At Xenia's stock `0x20` the gate reads clear and no device is ever made. |
| `guide_create_xam_device` | `true` | Calls xam's own CreateDevice (`8178F748`) and publishes the result as `VdGlobalXamDevice`. |
| `lle_xam_heap0_alias` | `true` | xam heap[0] is left as the `0xCCCC` placeholder under our bootstrap. Allocation classes in `0x10000000` map to index 0, so without the alias the XUI render host fails with `E_OUTOFMEMORY` and never publishes a resource provider. |
| `guide_use_title_device` | `false` | This is the source default. Do not turn it on: xam asserts an out-of-range index three times, then faults, and the dashboard stops rendering. |

`guide_skin_path` stays blank on purpose - hud carries its own skin as a
resource section, and the locator resolves as
`section://301B3000,hud#strings.xus`.

## What a good run looks like

```
Guide button: device gate [815F048C]=801D0030 [*]=00000220 bit200=set
Guide button: xam CreateDevice returned 00000000, device now <ptr>
GuideBootstrap: render host -> 00000000, XUI ctx <ptr>, provider 81D22A54
GuideBootstrap: scene creator 913EB940 -> 00000000, scene=00010000
GuideBootstrap: draw hook installed on title thread
Guide composite draw #1 -> 00000000
```

Pointer values move between runs; `provider 81D22A54` and `scene=00010000`
do not. The two failure signatures worth recognising:

- `render host -> 8007000E ... provider 00000000`, then
  `scene creator ... -> 8007000E, scene=00000000` - a heap problem. Check
  `lle_xam_heap0_alias`.
- A crash at guest PC `817FBA60` with a saved LR of `817B4B98` - that is
  inside xam's heap-creation routine `817B4B70`, reached only via
  `lle_xam_heap_init`. That cvar is default off for this reason; enabling it
  is not an alternative to the alias.

Note that this reproduces the *software* path only. It does not put pixels on
screen - see NEXT.md for why, and for what is still unknown.

## The deepest-reach configuration

The six settings above are the *stable* setup: the Guide's software runs, the
scene is created, and the dashboard keeps rendering. Everything below is a
second, less stable configuration that gets the present to complete. Keep them
separate - this one stops the dashboard after one frame.

On top of the six above, all default off:

| Setting | Why |
|---|---|
| `guide_create_primary_device` | Call xam's mode-1 device creator `8178E9F0` instead of `8178F748`. Mode 1 owns the GPU and binds a render target; mode 2 does neither. |
| `guide_bootstrap_before_device` | Queue the bootstrap before that call. The mode-1 creator never returns, so in the normal order the queue call is dead code and the Guide never starts at all. |
| `guide_use_bound_device` | Point the present path at the device that has a render target. Under mode 1 two xam devices exist and only one is bound. Applied per frame to `[wrapper+12]`, because doing it once at bootstrap loses a race. |
| `guide_clear_null_render` | Clear `[xui_ctx+1C]` so the DC is built with `[dc+134] = 0`. Otherwise `XuiRenderPresent` returns S_OK without presenting. |
| `guide_fake_front_buffer` | Clone the bound colour surface into `[device+3F74]`. Nothing ever allocates a front buffer for xam's device, and the present dereferences it. |
| `guide_syscmdbuf_fields` | Fill `p0+0x30 = 0x500` and `p0+0x34 = 0x5BE` in `VdGetSystemCommandBuffer`'s descriptor - the display mode the guest checks for. |

### What it looks like

```
GuideBootstrap: render host -> 00000000, XUI ctx <ptr>, provider 81D22A54
GuideBootstrap: scene creator 913EB940 -> 00000000, scene=00010000
Guide: front buffer [dev <ptr> +3F74] = clone <ptr> of RT0 <ptr>
Guide composite draw #1 -> 00000000; draw dc=<ptr> [11C]=00000000
                           [134]=00000000 [1CC]=<ptr>
```

`[134]=00000000` is the part that matters: Present did not short-circuit. Zero
guest crashes.

### What it does not do

- **No pixels.** A capture is byte-identical to `work/noguideshot.ps1`.
- **Exactly one draw.** The title thread stops afterwards, so the dashboard
  freezes. This is not a configuration to leave enabled.

Three of these six - `use_bound_device`, `fake_front_buffer`,
`syscmdbuf_fields` - hand the guest something the system would normally have
built for itself. They are probes that establish what is missing, not fixes.
The real gap is in PRESENT.md: `VdGetSystemCommandBuffer` returns a descriptor
with no command buffer in it, and `VdSwap` throws away the one the guest hands
back.

## Regression check after the tooling and fix work

Verified on a clean tree, after the `KeDebugMonitorData`/`KeCertMonitorData`
fixes and the diagnostic cvars were added:

- clean rebuild: **936/936 targets, 186s, no errors or warnings**
- stable configuration reproduces its documented signature exactly:

```
GuideBootstrap: render host -> 00000000, XUI ctx <ptr>, provider 81D22A54
GuideBootstrap: scene creator 913EB940 -> 00000000, scene=00010000
GuideBootstrap: draw hook installed on title thread
Guide composite draw #1 -> 00000000; ... [134]=00000001 ...
```

- guest crashes: **0**
- dashboard framebuffer: **2EF6B4B7**, the same hash as the no-press control
  and as every earlier capture

Every diagnostic cvar added during this work defaults off, so a fresh config
gets the stable behaviour. The list, all `false`/`0` by default:
`guide_stall_probe_seconds`, `guide_fake_gpu_writeback`,
`guide_force_cmdbuf_complete`, `guide_watch_front_buffer`,
`guide_watch_null_render`, `guide_diff_draw_writes`, `guide_word_diff`,
`guide_execute_command_stream`, `guide_syscmdbuf_buffer_kb`,
`guide_trace_setrendertarget`, `guide_call_boot_entry`,
`guide_bind_depth_copy`.

## Correction: five values, not six

The table at the top lists six settings, but one of them -
`guide_use_title_device = false` - **is now the source default**, changed early
in this work precisely because defaulting it on was dangerous. It does not need
setting; it needs leaving alone.

Comparing the working config against every source default programmatically, the
values that genuinely differ are five:

```
lle_xam                  = "GAME:\xam.xex"
guide_hud_path           = "GAME:\hud.xex"
xbox_hardware_info_flags = 0x220
guide_create_xam_device  = true
lle_xam_heap0_alias      = true
```

Nothing else in the config differs from its default. `guide_message` shows up in
a naive comparison because the file stores `2147483652` where the source writes
`0x80000004` - the same number.

So: **set those five, leave everything else alone.** The entry for
`guide_use_title_device` stays in the table above as a warning, not as an
instruction.

Also verified in the same pass: no cvar on this branch is defined but unused.
`guide_xui_anim_init` was the only one, and it is now wired up and defaulted to
0.

## Verified from scratch

The reconstruction procedure in this file has now been tested the hard way:

1. Deleted `xenia-canary.config.toml` entirely.
2. Ran once to let Xenia write a fresh file from source defaults (85,268
   bytes).
3. Set exactly the five values listed above - nothing else.
4. Ran.

Result:

```
GuideBootstrap: render host -> 00000000, XUI ctx 4088A0A0, provider 81D22A54
GuideBootstrap: scene creator 913EB940 -> 00000000, scene=00010000
GuideBootstrap: draw hook installed on title thread
Guide composite draw #1 -> 00000000; ... [134]=00000001 ...
```

Zero guest crashes, signature identical to the one documented at the top.

That is the check this file exists for. The original configuration was lost
once because it existed only in an untracked TOML, and reconstructing it took a
session and turned up `lle_xam_heap0_alias`, which no document mentioned. This
procedure now demonstrably works from nothing but the five lines above.

The config in the tree is that freshly generated one, so it carries every cvar
this branch added at its default rather than whatever state a long series of
experiments left behind.

## The deep configuration is smaller than documented

The deep-reach section above lists six cvars. Testing each by removal:

| cvar | removed | needed? |
|---|---|---|
| `guide_syscmdbuf_fields` | emitter still reached, no crash, draw returns | **no** |
| `guide_fake_front_buffer` | crash at `819F5EC4`, null descriptor | yes |
| `guide_use_bound_device` | crash at `819DE94C`, null render target, emitter never reached | yes |
| `guide_create_primary_device` | (gates mode 1 itself) | yes |
| `guide_bootstrap_before_device` | (bootstrap never queued under mode 1) | yes |
| `guide_clear_null_render` | (the flag being cleared is the whole point) | yes |

So five, not six. `guide_syscmdbuf_fields` is redundant: it was necessary
before `guide_use_bound_device` existed - filling the display-mode fields was
what let the present complete back then - and pointing the DC at the bound
device now achieves the same thing properly. An earlier section describes those
fields as load-bearing; that was true when written and is not any more.

### What is actually fabricated

Of the five, only two hand the guest invented state:

- `guide_use_bound_device` - repoints the DC at a device that already exists.
  A redirect, not an invention.
- `guide_fake_front_buffer` - **a clone of the colour surface standing in for a
  front buffer nothing allocates.** This is the one genuinely made-up object.

The other three are ordering and a flag patch. So the earlier warning that deep
measurements rest on "three pieces of hand-fabricated state" overstates it:
there is one invented object and one redirect. That is still enough to make
"the emitter emits zero draws" a statement about this configuration rather than
about xam, but the surface is smaller and better understood than it was.
