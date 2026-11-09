# Readable boot console

Use `console_zoom=3` for 24x48 pixel log cells instead of 8x16. The 3x version was RAM-booted on Xiaomi 15 Ultra and the framebuffer screenshot confirmed readable larger text. The subsequent 72-pixel horizontal margin was also verified by a device framebuffer screenshot (console-margin-screen.png).

Zoom is accepted from 1 to 4 only for 32-bit framebuffer mode. Character painting, scrolling, clearing and cursor sizing use the same cell dimensions. Native framebuffer geometry remains unchanged for the iOS graphics driver. Omit the boot argument to retain the original size.

Device screenshot and raw log are preserved in the host phone-ios26-test directory as large-console-screen.png and large-console.log. Full XNU make still has unrelated KEXT export diagnostics.
