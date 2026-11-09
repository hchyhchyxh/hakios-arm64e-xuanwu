# Xiaomi 15 Ultra bring-up record

RAM boot only. iOS lock screen rendering is device verified; physical touch is not working.

The kernel source is an isolated Apple XNU 12377.121.6 tree with the upstream HakiOS patch. Local changes are stored here as reviewable patches, not as a second full kernel import.

## Memory layout

The kmem patch adds claim diagnostics and rejects unsigned budget underflow. RAM tests use vm_compression_limit=131072 (2 GiB at 16 KiB/page). This is not a full allocator or KEXT build fix.

## Touch transport

Ultra pins and regulators differ from Xiaomi 15. Tests verified GPIO reset readback, SE0 FIFO disabled, writable GPI scratch, clock vote restoration, and absence of a pre-existing SMMU mapping for stream 0xb6. Eleven IRQ preparation registers read back correctly and were restored. Prepared event allocation succeeded with state 1; its test log remains on the host. Physical input and DMA transfers are not implemented yet. Earlier bare allocation tests failed to return and required a manual Android restart.

All hardware probes are opt-in boot arguments. Full kernel make still reports KEXT export errors; compiler/linker completion must not be described as a clean full build.

No IPSW, extracted Apple system files, phone firmware, backups, signing private keys or large build artifacts are included in these commits.
