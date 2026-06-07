# blackwell-vram-temp

**Read the GDDR7 memory temperature of an NVIDIA RTX 50-series (Blackwell) GPU
on Linux — rootless, no kernel module of your own, no NVIDIA SDK.**

On RTX 50 (Blackwell) consumer cards, `nvidia-smi` reports the memory junction
temperature as `N/A`:

```
$ nvidia-smi -q -d TEMPERATURE | grep -i memory
        GPU Memory Current Temp          : N/A
        GPU Memory Max Operating Temp     : N/A
```

This tool reads it anyway — typically within a fraction of a degree of what
third-party Windows tools show — by talking to the open NVIDIA kernel module
through its **NV00DE "RM User Shared Data" (RUSD)** interface and pulling the
`MEMORY` temperature slot straight out of a shared-memory buffer.

```
$ blackwell-vram-temp
...
GDDR7 temp [type 1 MEMORY(GDDR7)]: raw=15104 (0x00003b00)  59.000 C
RESULT GDDR7_MEMORY_TEMP_C = 59.000  (raw NvSFXP24_8 = 15104)
```

No root. No `setuid`. No custom kernel module. It runs as your normal user
against `/dev/nvidiactl` + `/dev/nvidia0`.

---

## Why this is needed

NVIDIA made two separate decisions on Blackwell that, together, leave Linux
users with no supported way to read VRAM temperature on a GeForce card:

1. **The Hot Spot (GPU junction) sensor was removed on RTX 50.** This is not a
   software gate — the per-die hottest-point readout that existed through Ada
   (RTX 40) is simply gone on Blackwell. No tool on any OS reports it, because
   the hardware no longer exposes it. **This program does not and cannot bring
   Hot Spot back.** See the caveats below; this is the one number you will not
   get.

2. **The GDDR7 memory temperature is *gated*, not gone.** The sensor exists and
   the driver reads it (Blackwell data-center parts expose it via NVML, and
   Windows third-party tools read it on GeForce), but on GeForce SKUs the normal
   query paths return `N/A` / "not supported". The data is present in the
   driver; the public query surface just refuses to hand it to you.

The memory temperature is the one that actually matters for day-to-day use:
GDDR7 runs hot, throttles, and is the limiting factor when undervolting,
overclocking, tuning fan curves, or validating a cooler/backplate mod. Being
told `N/A` on a $2000 card is, to put it mildly, frustrating.

This tool exists to recover that **one specific, gated-but-present** number
through an interface the open kernel module genuinely exposes.

---

## How it works

The [open GPU kernel module](https://github.com/NVIDIA/open-gpu-kernel-modules)
exposes a class object, **`RM_USER_SHARED_DATA` (class `0x00de`, "NV00DE")**,
whose entire purpose is to let *userspace* read a curated set of telemetry
without an ioctl round-trip per sample. The driver's GSP-side poller writes
values into a shared page on an interval; userspace maps that page read-only and
reads fields directly. NVIDIA itself uses this path internally for
`nvidia-smi`-style queries.

The sequence this tool performs (all on a `/dev/nvidiactl` fd, with
`/dev/nvidia0` held open so the GPU counts as "accessible" to a non-kernel
client):

1. `RM_ALLOC` a root **client** (`0x41`), a **device** (`0x80`), and a
   **subdevice** (`0x2080`) — the standard handle hierarchy.
2. `RM_ALLOC` the **NV00DE** object under the subdevice with
   `polledDataMask = NV00DE_RUSD_POLL_THERMAL (0x10)`.
3. `RM_CONTROL` → `NV00DE_CTRL_CMD_REQUEST_DATA_POLL` to (re)arm thermal polling.
4. `RM_MAP_MEMORY` the object's shared buffer and `mmap()` it **read-only** into
   the process.
5. Read `NV00DE_SHARED_DATA.temperatures[RUSD_TEMPERATURE_TYPE_MEMORY]` using
   the driver's **seqlock** protocol (load-acquire fence, copy, re-check the
   timestamp; retry on a torn read), exactly as the in-kernel `RUSD_READ_DATA`
   macro does.
6. Convert the raw `NvTemp` (`NvSFXP24_8`, signed 24.8 fixed-point) to Celsius:
   `celsius = raw / 256.0`.

