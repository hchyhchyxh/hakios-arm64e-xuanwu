# Framebuffer console handoff

The MOVE-coalescing device run reached SpringBoard and reported animated windows at 33–34 FPS, with substantial pauses. USB framebuffer screenshots still contained continuously updated text. This does not establish usable UI performance.

Inspection found that kPEGraphicsMode changes console mode flags without stopping gc_enabled. The CPU drawing path called that operation alone; the scanout path only set fGraphics. Both paths now also request kPEDisableScreen, whose ARM implementation pauses graphical console drawing and switches to the serial console. USB logging and physical UI behavior require device verification.

The small font preference remains unchanged. The bounded patch is separate from touch coalescing. Build and on-device validation are pending.

Device follow-up: USB screenshot now shows an unobscured iOS lockscreen with the bottom swipe bar. USB logs continue to update. Physical unlock, Settings and lag comparison are pending; the graphical handoff alone does not establish normal usability. Compiler/linker output was accepted, but the overall build still reports 268 KEXT export diagnostics. The kernel copyio relocation repair completed.

Offline follow-up: the payload builder now accepts explicit --cpus (1 or 2), --fb-threads (1..3), and --output. Defaults remain the existing single-CPU/three-scaler configuration for a reproducible baseline. A separately named two-CPU/two-scaler candidate was built and its embedded arguments checked; it has not been booted. Unsupported CPU counts are rejected. No SCMI clock or GPU setting is changed.
