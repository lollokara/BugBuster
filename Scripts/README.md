# Scripts

Utility scripts and Rust test sources for BugBuster hardware validation.

## Python scripts

| File | Purpose |
|------|---------|
| `test_hat_verify.py` | Connects to the BugBuster HAT over USB-serial and verifies firmware version, GPIO loopback, and SPI roundtrip. Run after flashing RP2040 firmware. |
| `test_spi_clock_sweep.py` | Sweeps SPI clock frequencies on the HAT and measures throughput / error rate. Used for hardware bring-up and regression testing after PCB revisions. |

## Rust test sources

| File | Purpose |
|------|---------|
| `test_dedup.rs` | Unit-level test for the deduplication logic in the LA capture pipeline. Compile with `rustc test_dedup.rs -o /tmp/test_dedup && /tmp/test_dedup`. |
| `test_la_bug.rs` | Reproducer for an early LA streaming bug (fixed). Kept as a regression reference. |
| `test_la_bug2.rs` | Reproducer for a second LA streaming bug variant (fixed). Kept as a regression reference. |
| `test_empty_bug.rs` | Reproducer for an empty-buffer edge-case in the LA capture path (fixed). Kept as a regression reference. |

## Note

The compiled binaries `test_dedup`, `test_la_bug`, and `test_la_bug2` have been removed from the repository (macOS arm64 local builds, not portable). Rebuild from the `.rs` sources when needed.
