# ResourceRateMod.ini schema (R14)
# Edit while the game is NOT running, or flip toggles with the F9 overlay panel.
# Missing/partial/garbage values fall back to these defaults.

[General]
Enabled = 1                  ; int 0/1 — master switch for the overlay + rate engine
Hotkey = 0x78                ; int — virtual-key code; 0x78 = F9 (F10=0x79, F8=0x77)
StartHidden = 0              ; int 0/1 — start with the panel hidden (F9 to show)

[Display]
FontName = Georgia           ; empty = default serif (no custom font registered = game font style)
FontSize = 14                ; int 8..40 — panel row height scales with this
Opacity = 0.85               ; float 0..1 — backdrop + text alpha
PosX = 12                    ; int — panel top-left X
PosY = 12                    ; int — panel top-left Y
ShowHeader = 1               ; int 0/1
ShowSlots567 = 0             ; int 0/1 — show unknown slots 3..6 when nonzero

[Rate]
SampleMs = 500               ; int 100..1000 — ring-sample interval (game-time ms)
Smoothing = med              ; low|med|high — EMA alpha 0.1 / 0.2 / 0.4
Unit = min                   ; min|sec — display "+x.x" per minute or per second
UseGameTime = 1              ; int 0/1 — clock source; 1 = game tick (1 tick = 1 ms),
                             ;            0 = realtime (QueryPerformanceCounter)
DiscontinuityRatio = 3.0     ; float — spending spike: skip sample when the instant
                             ;           negative rate exceeds EMA * this ratio
ShowGains = 1                ; int 0/1 — positive jumps (instant gains) counted

[Debug]
Enabled = 0                  ; int 0/1 — extended diagnostics into d3d9mod.log:
                             ; version gate, structures, values, rates, UI init, hooks