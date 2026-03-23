# Rebrew — Splatoon Wii U PC Port

This directory contains the PC port runtime for the Rebrew static-recompilation
pipeline.  The recompiler translates the game's PowerPC 750CL binary into a
compilable C++ program; this runtime provides the Wii U OS, GX2 graphics, input,
and audio interfaces that the translated code calls into.

> **Legal notice:** You must own a legal copy of the game.  Redistribution of
> game assets, the original RPX binary, or any extracted content is prohibited.
> All references to file extraction below assume you already own the title.

---

## Supported Game Version

| Field | Value |
|---|---|
| Title | Splatoon (WUP-AAZP / WUP-AAZE / regional equivalents) |
| Version | **v1.0 (launch)** — no update applied |
| RPX SHA-256 | `45ecd1ed81a97f46b1c8ae24820cd47fbc55399b50af09bf1f9050fe803308e5` |

Only the v1.0 RPX has been tested against this port.  Later updates change code
addresses that the recompiler and the runtime frame-pacer override rely on.  If
your RPX hash does not match, results are undefined.

### Verifying your RPX

```bash
# Linux / macOS
sha256sum Gambit.rpx

# macOS (if sha256sum is not available)
shasum -a 256 Gambit.rpx

# Windows PowerShell
Get-FileHash Gambit.rpx -Algorithm SHA256
```

The output should start with `45ecd1ed81a97f46b1c8ae24820cd47fbc55399b50af09bf1f9050fe803308e5`.

---

## System Requirements

| Component | Minimum |
|---|---|
| CPU | x86-64, 4+ cores recommended |
| RAM | 2 GB free (768 MB guest arena + host overhead) |
| GPU | OpenGL 4.1 core profile (most GPUs from 2011+) |
| OS | Linux, Windows 10+, or macOS 11+ |
| Disk | ~2 GB for game content |

---

## Building

### Prerequisites

The project uses **CMake 3.18+** and a **C++20** compiler.  The following
libraries must be available to CMake's `find_package`:

| Library | Purpose |
|---|---|
| SDL2 | Window, OpenGL context, input |
| OpenGL | Rendering back-end |
| libcurl | Network stubs |
| zlib | Compression stubs |

Optionally, place `miniaudio.h` (single-header, MIT) in `third_party/` to
enable the audio back-end:

```bash
wget https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h \
     -O third_party/miniaudio.h
```

---

### Linux (GCC / Clang)

**Ubuntu / Debian:**

```bash
sudo apt update
sudo apt install -y \
    cmake build-essential \
    libsdl2-dev \
    libcurl4-openssl-dev \
    zlib1g-dev \
    libgl-dev
```

**Fedora / RHEL:**

```bash
sudo dnf install -y \
    cmake gcc-c++ \
    SDL2-devel \
    libcurl-devel \
    zlib-devel \
    mesa-libGL-devel
```

**Arch Linux:**

```bash
sudo pacman -S cmake sdl2 curl zlib mesa
```

**Configure and build:**

```bash
# From the repo root
cmake -S port -B build \
      -DCMAKE_BUILD_TYPE=Release

# Parallel build — use all available cores
cmake --build build --parallel
```

The output binary is `build/Gambit`.

---

### Windows — MSVC (Visual Studio 2022)

**1. Install dependencies via vcpkg:**

```powershell
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg install sdl2:x64-windows curl:x64-windows zlib:x64-windows opengl:x64-windows
```

**2. Configure and build:**

```powershell
cmake -S port -B build `
      -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_TOOLCHAIN_FILE="<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake" `
      -DVCPKG_TARGET_TRIPLET=x64-windows

cmake --build build --config Release
```

The output binary is `build\Release\Gambit.exe`.

**3. Runtime DLLs:**
Copy `SDL2.dll` and `libcurl.dll` from the vcpkg installed tree next to
`Gambit.exe`, or add the vcpkg `bin` directory to `PATH`.

---

### Windows — MinGW-w64 (MSYS2)

```bash
# Install MSYS2 from https://www.msys2.org, then in the MINGW64 shell:
pacman -S --needed \
    mingw-w64-x86_64-cmake \
    mingw-w64-x86_64-gcc \
    mingw-w64-x86_64-SDL2 \
    mingw-w64-x86_64-curl \
    mingw-w64-x86_64-zlib

cmake -S port -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -G "Ninja"

cmake --build build --parallel
```

---

### macOS

**Homebrew:**

```bash
brew install cmake sdl2 curl zlib
```

**Configure and build:**

```bash
cmake -S port -B build \
      -DCMAKE_BUILD_TYPE=Release

cmake --build build --parallel
```

