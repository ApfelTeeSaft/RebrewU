# Standalone Latte shader translator

Adapted from Cemu revision `cc6fea125a5d4f559c662d13c5f4af0b6dd0876f`. The included sources and headers retain their MPL 2.0 notices; see `LICENSE.txt`.

The adapter parses Latte instructions and emits one native Vulkan GLSL interface. Uniform-variable offsets, descriptor bindings, vertex inputs, texture types and stream-out metadata describe the original GX2 program. Host compilation produces SPIR-V through the audited Vulkan compiler profile.

The integration uses standard containers and C++20 formatting, checked translation errors, caller-supplied registers and explicit shader options. It does not contain an emulator renderer, graphics-pack system, guest memory arena or per-game profiles. Unsupported/truncated instructions fail translation.

GPU fixtures exercise shader exports, resource bindings, comparisons, integer bits, mip/slice views, clipping and stream-out. Saved game captures and original reference material remain outside this source directory.
