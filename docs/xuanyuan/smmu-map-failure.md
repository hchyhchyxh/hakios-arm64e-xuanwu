# Temporary SMMU route failure

The read-only probe found no stream 0xb6 match and no active stream references to bank 82. An opt-in temporary route test read back SMR 0x800000b6 and S2CR 0x52, then the device raised an implementation-specific abort before restoration was logged. This is a failed test, not proof of working DMA translation or successful rollback.

No DMA channel was started. The write path is now disabled unconditionally in source. A loader reboot was requested to reset hardware state. Future work must inspect the SoC/hypervisor context ownership and implement valid translated mappings rather than enabling this route again.

Full XNU make retains unrelated KEXT export diagnostics. QEMU checkpoint 67 regression passes with console_zoom=3, but does not exercise the physical SMMU.
