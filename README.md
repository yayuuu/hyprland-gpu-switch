# hyprland-gpu-switch

Automatic renderer switching for hybrid-GPU systems running Hyprland.

When a monitor is connected to a discrete GPU, the plugin moves Hyprland's renderer to that GPU. When its last monitor is disconnected, rendering returns to the integrated GPU. GPU roles are detected automatically, without rules tied to vendor names or connector numbers.

The plugin runs on stock Hyprland. Existing windows remain open during migration; the displays may briefly flicker.

## Requirements

This version targets **Hyprland 0.56.2** (`efb50993780079460b0cbed1363e2166a2de1d9f`) and **Aquamarine 0.15.1**. Development headers must match the installed compositor and libraries. Other versions require compatibility work.

Build tools: **GCC/G++ 14 or another compatible C++23 toolchain**, GNU Make and `pkg-config`. GCC 14.2 is the tested compiler.

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
sudo apt install build-essential pkg-config \
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
| `hyprctl gpu-switch retry` | Clear a migration error and allow another attempt |

Disabling switching leaves the current renderer in place. After a migration, the plugin keeps live GPU resources and cannot be unloaded safely within that session.

## Compatibility and power management

This is an early, version-specific release. Bidirectional switching, monitor disconnect/reconnect, retained windows and HDR output have been tested on an Intel UHD/NVIDIA T1200 laptop. Other driver combinations are not yet verified.

GPU sleep requires working runtime power management in the driver. A complete suspend/wake cycle has been verified after enabling fine-grained runtime PM on the test system. Monitoring tools such as nvtop can keep the GPU awake.

Complete rollback after the first presentation and retirement of all retained contexts are not yet implemented. Physical removal of an entire eGPU is unverified. Unsupported cross-GPU HDR formats are rejected rather than silently converted to lower precision.

## Development

Diagnostic tracing and developer-only commands are compiled out by default. To enable them, build with `make DIAGNOSTICS=1`.

See [CONTRIBUTING.md](CONTRIBUTING.md) for tests, diagnostic commands, implementation notes and developer tools.

## License

[BSD-3-Clause](LICENSE). Vendored Aquamarine declarations retain their upstream copyright notice and [license](vendor/aquamarine/LICENSE).
