# Icon placeholder investigation

The user reported empty home icons and severe lag in the single-CPU run. A saved screenshot actually shows Spotlight, its keyboard, and a gray Photos suggestion tile. Iconservicesagent creates RGhA (RGBA16Float) surfaces; creation logs do not prove those surfaces contain valid pixels or that the consumer receives them. No explicit icon failure was found in the captured log.

The existing kernel supports surftrace_of=iconservicesagent with bounded surftrace=12. It records surface properties, sampled nonzero content at port lookup, and content at release. A separately named diagnostic payload was built using these flags and the two-CPU/two-worker configuration. It has not been started; the current interaction test remains intact.

Next: compare producer content with port-import content. Nonzero samples alone cannot establish correct colors, alpha, or complete icon rendering. Investigate the image producer/cache path if empty, and import/render path if populated. Keep the RAM-only test workflow.

Dual-core follow-up: user confirmed Photos and Settings icons now render and Settings opens. USB screenshot verifies the Settings list. Persistent lag remains. Logs identify a backboardd SIGABRT in CA OGL push_surface (missing transient surface), followed by compositor and SpringBoard restart. The icon-only diagnostic is deferred in favor of this concrete stability failure; no generalized icon-format fix is justified by the current evidence.