### Self-contained, byte-exact layout

The single translation unit `blackwell_vram_temp.c` **vendors the minimal subset
of the NVIDIA 610.43.02 SDK headers verbatim** — base types, the fixed-point
temperature type, the two ECC ctrl structs that sit ahead of `temperatures[]`,
and the full `NV00DE_SHARED_DATA` layout. Because the full struct is reproduced
rather than hand-offset, the **compiler** computes the byte-exact offset of the
memory slot for us. `_Static_assert`s pin the observed offset (`2336`) and
struct size (`3152`); if the vendored copy ever drifts, the **build fails**
instead of silently reading the wrong bytes. No `-I`, no SDK clone, no headers
to fetch — `gcc blackwell_vram_temp.c` is the whole build.

---

## Why every other approach fails

Before reaching for RUSD, the obvious routes were each tried and each dead-ends
on Blackwell GeForce. This is the core reason this tool has to exist:

| Approach | What it is | Why it fails on RTX 50 GeForce |
|---|---|---|
| **NVML** (`nvmlDeviceGetTemperature`, `...GetFieldValues`) | The documented, supported telemetry API behind `nvidia-smi` | The `MEMORY` sensor / memory-temp field is **gated on GeForce** — returns `NVML_ERROR_NOT_SUPPORTED` / `N/A`. Present on data-center Blackwell, refused on consumer. |
| **`THERMAL_SYSTEM_EXECUTE_V2`** (RM thermal ctrl) | Lower-level RM thermal control call | The Blackwell thermal-system provider exposes **no `MEMORY` target** — there is a GPU target but no memory channel to query through this path. |
| **Root + thermal sysfs / privileged RM** | "Just run it as root" | Privilege **does not help** — the limitation is the absent query target, not a permission check. Root reads the same gated/empty surface. |
| **BAR0 MMIO poke** (the `gddr6` approach) | Read the memory-temp register directly over the GPU's BAR0 register window | The fixed BAR0 register offsets used for GDDR6 on Ampere/Ada are **dead on Blackwell** — the readout moved/changed and the old offsets return nothing useful. |
| **SMBPBI** (SMBus Post-Box Interface) | Out-of-band BMC/management telemetry | **Out-of-band** — meant for server baseboard management controllers over SMBus, not reachable as an in-band path on a desktop GeForce. |
| **RUSD / NV00DE** ← *this tool* | Shared-data buffer exposed by the open kernel module | **Works.** The `MEMORY` slot is populated by the GSP poller and mappable read-only by a normal user. |

The short version: on Blackwell GeForce the memory temperature is reachable
**only** through the shared-data buffer, and only because the open kernel module
publishes that ABI.

---

## Build

Requires `gcc` and a glibc toolchain. No NVIDIA SDK, no `-I` paths.

```sh
make
```

This runs `gcc -O2 -Wall blackwell_vram_temp.c -o blackwell-vram-temp`.

Optionally install into `~/.local/bin`:

```sh
make install        # copies the binary to ~/.local/bin/blackwell-vram-temp
```

Clean up:

```sh
make clean
```

---

## Run / usage

```sh
./blackwell-vram-temp
```

There are no flags. The tool opens the NVIDIA device nodes, sets up the RUSD
buffer, polls until the `MEMORY` slot is valid (up to ~6 s on a cold buffer),
prints both the GPU-target and GDDR7 memory temperatures, and exits.

**Permissions:** you need read/write access to `/dev/nvidiactl` and
`/dev/nvidia0`. On a normal desktop with the proprietary/open driver installed,
your interactive user already has this (the nodes are typically `0666`). **No
root required.**

**Exit codes:**

| Code | Meaning |
|---|---|
| `0` | Success — memory temperature read and printed |
| `2` | Could not open `/dev/nvidiactl` or `/dev/nvidia0` |
| `3` | RM allocation / setup failed |
| `10` | Buffer mapped but the `MEMORY` slot never populated (see caveats) |

### Sample output

