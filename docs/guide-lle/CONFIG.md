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
