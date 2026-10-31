# GPU migration development

- Target stock Hyprland. Do not rebuild or patch the compositor to implement this plugin.
- Keep the project focused exclusively on GPU migration.
- Read README.md and CONTRIBUTING.md before building or testing.
- Place diagnostic-only sources in diagnostics/. Keep diagnostics behind GPU_SWITCH_DIAGNOSTICS, disabled by default. The default make target must build only the normal plugin.
- Respect test authorization already provided in the current conversation. Never log the user out or restart their desktop session automatically.
- Keep test sources in tests/ and diagnostic-only implementation/tools in diagnostics/.
- Use native-test and transfer-test for buffer tests. Their NULL backend has no display outputs or Wayland connection. Do not treat nested Wayland as an isolation boundary.
- Load the plugin before rendering starts to obtain tracking_complete=true. Automatic switching is enabled by default. Use hyprctl gpu-switch disable to turn it off for the current session.
- Read the current instance signature with hyprctl instances -j. Do not assume that a numeric instance index remains valid after a session restart.
- The plugin uses RTLD_NODELETE to keep buffer callbacks valid. Replacing a library at the same path does not replace its code in the running process. Use a distinct test library path or a new login session when appropriate.
- Do not unload the migration plugin after it has migrated live GPU resources. Disable automatic switching instead.
- Preserve advertised format/modifier checks and synchronization when changing transfer paths. Do not force unsupported buffer imports.
- Follow the workspace command-logging instructions. In this workspace, run diagnostic and operational commands through /home/daniel/.local/bin/codex-diagnostic. Do not log source or file-content edits.
