# blackwell-vram-temp

**Read the GDDR7 memory temperature of an NVIDIA RTX 50-series (Blackwell) GPU
on Linux — no kernel module of your own, no NVIDIA SDK. Rootless on driver
610.x; needs `CAP_SYS_ADMIN` on 615.x (see [Driver 615 and later](#driver-615-and-later)).**

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

On driver 610.x: no root, no `setuid`, no custom kernel module. It runs as
your normal user against `/dev/nvidiactl` + `/dev/nvidia0`.

---

## Driver 615 and later

From **615.71.09** the GSP firmware no longer writes the `MEMORY` slot of the
RUSD buffer on GeForce. The `GPU` slot next to it is still live, the buffer
layout up to `temperatures[]` is unchanged, and the open kernel module's RUSD
code is identical to 610.43.02, so the change is inside the closed firmware.
On 615 `nvidia-smi` also reports `GPU Current T.Limit Temp` as `N/A`.

When the RUSD slot stays empty, the tool falls back to reading the GDDR7
temperature codes directly from the frame-buffer partition (FBPA) "DQR"
registers in BAR0. The register offsets and decoding come from
[gputemps](https://github.com/ThomasBaruzier/gddr6-core-junction-vram-temps).
Rather than mapping BAR0 through `/dev/mem` (which `CONFIG_IO_STRICT_DEVMEM`
blocks while the nvidia driver owns the region, and which needs
`iomem=relaxed`), the registers are read through the driver's own
`NV2080_CTRL_CMD_GPU_EXEC_REG_OPS` control.

That control skips its register allowlist only for `CAP_SYS_ADMIN`
(`osIsAdministrator()` → `capable(CAP_SYS_ADMIN)`), so on 615 the tool must run
as root or carry the capability:

```sh
sudo setcap cap_sys_admin+ep ~/.local/bin/blackwell-vram-temp
```

The binary takes no arguments and only issues fixed read-only register ops, but
`CAP_SYS_ADMIN` is a broad capability; decide whether you are comfortable with
it. The capability is stored on the file, so it survives reboots and driver
updates, but replacing the binary drops it. Use `make install-cap` to install
and reapply it in one step.

```
$ sudo ./blackwell-vram-temp
...
GDDR7 temp [type 1 MEMORY(GDDR7)]: <slot empty>  ts=0x0000000000000000
GDDR7 DQR registers: strap=0x069f9803 (standard), scanning 16 FBPAs
  no valid sensors on FBPA 6 7 8 9 10 11 12 13 14 15
  FBPA  0:  58 C  56 C  58 C  56 C
  FBPA  1:  58 C  56 C  58 C  56 C
  FBPA  2:  58 C  58 C  58 C  58 C
  FBPA  3:  56 C  56 C  56 C  56 C
  FBPA  4:  58 C  58 C  58 C  58 C
  FBPA  5:  58 C  58 C  58 C  58 C

RESULT GDDR7_MEMORY_TEMP_C = 58.000  (DQR registers, hottest of 24 sensors)
```

On the RTX 5090 tested, only FBPAs 0–5 set their valid bits (FBPAs 6 and 7
read as the `0xBADF....` poison value). How those 24 codes map onto the card's
16 GDDR7 packages is not established, so the result is the hottest *reported*
sensor, not a guaranteed hottest package.

The DQR value is the hottest valid sensor code. Codes are in 2 °C steps
(`(code - 20) * 2`), so this path is coarser than the RUSD value.

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
| **Root + thermal sysfs / privileged RM** | "Just run it as root" | For the thermal queries above, privilege **does not help** — the limitation is the absent query target, not a permission check. (Root does unlock the register fallback below.) |
| **BAR0 MMIO poke** (the `gddr6` approach) | Read the memory-temp register directly over the GPU's BAR0 register window | The fixed BAR0 register offsets used for GDDR6 on Ampere/Ada are **dead on Blackwell**. GDDR7 codes live in different (FBPA DQR) registers, which this tool uses as its 615.x fallback — via the driver's `EXEC_REG_OPS` control, not `/dev/mem`. |
| **SMBPBI** (SMBus Post-Box Interface) | Out-of-band BMC/management telemetry | **Out-of-band** — meant for server baseboard management controllers over SMBus, not reachable as an in-band path on a desktop GeForce. |
| **RUSD / NV00DE** ← *this tool, 610.x* | Shared-data buffer exposed by the open kernel module | **Works on 610.x.** The `MEMORY` slot is populated by the GSP poller and mappable read-only by a normal user. Empty from 615.71.09. |
| **GDDR7 DQR registers via `EXEC_REG_OPS`** ← *this tool, 615.x* | Driver-mediated BAR0 register reads | **Works with `CAP_SYS_ADMIN`.** Does not depend on the GSP firmware. |

The short version: on Blackwell GeForce the memory temperature is reachable
rootless **only** through the shared-data buffer, and only on drivers whose
firmware still publishes it.

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
make install-cap    # same, then sudo setcap cap_sys_admin+ep (driver 615+)
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
prints both the GPU-target and GDDR7 memory temperatures, and exits. If the
`GPU` slot is live but `MEMORY` stays empty for ~1.6 s, it moves on to the DQR
register fallback.

**Permissions:** you need read/write access to `/dev/nvidiactl` and
`/dev/nvidia0`. On a normal desktop with the proprietary/open driver installed,
your interactive user already has this (the nodes are typically `0666`). **No
root required on 610.x.** On 615.x the fallback needs `CAP_SYS_ADMIN` (see
[Driver 615 and later](#driver-615-and-later)).

**Exit codes:**

| Code | Meaning |
|---|---|
| `0` | Success — memory temperature read and printed |
| `2` | Could not open `/dev/nvidiactl` or `/dev/nvidia0` |
| `3` | RM allocation / setup failed |
| `10` | `MEMORY` slot empty and the DQR fallback found no valid sensor codes |
| `11` | `MEMORY` slot empty and the DQR fallback was refused (needs `CAP_SYS_ADMIN`) |

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
- **Driver:** `610.43.02` (RUSD path) and `615.71.09` (DQR fallback), **open**
  kernel module flavor (`nvidia-open` / `open-gpu-kernel-modules`)
- **OS / kernel:** Fedora / Bazzite, kernel **7.0** (610) and **7.2** (615)
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
- **The `MEMORY` slot can stay empty.** RUSD is poller-driven; on a cold buffer
  the memory slot may need a few hundred ms to populate, which is why the tool
  retries. From 615.71.09 it never populates on GeForce, and the tool falls
  back to the DQR registers.
- **The DQR decoding is reverse-engineered.** Offsets, the valid mask, the
  poison value and the `(code - 20) * 2` scale come from gputemps, not from
  NVIDIA documentation, and have 2 °C resolution.
- **Read-only.** The buffer is mapped `PROT_READ` and the register path only
  issues `READ_32` ops; this tool only reads telemetry. It does not and cannot change clocks, fan speeds, or power limits.

---

## Prior art & credit

This builds directly on the work of others who chipped away at the same problem:

- **[olealgoritme/gddr6](https://github.com/olealgoritme/gddr6)** and the
  **[ThomasBaruzier fork](https://github.com/ThomasBaruzier/gddr6)** — the
  original BAR0-MMIO approach to reading GDDR6 memory temperature on
  Ampere/Ada. That technique is what *doesn't* work on Blackwell (see the table
  above), and tracing *why* it broke is what motivated finding the RUSD path.
- **[gputemps](https://github.com/ThomasBaruzier/gddr6-core-junction-vram-temps)**
  — found the GDDR7 DQR register layout and decoding on Blackwell. The 615.x
  fallback here uses its offsets and decode rules.
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