```
$ ./blackwell-vram-temp
Allocated client handle:    0xc1d00000
Allocated NV00DE handle:     0x00de0001 (class 0xde)
Requested POLL_THERMAL (0x10) on NV00DE
Mapped NV00DE_SHARED_DATA: 0x7f3c2a4e0000 (maplen 4096, sizeof struct 3152)
offsetof(temperatures[MEMORY].temperature) = 2336 bytes
GPU   temp [type 0 GPU          ]: raw=11776 (0x00002e00)  46.000 C  (ts=0x0000018f3a2b1c40)
GDDR7 temp [type 1 MEMORY(GDDR7)]: raw=15104 (0x00003b00)  59.000 C  (ts=0x0000018f3a2b1c80)

RESULT GDDR7_MEMORY_TEMP_C = 59.000  (raw NvSFXP24_8 = 15104)
```

The machine-readable `RESULT GDDR7_MEMORY_TEMP_C = ...` line is easy to scrape
for a fan-curve daemon, a Conky/Waybar widget, or a logging script.

---

## Supported / tested hardware

Tested on:

- **GPU:** NVIDIA GeForce RTX 5090 (GB202, Blackwell)
- **Driver:** `610.43.02`, **open** kernel module flavor
  (`nvidia-open` / `open-gpu-kernel-modules`)
- **OS / kernel:** Fedora / Bazzite, kernel **7.0**
- **Arch:** x86-64

It should work on other RTX 50 Blackwell GeForce cards (5070/5070 Ti/5080/etc.)
running the **open** kernel module on a comparable driver, since they share the
NV00DE/RUSD ABI — but those are untested here. The proprietary (closed) module
also implements NV00DE; the open module is what this was developed and verified
against.

---

## Caveats (read this)

This tool is honest about what it is:

- **It uses an undocumented RM ABI.** The escape ioctl numbers, parameter
  structs, and the `NV00DE_SHARED_DATA` layout are reconstructed from the open
  kernel module source and pinned with `_Static_assert`s. They are **not** a
  stable public API.
- **It may break on driver updates.** If NVIDIA changes the `NV00DE_SHARED_DATA`
  layout, the offset asserts will fail at compile time (good — you'll know
  immediately) or, worse, the ABI shifts in a way the asserts don't catch. If a
  build breaks or output looks wrong after a driver bump, re-check the layout
  against the matching `open-gpu-kernel-modules` tag.
- **Hot Spot is genuinely unreadable.** The GPU junction / Hot Spot sensor was
  **removed in hardware** on RTX 50. This tool gives you the GDDR7 *memory*
  temperature and the GPU-target temperature; it cannot give you Hot Spot,
  because nothing can. Don't file an issue asking for it.
- **The `MEMORY` slot can stay empty (exit `10`).** RUSD is poller-driven; on a
  cold buffer the memory slot may need a few hundred ms to populate, which is
  why the tool retries. If it *never* populates on your SKU/driver, that's the
  residual GSP-side gating risk — the interface worked but the value wasn't
  published.
- **Read-only.** The buffer is mapped `PROT_READ`; this tool only reads
  telemetry. It does not and cannot change clocks, fan speeds, or power limits.

---

## Prior art & credit

This builds directly on the work of others who chipped away at the same problem:

- **[olealgoritme/gddr6](https://github.com/olealgoritme/gddr6)** and the
  **[ThomasBaruzier fork](https://github.com/ThomasBaruzier/gddr6)** — the
  original BAR0-MMIO approach to reading GDDR6 memory temperature on
  Ampere/Ada. That technique is what *doesn't* work on Blackwell (see the table
  above), and tracing *why* it broke is what motivated finding the RUSD path.
- **[LibreHardwareMonitor PR #1878](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/pull/1878)**
  — added the Blackwell memory-temperature channel on **Windows**, confirming
  the sensor exists on GeForce and giving a cross-OS sanity check for the values
  this tool reads.
- **[NVIDIA/open-gpu-kernel-modules](https://github.com/NVIDIA/open-gpu-kernel-modules)**
  — the source of truth for the NV00DE / RUSD ABI: the class definition
  (`cl00de.h`), the `NV00DE_SHARED_DATA` layout, the `RUSD_READ_DATA` seqlock
  protocol, and the request-data-poll control. Without the open module
  publishing this, none of this would be possible.

---

## License

MIT — see [LICENSE](LICENSE). Copyright (c) 2026 RogueSergeant.
