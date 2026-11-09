# Software render scale control

The current UI renders 1125x2436 pixels and scales to the Ultra 1440x3200 panel. Existing fbscale=2 changes the compositor display to 750x1624 and updates chosen/display-scale and product/artwork-scale-factor to 2, preserving the 375x812 logical geometry. Internal pixel count becomes 4/9 of scale 3 (55.6% fewer). The panel still scans at its native resolution with bilinear scaling, so sharpness may change.

A separate four-core candidate enables fbscale=2 and touch_lat=1 while retaining the asynchronous-node workaround. It is not deployed yet. This is a performance diagnostic, not proof of normal usability or a substitute for proper graphics acceleration. Compare against the full-resolution four-core latency candidate; measure queue-to-read and HID-send separately from visual response.

Device follow-up: framebuffer and WindowServer both report 750x1624 at scale 2. Settings is visible, with no captured compositor abort. A red status bar displays rdar:45025538; the exact system diagnostic in the current UIKit/SystemStatus cache says no status-bar visual provider matches the screen width, scale, idiom and corner radius. This is a compatibility regression of the scale experiment, not a finished performance fix. Preserve scale 3 as baseline while assessing any benefit.

User feedback: scale 2 reduced lag a little. It did not establish normal usability and introduced the status-bar provider regression; it is not promoted as the final default.