> **Note:** macOS deprecated OpenGL in 10.14.  The port uses OpenGL 4.1 core,
> which works on all Macs with AMD or Nvidia GPUs.  Apple Silicon (M-series)
> requires Rosetta 2 or a MoltenVK/ANGLE bridge — native Metal support is not
> yet implemented.

---

## Game Assets

The game reads all assets through the Wii U filesystem API (`FSOpenFile` under
`/vol/content/`).  The runtime remaps these paths to `./content/` relative to
the `Gambit` executable.

```
Gambit              ← compiled binary
content/            ← /vol/content  (game data)
  *.pack
  *.bfres
  sound/
  …
meta/               ← optional, metadata only
  meta.xml
  iconTex.tga
```

If an asset is missing you will see:

```
[gambit-fs] FSOpenFile: NOT_FOUND: ./content/some/file.pack
```

Set `RBREW_FS_VERBOSE=1` in the environment to log every successful open.

---

### Extraction from a Wii U dump

> Only extract content you legally own.

#### Option A — Dumpling (recommended, runs on console)

1. Install **[Dumpling](https://github.com/emiyl/dumpling)** homebrew on your Wii U.
2. Insert a FAT32 USB drive.
3. Open Dumpling, choose **Dump game** → select Splatoon.
4. On your PC, copy `<dump>/content/` next to the `Gambit` executable.

#### Option B — NUSPatcher / CDecrypt (eShop / CDN)

1. Obtain your console's device certificate and the game's encrypted ticket.
2. Download the NUS package with **[NUSPatcher](https://github.com/nicoboss/NUSPatcher)**.
3. Decrypt with **[CDecrypt](https://github.com/VitaSmith/cdecrypt)**:
   ```bash
   cdecrypt <title.tmd> <encrypted-content-dir>
   ```
4. The decrypted output contains a `content/` folder — copy it next to `Gambit`.

#### Option C — Cemu MLC directory (if you already use Cemu)

Cemu stores installed titles under:

```
mlc01/usr/title/<titleHi>/<titleLo>/
```

The `content/` subfolder inside that directory is already decrypted.  Copy it
next to `Gambit`.

#### Option D — Physical disc with wudd / disc2app

1. Dump the disc to a `.wud` or `.wux` image using
   **[wudd](https://github.com/wiiu-env/wudd)** on a hacked Wii U.
2. Extract the `content/` partition from the image using
   **[disc2app](https://github.com/koolkdev/disc2app)** or a compatible tool.
3. Copy the `content/` folder next to `Gambit`.

---

## Controls

| Action | Binding |
|---|---|
| Move | Left stick (gamepad) |
| Camera | Mouse (TV View) or right stick |
| Jump | A button |
| Special / shoot | Right trigger |
| Switch view | **Tab** — toggles between TV View and Gamepad View |
| Touch input | Mouse click in **Gamepad View** (simulates GamePad touchscreen) |
| Quit | Close window |

Mouse is captured in TV View for camera control.  Press **Tab** to release the
cursor and interact with the touchscreen (map, weapon select, etc.).

---

## Troubleshooting

**Black window / nothing renders**
The stub GLSL shader is active.  Full shader translation 
will enable visible rendering.  This is expected at the current stage.

**`[gambit] Failed to allocate … MB guest arena`**
Your system does not have enough contiguous virtual memory.  Close other
applications and retry.  On Windows, ensure DEP/CFG do not block large
allocations from non-executable regions.

**`[gx2_render] WARNING: could not load glXxx`**
Your GPU driver does not expose an OpenGL 4.1 core context.  Update your
drivers.  On Linux with Intel integrated graphics, install `mesa` 21.0 or later.

**`[gambit-fs] FSOpenFile: NOT_FOUND`**
The `content/` directory is missing or in the wrong location relative to the
`Gambit` executable.  See **Game Assets** above.

**Game crashes immediately with `OSPanic ind_sgnl.c:105`**
This was fixed in the dimport repatch.  If you see it again, ensure
you are building from the latest source and not mixing old object files.

---

## Project Status

| Description | Status |
|---|---|
| Recompiler pipeline | ✅ Complete |
| Boot infrastructure | ✅ Complete |
| Asset handling + dual-screen UI | ✅ Complete |
| GX2 display infrastructure (FBOs, textures, raster state) | ✅ Complete |
| Draw call infrastructure (VBO/VAO, fetch shader, byte-swap) | ✅ Complete |
| AMD R700 → GLSL shader translation | 🔄 In progress |
| Audio (AX voice pool + miniaudio mixing) | 🔄 In progress |
| Real threading (std::thread mapping) | ⏳ Planned |
| Save data (nn_save → host filesystem) | ⏳ Planned |
| Networking (TCP/IP) | ⏳ Planned |
| Full compatibility pass | ⏳ Planned |
