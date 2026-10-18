# Recompiler and native runtime

This folder holds all of the port's source code. Players don't need to touch it: run `Setup.bat` in the repository root and it builds everything here automatically. See the main [README](../README.md).

| Folder | Contents |
|---|---|
| `src`, `include`, `runtime` | **RebrewU**, the static recompiler. It reads a Wii U `.rpx` and writes C++. |
| `port` | The native Windows runtime: Cafe OS services, the GX2→Vulkan renderer, audio and input. |
| `port/third_party/cemu_shader` | The Latte shader decompiler adapted from Cemu (MPL-2.0, see its `LICENSE.txt`). |
| `tools/build_vulkan_dependencies.py` | Builds the pinned SDL 2.32.10 and glslang 16.4.0 profiles. |

`scripts/Setup.ps1` shows the exact build steps.

The C++ that the recompiler generates is derived from your copy of the game. Keep it private and never commit it.
