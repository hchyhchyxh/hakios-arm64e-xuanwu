# Same-board SMMU source audit

MiCode Xiaomi_Kernel_OpenSource branch xuanyuan-v-oss contains arm-smmu.c, arm-smmu.h and arm-smmu-qcom.c. Download hashes and source URLs are recorded separately; branch contents can change.

arm_smmu_write_context_bank writes CBA2R/CBAR, then TCR2/TCR before TTBR0/TTBR1, then MAIR0/MAIR1, and enables SCTLR last. AArch64 context setup includes TCR2.AS and SEP_UPSTREAM. Stage-1 CBAR carries BPSHCFG_NSH and S1_MEMATTR_WB in addition to type. The previous experimental context setup did not reproduce this complete configuration and is still disabled.

qcom_def_domain_type states that the hypervisor treats identity domains as blocked and disallows S2CR.TYPE=1. This is relevant platform evidence, but it does not prove the exact cause of the earlier implementation-specific abort (that test routed through a context bank).

The new offline register-plan test rejects unproven bank ownership, misaligned/out-of-range TTBRs and unsupported ASIDs. It checks ordering and leaves SCTLR enable out. No context is declared available merely because no matching stream was found. Device integration still requires ownership/capability evidence and TLB synchronization. Physical touch remains unverified; phone stays in Android.

## Bounded context synchronization candidate

The host-only `tests/xuanyuan-smmu-sync.h` follows the vendor context invalidation sequence: write barrier, TLBIASID (0x610), barrier, TLBSYNC (0x7f0), poll TLBSTATUS (0x7f4), final completion barrier. The Xiaomi source defines QCOM_DUMMY_VAL as -1, so TLBSYNC receives UINT32_MAX. The fake backend asserts both offsets and values.

Polling uses exponential delays capped at 128 us and a caller-provided total budget. Timeout latches the shared SMMU state and rejects later synchronization attempts without register writes. The caller must serialize access and establish ownership. The helper has no physical MMIO backend and is not integrated into the phone driver.

Validation: `gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined docs/xuanyuan/tests/test-smmu-sync.c -o /tmp/test-smmu-sync && /tmp/test-smmu-sync`. Result: PASS: invalidation/sync order; completion; bounded timeout; post-timeout writes blocked; invalid inputs rejected. Exit 0. Installed Clang sanitizer runtimes were absent; GCC sanitizers completed successfully.

The prior device mapping path remains disabled. No successful hardware mapping, TLB synchronization, or physical touch result is claimed by this host test.

## Live handoff rule and removal of failed writes

Read the running phone's apps-smmu qcom,handoff-smrs property using root ADB, without changing device state. Its eight bytes are `00 00 08 00 00 00 00 02`: stream ID 0x800, mask 0x2. GPI stream 0xb6 does not match. This says nothing about whether an unreferenced context is available to this loader.

The incremental `patches/xnu-smmu-remove-failed-route.patch` applies after the prior probe patches. It deletes the entire failed temporary route write block, including its attempted restore sequence, and logs valid SMR/S2CR routes in the read-only probe. The formerly named candidate bank is now explicitly an inspected bank; it is not offered for allocation. The patch reverse-checks against the edited source.

The edited kernel compiled and linked; overall make still exits 2 with 268 pre-existing KEXT export diagnostics. The 45 copyio relocations were reconstructed. QEMU checkpoint 67 printed `checkpoint 67 0` and exited 0. QEMU does not exercise the physical SMMU. No new phone RAM boot or partition write occurred in this change. Physical touch remains unresolved.

The timeout latch is shared across all contexts of one SMMU, matching the vendor driver device-wide gate. A second fake context is checked after the first times out; no reads, writes, or barriers are issued. This state must be shared by any future hardware backend.

## Device capability and trapped zero-store correction

The RAM probe returned IDR2=0x5111. Decoding through the vendor size enumeration gives 36-bit UBS, IAS and OAS, with a 4 KiB granule supported. Stage-1 setup therefore uses input_bits=36 and output_bits=36, not the illustrative 39/40-bit host defaults used previously. TCR.T0SZ becomes 28 and TCR2.PASIZE becomes 1. The table builder now rejects table/buffer ranges beyond 36-bit physical addresses by default.

