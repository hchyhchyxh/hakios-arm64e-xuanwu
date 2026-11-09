# Compositor transient surface abort

Observed: the dual-core run reached Settings with physical input, but backboardd had earlier aborted in CA::OGL push_surface because a transient surface was absent from its list. SpringBoard restarted as a consequence. This is an assertion failure, not evidence of stack exhaustion.

Existing inject26.c supports caenv boot arguments before CoreAnimation starts and explicitly names CA_DISABLE_BACKDROP_CACHE. A separately named two-core test payload sets only CA_DISABLE_BACKDROP_CACHE=1 relative to the current configuration. This is a diagnostic hypothesis: a backdrop cache lifecycle issue has not been established, and the flag being applied does not prove the framework honors it. It is not yet a fix.

Next device gate: confirm the injected caenv line, reproduce the same unlock/Settings interactions, check for the exact push_surface assertion and compositor respawns, and compare responsiveness. Keep the normal payload unchanged; do not suppress the assertion or return success with a missing surface. Current settings interaction test remains running while awaiting feedback.

Device result: the user repeated Settings and home gestures, observed persistent lag and a black spinner on returning home. Captured logs contain the same push_surface assertion and backboardd SIGABRT. CA_DISABLE_BACKDROP_CACHE=1 was logged by the injector. This control did not prevent the failure and must not be described as a repair.

System cache inspection found CA_DISABLE_FILTER_MERGING and CA_PRINT_SURFACE_TRACKING in the current dyld cache (.01). The next isolated control removes the failed backdrop-cache flag, disables filter merging and enables surface tracking. This checks a different transient-surface construction path and gathers lifecycle evidence; string presence and flag injection alone do not prove behavior or a repair.

Stack-threshold device result: the exact assertion recurred. Symbolizing the current crash frames after removing the shared-cache slide and retained pointer tag gives QuartzCore Context::push_surface+592 (0x183f0f3a0), called from AsynchronousNode::retain_surface+1080 (0x184171fa4), then ImagingNode::render+560. Deeper frames repeat render_layers/LayerNode::apply/ImagingNode::render. The immediate missing-surface path is asynchronous; this does not prove a race or kernel defect. Full disassembly and symbolized frames are retained with the device logs.

A targeted candidate adds CA_DISABLE_ASYNCHRONOUS=1 to the stack-fixed dual-core configuration. The flag string exists in this exact iOS cache at subcache .01 offset 0x41e623f. This remains a diagnostic control. Keep assertions, verify flag injection and repeat Settings-to-home gestures before assessing it.

Asynchronous-node control: the user repeated Settings scrolling and returning home, reporting no black spinner this time, with lag unchanged. The captured summary records the injected flag and any compositor aborts. This is a promising workaround for the tested transition, not proof of long-term stability or normal usability. Preserve this configuration for performance work.
