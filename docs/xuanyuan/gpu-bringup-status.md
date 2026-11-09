# Ultra GPU bring-up status

The original live device tree identifies Adreno 830 chip 0x44050001, GPU base 0x03d00000, GPUCC 0x03d90000, SMMU 0x03da0000. Streams are user/core 0, LPAC 1, secure 2 and GMU 5. These match the inherited driver resources; this alone is not proof of operational compatibility.

The first physical power/clock test passed: PLL lock, GPUCC clocks, GMU crystal counter about 19.197 MHz, staged teardown completed. Some GCC bus clocks reported off and the driver explicitly did not rely on them; retain this observation for later faults. No GPU firmware or command submission has yet been verified.

Next inputs from Android: gen80000_gmu.bin, gen80000_sqe.fw, gen80000_aqe.fw, and the gen80000_zap image (possibly split MDT segments). Record original paths, sizes and SHA-256, preserve original bytes, and inspect driver parsing requirements before constructing a candidate. Do not publish device calibration or private partition contents.

The current iOS userland injector has no accelerated-renderer loader call. Hardware power-up, firmware/SMMU, command completion, userland bridge and final interactive rendering therefore remain separate gates. Keep the async-node workaround and full render scale as the interaction baseline.

The loader accepted EfiResetCold but Android did not enumerate within 30 seconds. Host PnP still showed an unready bootloader; a manual power restart was requested. This is a recovery dependency, not a GPU-stage-one failure.
