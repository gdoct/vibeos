# TCP receive-path fuzzer

Host-side, coverage-guided fuzzing of the kernel TCP stack's receive path
(`tcp_input()` and everything it fans into), built under ASan + UBSan + libFuzzer.
No QEMU required — the stack is compiled and driven directly on the host.


# Result
~2.99 million segments, ~11.7k exec/s, 256s. Zero memory errors, zero UB, zero crashes, zero genuine hangs. Per-function coverage of the receive path:

| function | line | branch |
|----------|------|--------|
| tcp_input | 78% | 69% |
| tcp_recv_data | 100% | 90% |
| tcp_recv_ack | 100% | 88% |
| tcp_reassemble | 100% | 100% |
| tcp_ooo_store | 100% | 93% |
| tcp_parse_mss | 100% | 100% |
| tcp_rx_append | 100% | 75% |
| snd_copyout | 100% | 100% |
| tcp_output | 80% | 59% |
| tcp_timer | 80% | 64% |
| tcp_enter_timewait | 0% | 0% |

The signed-sequence comparisons (seq_lt etc.) hold up — the overlap/skip branches in tcp_recv_data/tcp_reassemble are correctly bounded, which is exactly where hand-rolled stacks usually get OOB writes. The parser (tcp_parse_mss) and the option/doff bounds checks are fully exercised and clean.
## Findings (tl;dr)

Real (minor): no TCP checksum verification on receive. tcp_input accepts any segment matching the 4-tuple; IP/ICMP/UDP and TCP-send all compute checksums, only TCP-receive skips it. Harmless over QEMU SLIRP/loopback (lossless), but a bit-flipped segment on a real NIC would be accepted as data. Not a memory-safety issue.

Limitations (what this did not test)

Input path only. No app-side close(), so active-close → FIN_WAIT → TIME_WAIT is unexercised (hence tcp_enter_timewait at 0%). That's the obvious next target.
No live concurrency test. This sandbox has no qemu/dotnet/mtools, so I couldn't boot it and hammer concurrent connections — that's where SMP/locking races in the worker would show, which static fuzzing can't reach.
Single-connection model; multi-PCB interactions and the socket/loopback path are lightly covered.

Net: the receive path is memory-safe and hang-free under ~3M adversarial segments, with one real-but-minor robustness gap (rx checksum). 
## How it works

`tcpfuzz.cc` `#include`s `../kernel/src/net.c` directly, so it can call the
`static` functions and seed the `static` PCB table. The only kernel
dependencies are satisfied by tiny stubs in `stub/`:

| stub header   | replaces                | behaviour                                  |
|---------------|-------------------------|--------------------------------------------|
| `kernel.h`    | `kmemcpy/kmemset/kprintf` | libc `memcpy/memset`; `kprintf` is a no-op |
| `task.h`      | scheduler + wait queues | locks/wakes are no-ops; **sleep advances the clock** (mirrors the timer ISR, so bounded waits like `arp_resolve` terminate) |
| `timer.h`     | `timer_ticks()`         | reads `g_now_ticks`                         |
| `csprng.h`    | `csprng_tcp_isn()`      | deterministic (so the fuzzer can complete a handshake) |
| `net.h`       | —                       | forwards to the real `kernel/include/net.h` (no copy, no drift) |

Each iteration resets all stack state, seeds one ESTABLISHED connection with
data in flight plus a listener, then feeds up to 48 attacker-controlled
segments (4-tuple, seq/ack offered both absolute and relative to connection
state, flags, window, options, payload) into `tcp_input()`, periodically
advancing the clock and running `tcp_timer()`.

## Usage

Requires `clang`/`clang++` (any recent version with libFuzzer; tested with 18)
and, for `make cov`, `llvm-profdata`/`llvm-cov`.

```sh
make -C fuzz             # build the fuzzer
make -C fuzz run         # 60s campaign (override: make -C fuzz run TIME=600)
make -C fuzz cov         # per-function coverage over the current corpus
make -C fuzz dbg         # standalone replay build for triaging a saved input
```

A crashing or hanging input is written to `fuzz/artifacts/`. To triage a hang:

```sh
make -C fuzz dbg
./fuzz/tcpdbg fuzz/artifacts/timeout-XXXXXXXX     # prints a backtrace after 3s
```

## CI (GitHub Actions)

```yaml
# .github/workflows/fuzz.yml
name: tcp-fuzz
on: [push, pull_request]
jobs:
  fuzz:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get update && sudo apt-get install -y clang llvm
      - run: make -C fuzz run TIME=120
      - uses: actions/upload-artifact@v4   # keep any crash for download
        if: failure()
        with: { name: tcp-fuzz-artifacts, path: fuzz/artifacts/ }
```

The job fails if libFuzzer finds a crash, a sanitizer error, or a unit slower
than `-timeout=10`s. Commit the `corpus/` you accumulate locally to make CI runs
warm-start and reach deep states fast.

## Known gaps (what this does NOT cover)

- **Input path only.** No app-side `close()`, so the active-close →
  `FIN_WAIT_*` → `TIME_WAIT` path is not exercised (`tcp_enter_timewait` shows
  0% coverage). Driving `tcp_close()` from the harness is the obvious extension.
- **No concurrency.** Single-threaded; this cannot find SMP/locking races in the
  net worker — that needs a live multi-core boot.
- The receive path does **not verify the TCP segment checksum** (send side does).
  Lossless over SLIRP/loopback; a corrupted segment on a real NIC is accepted.
