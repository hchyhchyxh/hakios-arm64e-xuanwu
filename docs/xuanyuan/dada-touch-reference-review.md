# Xiaomi 15 dada touch reference review

## Verified baseline

The local origin is https://github.com/superboyyy/hakios-arm64e-dada. The imported reference is commit f1ae906. Remote HEAD verification is still pending; no claim is made that this snapshot is latest.

The upstream kernel patch explicitly states that dada serial engine 2 has its FIFO disabled and uses GPIO bitbang until GPI DMA is implemented. Its working fallback calls bitbang_init() and tcm_probe(). The upstream does not provide a working GPI SPI transport to copy.

| Item | dada reference | Ultra current adaptation |
| --- | --- | --- |
| Serial engine | SE2, 0xa88000 | SE0, 0xa80000 |
| SPI GPIOs | 40/41/42/43 | 32/33/34/35 |
| Reset / interrupt | 161 / 162 | 161 / 162 |
| Fallback | GPIO bitbang, SPI mode 0 | Prior identification reads returned zero; cause unresolved |
| Transport work | FIFO attempt then bitbang | Experimental GPI diagnostics, GO execution unresolved |

## Changed investigation strategy

Pause repeated GO boots. Reuse the upstream bitbang path and compare native Ultra pin ownership, power ordering, reset and SPI sampling with dada before the next device test. Verify changes offline, then run a bounded identification test with clear acceptance criteria: a valid Synaptics TCM header and identity, rather than merely a successful GPIO write.

Android recovery was verified after the stock SPI unbind experiment: driver spi_geni bound, power policy auto, GPI logging level 2, sys.boot_completed=1. The unbind experiment disconnected USB and did not complete its capture; do not repeat it as a register acquisition method.

Physical touch and normal interactive iOS use remain unverified. Existing GUI milestones are separate from transport completion.
