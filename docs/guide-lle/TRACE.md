# What pressing the Xbox button does — captured trace

Verbatim from a default-configuration run, dashboard running, Guide key
pressed at ~25s. Register/vtable dumps elided.

```
Guide button: user=0 lle_xam=on hud=loaded
Guide button: dispatching open to 913E69C0
Guide button: handler returned 00000000
Guide button: obj=401EA6A0 vtable=913E1CB4
Guide button: pre-init  +8=00000000 +12=00000000 +20=00000000
Guide button: VdGlobalDevice(801E6FC4) = 40952400
Guide button: device gate [815F048C]=801D0030 [*]=00000220 bit200=set
Guide button: xam CreateDevice returned 00000000, device now 40877380
Guide button: VdGlobalXamDevice = 40877380
Guide button: queued XUI bootstrap for the title thread (hud 913E0000, obj 401EA6A0)
GuideBootstrap: xam UI thread recorded=30030010 current=30058010 (r13=3005B000)
GuideBootstrap: CHUDBkgndScene slot 81D3F924 = stub 301D6000
GuideBootstrap: render host -> 00000000, XUI ctx 4087D980, provider 81D22A54
GuideBootstrap: provider 81D22A54 vtable 81608258 [0]=817924E0 [1]=8178F600 [2]=8178F600
GuideBootstrap: registrar 817503E8 -> 00000000
GuideBootstrap: registrar 8199BE08 -> 00000000
GuideBootstrap: registrar 8176B2C8 -> 00000000
GuideBootstrap: XuiRenderCreateDC -> 00000000 dc=40896490
GuideBootstrap: [guide+4] = skin module 301B3000
GuideBootstrap: render obj 401EA6B0 vtable 913E1C8C [7]=913F0DB0 [1]=913EA990
GuideBootstrap: scene creator 913EB940 -> 00000000, scene=00010000
GuideBootstrap: after init -> 00000000  +8=00010000 +12=40899D40
GuideBootstrap: hud globals 91400168=913E16AC 91400170=913E2368 91400690=401EA6A0
GuideBootstrap: DC 40899D40 contents:
GuideBootstrap: draw hook installed on title thread
Guide composite draw #1 -> 00000000
Guide composite draw #300 -> 00000000
```

Every step returns success. Nothing appears on screen: the device these
calls target is not connected to the GPU (see SUMMARY.md).
