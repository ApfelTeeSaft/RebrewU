# Native Vulkan runtime

`Gambit.rpx` is recompiled into native C++, linked with the Cafe OS runtime and rendered through Vulkan. SDL supplies static Windows window/input support. The shader compiler accepts Vulkan GLSL and emits SPIR-V.

Configure this directory with the existing generated guest checkpoint and the audited dependency profiles. `RBREW_GRAPHICS_BACKEND` accepts only `Vulkan`. Required Vulkan features are checked by `os/gx2/vk/gx2_vk.cpp`.

`gambit_runtime` is the host library. `Gambit` is the graphical release executable. `gx2_replay_quiet` validates saved draws using the same silent runtime. Renderer-free ABI tests use a separate headless implementation.

For host changes, build `gambit_runtime` and the replay target incrementally, then relink the game with unchanged generated objects. Do not regenerate original game code for a host-only renderer change. The complete local manual contains dependency commands, exact build identities, runtime layout and verification limits.
