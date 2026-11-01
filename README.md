# hyprland-gpu-switch

Automatic renderer switching and GPU-assisted cross-GPU frame transport for Hyprland.

When a monitor is connected to a discrete GPU, the plugin moves Hyprland's renderer to that GPU. When its last monitor is disconnected, rendering returns to the integrated GPU. GPU roles are detected automatically, without rules tied to vendor names or connector numbers.

The plugin runs on stock Hyprland. Existing windows remain open during migration; the displays may briefly flicker.

## Cross-GPU frame transport

Transport is enabled automatically whenever a supported application buffer is produced on a different GPU from Hyprland's renderer. It works without a previous renderer migration and while automatic switching is disabled. Same-GPU buffers keep the ordinary rendering path.

### SDR and HDR copying

- Runs an embedded Vulkan worker on the source GPU, selected by DRM device or PCI identity. No PRIME environment flag or forced Vulkan driver is required.
- Transports RGBA/BGRA8, RGB10/A2 and FP16 buffers without synchronous CPU pixel readback and upload. Native-format copies preserve color encoding, channel precision and alpha.
- Imports the source buffer's actual DRM modifier, including supported tiled layouts, and exports a shared linear buffer for the compositor GPU.
- Uses direct destination import when supported. For external-only DMA-BUF formats, a GPU sampling pass copies into a local compositor-owned texture, preserving values and restoring GL state.
- Handles ordinary window composition and supported window-capture passes without rewriting the application's surface state.

### Compact scRGB-to-PQ transport

Eligible opaque, linear FP16/scRGB buffers on HDR outputs are converted to compact BT.2020/PQ RGB10 on the source GPU before transfer. The packed result uses host-visible shared memory; on integrated GPUs that memory may also be reported as device-local. Discrete-GPU output retains the host-memory requirement.

Other supported buffers use native-format copying. Already-PQ content is not converted to PQ again, and FP16 surfaces requiring full alpha precision keep FP16 transport. SDR output does not require HDR conversion to benefit from optimized copying. Hyprland retains responsibility for tone mapping, composition, blending and final display conversion.

### Synchronization and resource reuse

- Identifies the producer using DMA-BUF exporter information, client DRM descriptors and native-fence provenance. Confirmed producer identities are cached while queued surface states are still pending, before completed fences can lose their driver metadata.
- Validates format, modifier, layout and GPU identity. Unknown or ambiguous provenance and unsupported transformations use the ordinary rendering path.
- Uses native fences for source acquisition, conversion completion, compositor sampling and output-slot reuse. Source buffers remain retained until the GPU has finished reading them.
- Starts preprocessing on source commits when possible, reuses three output slots and reserves capacity across ordinary window resizes. Where supported, row-band pipelining overlaps transfer and conversion; other queue configurations use the serial GPU path.
- Preserves specialized device-local input handling for confirmed NVIDIA producers without applying it to buffers from unrelated drivers.
- Starts the helper on demand and stops it after idle work has drained. Before renderer migration, worker submissions and transport resources are drained; imports and tracking are recreated for the resulting renderer, including rollback.

### Scope and limitations

The current worker handles one application surface at a time. Multiple competing surfaces can fall back to normal rendering; a per-surface worker pool is not implemented. Unsupported formats and layouts retain the compositor's fallback behavior. Transport does not move application rendering or video decoding to another GPU.

The worker executable and compute shader are embedded in the plugin, so no separate helper installation is needed. Detailed worker diagnostics are disabled in the normal build. Use `hyprctl gpu-switch transfers` to inspect the active route and transport mode (`gpu-copy` or `pq10-convert`).

## Requirements

This version targets **Hyprland 0.56.2** (`efb50993780079460b0cbed1363e2166a2de1d9f`) and **Aquamarine 0.15.1**. Development headers must match the installed compositor and libraries. Other versions require compatibility work.

Build tools: **GCC/G++ 14 or another compatible C++23 toolchain**, GNU Make, `pkg-config`, `glslangValidator` (`glslang-tools`) and binutils. GCC 14.2 is the tested compiler.

Required development libraries:

