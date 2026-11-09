# GPI translated DMA candidate

Android recovery is verified. The actual device places a00000.qcom,gpi-dma in IOMMU group 3, domain type DMA. reserved_regions reserves 0..0xfffff and 0x200000..0xffffffff, leaving the 1 MiB range 0x100000..0x1fffff for IOVA allocation. The default Android domain is translated with strict TLB invalidation.

The offline-only builder creates a 39-bit input, 4 KiB granule, three-level stage-1 page-table candidate for a bounded part of that aperture. 1004 translations, six unmapped boundary cases and three rejected invalid layouts pass. Page/table/XN/AF/shareability encodings were cross-checked with Linux io-pgtable-arm.c at commit de03b17ec046d81c4713bc54306579b9024fc513. Attribute index zero requires matching Normal-NC MAIR configuration.

The script does not write registers. Physical table/buffer addresses are illustrative inputs for host testing, not a verified allocation policy. Context ownership, TTBR/TCR/MAIR, stream-match programming and TLB synchronization are still pending. A successful software table walk does not establish that hardware DMA works.

The failed bypass path remains disabled. No physical touch has been verified. Phone remains in Android during this work.

## Runtime trace and descriptor checks

A dedicated xuanyuan_gpi tracing instance captured arm_smmu map/page-table-add events without altering the existing global tracing setup; the instance was disabled and removed afterward. No fresh a00000.qcom,gpi-dma mapping appeared in this sample. Other devices' events are not evidence of this GPI domain's context or page size. Do not infer TTBR/TCR from them. Original trace is kept on the host as android-iommu-trace.txt.

The candidate validator now checks page access flags, XN/AF/shareability, descriptor addresses and all unused leaf entries. An intentionally corrupted permission bit is rejected. Actual hardware setup is still pending, and the phone remains in Android.
