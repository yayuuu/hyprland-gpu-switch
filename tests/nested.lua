hl.config({
    xwayland = { enabled = false },
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true },
    animations = { enabled = false },
    decoration = { blur = { enabled = true }, rounding = 10 },
})
hl.plugin.load("/home/daniel/src/hyprland-gpu-switch/hyprland-gpu-switch.so")
