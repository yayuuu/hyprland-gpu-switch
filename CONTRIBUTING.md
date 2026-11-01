# Contributing

Read [README.md](README.md) for supported versions, build dependencies and installation. Keep development focused on GPU migration, cross-GPU client transport and compatibility with stock Hyprland.

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

The `bridges` report also exposes cumulative `wait_ns`, `read_ns`, `upload_ns` and `max_copy_ns`. Take two snapshots around a workload to distinguish waiting on the compositor GPU from source readback and target upload. Upload timing measures CPU call duration, not completion of all queued GPU work. These counters are absent from normal builds.

A standalone client-copy benchmark uses render-node contexts without connecting to Wayland or changing display outputs:

```sh
g++ -std=c++23 -O2 diagnostics/client-copy-bench.cpp native.cpp -lEGL -lGLESv2 -o .build/diagnostics/client-copy-bench
.build/diagnostics/client-copy-bench /dev/dri/renderD128 /dev/dri/renderD129
.build/diagnostics/client-copy-bench /dev/dri/renderD128 /dev/dri/renderD129 reuse
```

Append `reuse half` instead of `reuse` to benchmark RGBA16F buffers, including the native half-float readback path. Create `.build/diagnostics` first if it does not exist. Substitute the actual source and target render nodes. This benchmark isolates synchronous copying under synthetic conditions; it does not prove that a particular live client uses the fallback path.

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

Function lookup uses `function-hooks.hpp` to demangle the exact symbol signature associated with its address, rather than trusting separately cached name lists. Preserve this check when adding hooks. Render-pass hooks and elements must forward the supplied `CRenderContext`, including for isolated capture; texture bind hooks borrow that context only during synchronous pass execution.

The module uses `RTLD_NODELETE` because live buffers can retain C++ callbacks. Replacing a file at the same path does not replace code already mapped in a running process. The plugin refuses unloading after migration; use disable to pause policy decisions, and start a new session to test a replacement build with complete tracking.

Retained contexts and allocators still need a more complete retirement strategy. Do not destroy an origin context while a client bridge depends on it. Do not terminate an EGL display owned by another component. Keep these limitations explicit when changing lifecycle behavior.

## Packaging

The default installation prefix is `$HOME/.local`; `LIBDIR` overrides the plugin directory. System packages can use a staging root:

```sh
make install PREFIX=/usr DESTDIR=/path/to/package-root
```

Installation replaces the library atomically. It does not reload a running plugin or edit Hyprland configuration. `make uninstall` removes only the installed plugin library, and `make clean` removes build outputs. Do not commit generated libraries or test binaries.

## hyprpm manifest

`hyprpm.toml` pins Hyprland 0.56.2 (`efb50993780079460b0cbed1363e2166a2de1d9f`) to the last compatible plugin commit, `c088b367fe4180f4812fe8a0e23fced8913ccf96`. Hyprland 0.57.0 follows the latest revision of the repository's default branch. Compatibility remains enforced by the build's dependency version checks and the plugin's Hyprland hash checks.

The manifest runs `make clean` before `make DIAGNOSTICS=0` to rebuild against the selected Hyprland headers and avoid accepting a stale shared library after a failed build. No revision placeholders or fabricated commit hashes are used.

The plugin must still be loaded before the first rendered frame. See README.md for the managed library path; starting it only through a late session reload leaves resource tracking incomplete.

## Client transport

`client-transfer.cpp` owns render-pass wrapping, producer tracking and transfer lifecycle. `transfer/` contains the embedded Vulkan worker, synchronization protocol, transport formats and shared-output imports. No migration history or automatic-switching policy gate is required. Suspend transport before snapshotting migratable resources, then restart it after the final GL context is restored, including rollback.

`rendering-optimizations.cpp` owns single-pass HDR rendering, capture pacing and VRR capability correction. It calls the transport module directly. Render-pass observation uses the transport's existing hook, avoiding duplicate hooks on the same compositor function. Texture creation and binding also share the migration module's core hooks, including device-local input protection.

Before migration, stop rendering optimizations before transport; restore transport before rendering optimizations after the final GL context is selected. Per-output state holds weak monitor references. Never retain removed monitors through routing or optimization history.

```sh
make gpu-transfer-test
.build/release/cross-gpu-worker-test .build/release/gpu-transfer-worker /dev/dri/renderD128 /dev/dri/renderD129
.build/release/cross-gpu-worker-test .build/release/gpu-transfer-worker /dev/dri/renderD129 /dev/dri/renderD128
```

Append `copy`, `copy-tiled`, `copy8`, `copy8-tiled` or `copy10-tiled` to test native-format transport. Tiled cases require a source GPU supporting the requested modifier. Tests create offscreen buffers only and verify fences, color/alpha values and GL state restoration. They do not connect to Wayland or perform renderer migration.

`make check` also tests producer provenance and borrowing unique-owned queued states without converting weak pointers to shared ownership. `hyprctl gpu-switch transfers` retains operational status in release builds and adds frame counters in diagnostic builds. Detailed worker output is compiled out by default.

Output routes hold weak monitor references and retire buffers for destroyed or replaced outputs. A route must not extend a monitor's lifetime after the core clears `m_output`: late `wl_output` binds can otherwise reach a live monitor with no backing output. DPMS alone does not invalidate an attached output. The route-lifetime regression test covers these cases without connecting to a compositor.

The rendering-policy test checks every fullscreen safety gate and unique-owned capture-frame lifetime. `make DIAGNOSTICS=1 tools` also builds `frame-cadence-probe.so`, an opt-in event-listener probe for client commits, rendering and presentation cadence. It does not modify rendering and is not loaded automatically. Timing traces and GPU query profiling require a diagnostic build; normal builds retain operational optimization status.

Transport sessions use stable heap addresses keyed by weak surface and window
identities, rather than PID alone (one process can own multiple windows).
Worker IPC, generation caches, output slots and fences belong to each session.
Producer provenance remains shared and validates weak buffer identity before
reusing pointer-indexed records. Sessions with live protected proxies or pending
source-release fences cannot be collected. The NVIDIA native-input feedback
policy currently has one active producer claim; this does not limit ordinary
cross-GPU copy sessions. Migration drains all workers before replacing GL state.
