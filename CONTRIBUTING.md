# Contributing

Read [README.md](README.md) for supported versions, build dependencies and installation. Keep development focused on GPU migration and compatibility with stock Hyprland.

## Build profiles

```sh
make                         # Normal build, diagnostics off
make DIAGNOSTICS=1           # Diagnostic build
make                        # Select the normal build again
```

Both profiles default to automatic switching. Build artifacts are kept separately in `.build/release/` and `.build/diagnostics/`. The top-level `hyprland-gpu-switch.so` is updated to match the requested profile even if that profile was built earlier. No manual clean is needed to switch profiles.

`DIAGNOSTICS` accepts only `0` or `1`. It defines `GPU_SWITCH_DIAGNOSTICS` for the compiler; direct source builds default this macro to `0` as well.

Diagnostic builds include:

- Migration-stage tracing.
- Client-buffer copy/skip counters and detailed bridge information.
- Aquamarine transfer-context inspection.
- GPU inventory, topology generation and debounce details in status output.
- The developer-only renderer override, additionally gated by `HYPR_GPU_SWITCH_TEST`.

Normal builds retain operational status, error reporting and the enable/disable/retry controls. Disabling diagnostics does not remove validation, synchronization or format checks.

## Diagnostic commands

```sh
hyprctl gpu-switch bridges
hyprctl gpu-switch contexts
```

With a diagnostic build and `HYPR_GPU_SWITCH_TEST` set in Hyprland's environment, `hyprctl gpu-switch select integrated` and `hyprctl gpu-switch select discrete` override normal target selection for testing.

Migration traces are written to `$XDG_STATE_HOME/hyprland-gpu-switch/migration-trace.log`, falling back to `$HOME/.local/state/hyprland-gpu-switch/migration-trace.log`. Trace files use mode 0600; failures to create or write them do not interrupt migration. Normal builds do not create diagnostic files or directories.

## Tests

All test sources are in `tests/`. Test executables are stored under the selected build profile in `.build/release/` or `.build/diagnostics/`.

```sh
make check
make DIAGNOSTICS=1 check
```

These tests cover GPU selection policy, buffer format/modifier compatibility, route planning, sudden target removal and diagnostic output behavior. Diagnostic tests check XDG/HOME paths, private file permissions, symlink rejection and nonfatal I/O failures. Release-profile tests verify that no trace output is created.

GPU tests are built separately and require access to the chosen render nodes:

```sh
make native-test transfer-test
.build/release/native-test /dev/dri/renderD128 /dev/dri/renderD129
.build/release/transfer-test /dev/dri/renderD129 /dev/dri/renderD128
```

Use the render nodes appropriate to the test system. These tests cover texture migration, metadata-only reads, floating-point 3D LUTs, packed 10-bit formats, pixel correctness and transfer-context recovery. Transfer tests use a NULL backend with no Wayland connection, display outputs or modesetting.

Live-session testing must respect authorization given by the workspace owner. Never restart or log out their desktop automatically. A nested Wayland compositor is not an isolation boundary for driver or DMA-BUF import behavior.

## Developer tools

Diagnostic-only sources live in `diagnostics/`. Their tools are optional and never part of the default build:

```sh
make DIAGNOSTICS=1 tools
```

- `.build/diagnostics/gpu-switch-probe`: standalone Vulkan/DRM inventory and target selection.
- `.build/diagnostics/hook-probe.so`: inspect exact and demangled Hyprland function symbols.
- `.build/diagnostics/transfer-opt.so`: separate transfer experiments; not required by the migration plugin and not loaded automatically.

While the transfer experiment is loaded, its counters and controls use `hyprctl gpu-transfer-opt status|enable|disable|reset`. It includes upload-storage reuse and mapped destination-buffer experiments with exact-prologue checks. Those experiments did not establish an overall performance improvement on the test system and are not enabled in the main plugin.

## Implementation

Migration runs from a Wayland idle callback outside timer callback snapshots. Hotplug changes are debounced, and topology changes invalidate in-progress plans. Frame scheduling is paused while pending pageflips and render fences drain.

The target EGL context, textures, shaders, framebuffers and output buffers are prepared before live handles change. Aquamarine's primary/secondary routes are updated while each output keeps scanout memory on its own GPU. Actual buffer formats and modifiers are checked before import; unsupported layouts must never be forced through a driver.

Client buffers that cannot be imported on the target GPU retain a source context and use a shadow texture. Recognized Wayland surfaces refresh their shadow after a surface commit, rather than on every bind. Unrecognized textures keep a conservative copy path. Migration invalidates all target shadows.

Synchronization device handles and DMA-BUF feedback follow the selected renderer. Migration recreates rendering resources through the compositor's normal lifecycle; runtime-only customizations may need to be reapplied.

## ABI and resource ownership

The plugin uses Hyprland and Aquamarine internals. The running Hyprland hash must match the development headers. Aquamarine internal declarations are pinned to 0.15.1, commit `f31c47a1b9d300847d8fc3108ad959448103dfc1`; preserve their upstream license in `vendor/aquamarine/LICENSE`.

Function lookup must demangle the exact symbol signature associated with its address, rather than trusting separately cached name lists. Preserve this check when adding hooks.

The module uses `RTLD_NODELETE` because live buffers can retain C++ callbacks. Replacing a file at the same path does not replace code already mapped in a running process. The plugin refuses unloading after migration; use disable to pause policy decisions, and start a new session to test a replacement build with complete tracking.

Retained contexts and allocators still need a more complete retirement strategy. Do not destroy an origin context while a client bridge depends on it. Do not terminate an EGL display owned by another component. Keep these limitations explicit when changing lifecycle behavior.

## Packaging

The default installation prefix is `$HOME/.local`; `LIBDIR` overrides the plugin directory. System packages can use a staging root:

```sh
make install PREFIX=/usr DESTDIR=/path/to/package-root
```

Installation replaces the library atomically. It does not reload a running plugin or edit Hyprland configuration. `make uninstall` removes only the installed plugin library, and `make clean` removes build outputs. Do not commit generated libraries or test binaries.

## hyprpm manifest

`hyprpm.toml` intentionally omits `commit_pins`. hyprpm therefore follows the latest revision of the repository's default branch. Compatibility remains enforced by the build's dependency version checks and the plugin's Hyprland hash checks.

The manifest runs `make clean` before `make DIAGNOSTICS=0` to rebuild against the selected Hyprland headers and avoid accepting a stale shared library after a failed build. No revision placeholders or fabricated commit hashes are used.

The plugin must still be loaded before the first rendered frame. See README.md for the managed library path; starting it only through a late session reload leaves resource tracking incomplete.