| Library | Development package on the Debian-based development system |
| --- | --- |
| Hyprland | `hyprland-dev` |
| Aquamarine | `libaquamarine-dev` |
| Hyprutils | `libhyprutils-dev` |
| Pixman | `libpixman-1-dev` |
| libdrm | `libdrm-dev` |
| Pango and Cairo | `libpango1.0-dev` and its development dependencies |
| libinput | `libinput-dev` |
| libudev | `libudev-dev` |
| Wayland server | `libwayland-dev` |
| libxkbcommon | `libxkbcommon-dev` |
| EGL and OpenGL ES | `libegl-dev`, `libgles-dev` |
| Vulkan headers and loader | `libvulkan-dev` |

On systems providing the compatible Hyprland and Aquamarine packages:

```sh
sudo apt install build-essential pkg-config glslang-tools binutils \
  hyprland-dev libaquamarine-dev libhyprutils-dev \
  libpixman-1-dev libdrm-dev libpango1.0-dev \
  libinput-dev libudev-dev libwayland-dev libxkbcommon-dev \
  libegl-dev libgles-dev libvulkan-dev
```

Package names and availability vary by distribution. Install Hyprland's matching transitive development dependencies as well. The build checks the required package versions before compiling.

## Build and install

```sh
make
make install
```

`make` builds `hyprland-gpu-switch.so` with **automatic switching enabled** and **diagnostics disabled**. `make install` installs it to `~/.local/lib/hyprland/`. Parallel builds are supported with `make -j$(nproc)`.

Use `CXX=g++-14` to select a compiler, or `PREFIX=/custom/prefix` to change the installation prefix. To remove the installed library:

```sh
make uninstall
```

## Install with hyprpm

```sh
hyprpm add https://github.com/yayuuu/hyprland-gpu-switch.git
hyprpm enable hyprland-gpu-switch
```

The manifest has no commit pins: hyprpm builds the latest revision of the default branch. The same compiler, dependencies and version checks still apply.

This plugin needs to load before rendering starts. Enabling it in an existing session is not enough: configure early loading of the installed library and start a new login session. For Hyprland 0.56.2, the hyprpm-managed library can be loaded with:

```lua
hl.plugin.load("/var/cache/hyprpm/" .. os.getenv("USER") .. "/hyprland-gpu-switch/hyprland-gpu-switch.so")
```

Use this managed path instead of the manual installation path below. Do not rely solely on a late `hyprpm reload` to initialize resource tracking.

## Configure Hyprland

Load the plugin early in your Hyprland Lua configuration, before the first frame:

```lua
hl.plugin.load(os.getenv("HOME") .. "/.local/lib/hyprland/hyprland-gpu-switch.so")
```

Start a new login session after adding or updating the plugin. It must track resources before the first frame; loading it into an already-rendering session allows status queries but blocks migration.

Automatic switching starts when the plugin loads. No separate enable command is required.

## Commands

| Command | Action |
| --- | --- |
| `hyprctl gpu-switch status` | Show the actual renderer, target GPU and migration state |
| `hyprctl gpu-switch disable` | Pause automatic switching for this session |
| `hyprctl gpu-switch enable` | Resume automatic switching |
| `hyprctl gpu-switch transfers` | Show client transport readiness, mode and GPU route |
| `hyprctl gpu-switch retry` | Clear a migration error and allow another attempt |

Disabling switching leaves the current renderer in place. Cross-GPU client transport remains active independently. After a migration, the plugin keeps live GPU resources and cannot be unloaded safely within that session.

## Compatibility and power management

This is an early, version-specific release. Bidirectional switching, monitor disconnect/reconnect, retained windows and HDR output have been tested on an Intel UHD/NVIDIA T1200 laptop. Other driver combinations are not yet verified.

GPU sleep requires working runtime power management in the driver. A complete suspend/wake cycle has been verified after enabling fine-grained runtime PM on the test system. Monitoring tools such as nvtop can keep the GPU awake.

Complete rollback after the first presentation and retirement of all retained contexts are not yet implemented. Physical removal of an entire eGPU is unverified. Unsupported cross-GPU HDR formats are rejected rather than silently converted to lower precision.

## Development

Diagnostic tracing and developer-only commands are compiled out by default. To enable them, build with `make DIAGNOSTICS=1`.

See [CONTRIBUTING.md](CONTRIBUTING.md) for tests, diagnostic commands, implementation notes and developer tools.

## License

[BSD-3-Clause](LICENSE). Vendored Aquamarine declarations retain their upstream copyright notice and [license](vendor/aquamarine/LICENSE).
