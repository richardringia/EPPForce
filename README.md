# EPPForce

A small macOS kext that sets the Intel HWP **Energy Performance Preference (EPP)** to `0x10`
on every logical CPU after boot and after every wake. It has no userspace interface.

On some Hackintosh CPUs, macOS (XCPM) writes `IA32_HWP_REQUEST` = `0x8000ff01` on every core,
which is EPP `0x80` ("balanced"). Once the package hits its power limit, the CPU then cuts P-core
clocks hard, while Linux, which uses EPP `0x10`, keeps them at full speed.
CPUFriend's EPP value is ignored by XCPM on these CPUs, and VoltageShift can write the MSR but lets
any local user read and write MSRs. EPPForce does the one write from the kernel and exposes only
read-only status properties.

Tested on an Intel Core Ultra 7 270K Plus (Arrow Lake, 8P + 16E), macOS 26.7.1 Tahoe, OpenCore 1.0.9, SMBIOS MacPro7,1.

| GB7 multi-core phase | EPP 0x80 (stock) | EPP 0x10 (EPPForce) |
|---|---|---|
| P-core clock (median) | ~4400 MHz | 5336 MHz |
| Package power (median / max) | ~201 W | 226 W / 250 W |

## What it writes

`0x1000ff01` → `IA32_HWP_REQUEST` (MSR `0x774`), and `IA32_HWP_REQUEST_PKG` (`0x772`) if
CPUID.06H:EAX[11] is set. Fields: min perf `0x01`, max perf `0xff`, desired `0x00`, EPP `0x10`.
The value is a compile-time constant in `src/EPPForce.cpp` (`kEPP_VALUE`).

## When it writes

HWP MSRs raise #GP while HWP is not enabled, and a fault inside an interrupts-off rendezvous
freezes the machine without a panic screen. The kext is loaded ~5 s after power-on, when only
the boot CPU is running. So nothing is written until all of the following are true:

1. CPUID.06H reports HWP (bit 7) and EPP (bit 10). Otherwise the kext does nothing.
2. A timer fires, first 30 s after `start()`, then every 5 s for up to 60 attempts.
3. `hw.activecpu == hw.logicalcpu_max`, meaning every CPU is online. This is checked with sysctl, no rendezvous.
4. A read-only rendezvous sees `IA32_PM_ENABLE` (`0x770`) bit 0 = 1 on every CPU.

Then one rendezvous writes the value on every CPU and reads it back. Each CPU re-checks its own
`PM_ENABLE` first. A read-only verify pass runs 10 s later. On wake (`kIOMessageSystemHasPoweredOn`)
the same sequence runs again, starting 5 s after wake. Every MSR access sits inside a per-CPU action,
below that CPU's `PM_ENABLE` check.

## Status

```
ioreg -c EPPForce -r | grep EPP
```

| Property | Meaning |
|---|---|
| `EPPState` | `waiting` → `waiting-cpus` / `waiting-hwp` / `retrying` → `applied` → `ok-held` (or `OVERWRITTEN` if something rewrote the MSR before the verify pass), `gave-up` after 60 attempts |
| `EPPCPUsAtEpp10` / `EPPCPUsExamined` | CPUs reading back EPP `0x10` / CPUs in the last pass |
| `EPPSample0x774` | one read-back value (`268500737` = `0x1000ff01`) |
| `EPPCPUsOnline`, `EPPCPUsMax`, `EPPCPUsNotReady`, `EPPAttempts`, `EPPWakes`, `EPPLastPass`, `EPPVersion` | diagnostics |

The kext also logs with IOLog and os_log, but when it is injected by OpenCore neither reached the
unified log on the test machine. Use the registry properties.

## Build

Needs Xcode (tested with 26.5). There is no Xcode project; `build.sh` drives clang directly and
supplies the kmod_info stub (`src/kmod_info.c`) that Xcode would normally generate.

```
./build.sh          # → build/EPPForce.kext (x86_64, ad-hoc signed)
```

## Install (OpenCore)

Copy `build/EPPForce.kext` to `EFI/OC/Kexts/` and add a `Kernel → Add` entry
(`BundlePath` `EPPForce.kext`, `ExecutablePath` `Contents/MacOS/EPPForce`,
`PlistPath` `Contents/Info.plist`, `Arch` `Any`). Keep a bootable fallback EFI while testing.
To remove it, set `Enabled` to `false`.

## History

- **v1:** wrote from `start()` → hung at the Apple logo (HWP still off).
- **v2:** timer + per-CPU `PM_ENABLE` guard → booted and applied EPP.
- **v2.1:** added status properties, with an unguarded `rdmsr(0x774)` in `publish()` → #GP panic
  (`RCX = 0x774`, `EPPForce::tick() + 0x1e7`). That panic also showed only 1 CPU online at that point.
- **v2.2:** removed the unguarded read.
- **v2.3:** waits for all CPUs online + read-only HWP probe before any write.
- **v2.4:** adds os_log breadcrumbs → current version.

Debugging note: during testing, boots kept failing silently even with the kext disabled. They
recovered after removing `debug=0x100` from boot-args and doing a full power-off. It is unclear
which of the two fixed it.

## Caveats

This writes model-specific registers from the kernel on every CPU. It is tested on one machine only.
A different CPU, or a macOS that manages EPP itself, may behave differently. The kext uses the
private symbol `mp_rendezvous_no_intrs` (`com.apple.kpi.unsupported`).

## License

MIT — see [LICENSE](LICENSE).