The vendor driver documents a hypervisor emulation bug for stores whose source is WZR/XZR. A plain volatile zero store compiles to `str wzr, [x0]`; the explicit inline-assembly register operand compiles to `mov w8, wzr; str w8, [x0]`. This is relevant evidence for the prior implementation-specific abort, not proof of its precise cause. The new helper has not yet performed a device write.

The new read-only context probe reports routes for bank 0..4 and no match for GPI stream 0xb6. Banks 5..7 return uninitialized-looking context values and CBAR/CBA2R zero. Those values alone do not establish ownership. The initial short run returned before driver initialization; a 90-second run reached the probe and printed the Darwin hello. Its PID 1 intentionally exits, producing the expected initproc-exited diagnostic before return to the loader. This is not a full iOS boot test.

The explicit-register helper is now integrated into an opt-in CBA2R-only device probe. On bank 5, no active stream references were present, CBAR/CBA2R were zero, and SCTLR.M was clear. Writing 1 read back 1; restoring 0 read back 0. Darwin hello ran and the loader returned normally. No stream routing, context enable, or DMA start was performed. This verifies these two accesses, not full translated DMA.

## Stage-1 device test and cleanup failure

The device table-writing loop was extracted, compiled with GCC ASan/UBSan, and compared byte-for-byte against the independent Python table builder; all descriptors and four guard words on either side matched. In the real phone, bank 5 accepted TCR=0x80301c, TTBR0=0x00050000c77f4000 and MAIR0=0x44. TCR2 readback includes additional 0x60 bits; the comparison excludes these observed bits and still checks the programmed fields.

Context TLB invalidation and synchronization completed after one 1-us poll. The route read back SMR=0x800000b6, S2CR=5, SCTLR=7. Removing the route then triggered an implementation-specific abort before the cleanup marker. Therefore route activation alone is not recorded as a successful complete test, and the original context was not verified restored. DMA was never started. The phone returned to the loader and remains accessible for a separate cleanup test. Zero-register avoidance did not by itself solve the cleanup failure.

The separate FAULT cleanup test succeeded, followed by a complete create/remove round trip: TLB sync completed, route 0xb6->5 read back, S2CR changed to FAULT (0x20000), SMR was invalidated and bank 5 disabled. Both completion checks printed 1; Darwin hello ran without an implementation-specific abort. The original unused S2CR value zero was deliberately not restored: it requests translation through another owner's bank 0. The inactive entry is now explicitly faulting. This fixes route cleanup but does not yet validate DMA or touch.

## Empty-channel investigation

The exiting hello ramdisk ended the first run before a START result. A persistent PID 1 diagnostic avoids this premature exit. The persistent and paired-start runs did not restore USB response, so neither is a lifecycle pass. A control run without GPI writes is required to separate the diagnostic-program path from channel-start behavior. The vendor configures both channels before starting either; this ordering is now implemented. No transfer doorbell has been sent.

The no-GPI persistent PID 1 control returned after loader_return_after expired. Pair-first configuration, GENI DMA/GSI mode and masking the verified INTID 312 have not produced a completed channel test. Each lost USB; this does not establish an interrupt-storm cause. Source startup and timeout behavior will be checked with the loader on a different core before another channel start. Android IPC logging was restored to level 2 and the SPI runtime policy to auto after reference capture.

## Latest transport limits

Two chained CONFIG descriptors were consumed with RP/WP=0x100020 and a completion event pointing to 0x100010, code 4 (EOB). Repeated doorbells after a standalone EOB stalled despite descriptor prefill, explicit PoC maintenance and another START command. No SPI data has been verified. Later power/reset/GO candidate runs automatically rebooted. High-frequency logs showed initialization farther along than the older three-second snapshots; early snapshot endpoints cannot identify the faulting instruction. GPIO162 INTR_CFG=0x742 already had delivery disabled. Masking it did not prevent reboot. Skipping touch reset and then RPMh requests also did not establish a successful transfer. Latest hypotheses remain unproven. Stable empty-channel and CONFIG-only results must not be described as a working touchscreen.
