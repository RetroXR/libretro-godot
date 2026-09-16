# libretro-godot

A GDExtension (C++) that runs libretro emulator cores inside Godot 4.5+. Bridges Godot's scene system with the libretro API, enabling retro game emulation within Godot projects.

Originally forked from [Skurdt/SK.Libretro.Godot](https://github.com/Skurdt/SK.Libretro.Godot).

## Prerequisites

- **SCons**: build system
- **MSVC** (Windows), **GCC/Clang** (Linux), **Xcode command-line tools** (macOS),
  or **Android NDK r27d+** (Android/Quest)
- **Python 3**: required by SCons
- **Vulkan loader, SDL3, OpenGL** (Linux): linked by soname at build time
  (`libvulkan.so.1`, `libSDL3.so.0`, `libGL.so.1`); these ship with the distro's Vulkan
  runtime, SDL3 package, and Mesa. The `-dev`/`-devel` packages are not required.

## Setup

```bash
git clone --recursive https://github.com/RetroXR/libretro-godot.git
cd libretro-godot
```

If you already cloned without `--recursive`:

```bash
git submodule update --init --recursive
```

## Building

### Windows (x86_64)

```powershell
scons platform=windows arch=x86_64 target=template_debug dev_build=yes
scons platform=windows arch=x86_64 target=template_release
```

### Linux (x86_64)

```bash
scons platform=linux arch=x86_64 target=template_debug
scons platform=linux arch=x86_64 target=template_release
```

Links the Vulkan loader, SDL3, and the GL loader by soname (`libvulkan.so.1`,
`libSDL3.so.0`, `libGL.so.1`), so no `-dev`/`-devel` packages are needed. Software,
OpenGL, and Vulkan hardware-render cores all work. OpenGL uses SDL3 to create a hidden
GL context (the same code path as Windows), so a display server (X11/Wayland) must be
available when a GL core runs; headless GL init fails gracefully.

### Android / Quest (arm64)

Requires `ANDROID_NDK_ROOT` to be set:

```bash
ANDROID_NDK_ROOT="/path/to/android-ndk-r27d" ANDROID_HOME="" \
  scons platform=android arch=arm64 target=template_debug
```

The Android build defines both `HAVE_NEON` and `__ARM_NEON__`. libretro-common keys
every NEON path off `__ARM_NEON__` (the ARMv7-era spelling), but clang for aarch64
defines `__ARM_NEON` without the trailing underscores, so all of them silently compile
out and the resampler, pixel conversion and s16→float conversion fall back to scalar C.

Both defines are needed and they do different jobs: `HAVE_NEON` compiles the NEON code
in, `__ARM_NEON__` makes `features_cpu.c` set `RETRO_SIMD_NEON`, which is what the
resampler's runtime dispatch actually tests. With only `HAVE_NEON` the code is present
but never selected: the CPU mask reports `ASIMD`, the dispatch asks for `NEON`, and the
branch body is empty after preprocessing. Measured on a Quest 3 resampling 32040 → 44100
Hz: scalar 33.5 µs/batch, `HAVE_NEON` alone 33.1 µs (i.e. unchanged), both defines
20.7 µs.

Do **not** define `HAVE_ARM_NEON_ASM_OPTIMIZATIONS`. That selects
`sinc_resampler_neon.S`, which is ARMv7 assembly and cannot assemble for arm64.

### macOS (arm64 or x86_64)

```bash
scons platform=macos arch=arm64 target=template_debug macos_deployment_target=13.0
scons platform=macos arch=x86_64 target=template_release macos_deployment_target=13.0
```

Vulkan hardware rendering runs through the MoltenVK copy already embedded in
Godot's macOS executable. The extension creates a fixed-size `CAMetalLayer` for
cores that require a surface and uses the same Vulkan image readback path as the
other platforms. The pinned official universal MoltenVK dylib is retained in
`external/MoltenVK/` for provenance and standalone diagnostics, but is not loaded
into Godot: loading a second MoltenVK runtime duplicates Objective-C classes and
can crash the process.

### Output

By default, compiled libraries are placed in `bin/`.

To override the output directory, set the `LIBRETRO_GODOT_OUTPUT_DIR` environment variable:

```bash
# Output to a Godot project directory
LIBRETRO_GODOT_OUTPUT_DIR="../MyProject/addons/libretro_godot" scons platform=windows arch=x86_64 target=template_debug
```

## Audio

Core audio has two output paths, chosen at `StartContent` time.

**Default: Godot's own 3D audio.** `AudioHandler` creates an `AudioStreamGenerator`
at the core's declared sample rate and feeds an `AudioStreamPlayer3D` parented to the
`Libretro` node. Nothing extra is required; this is what runs unless the optional
extension below is present.

**Optional: Meta XR Audio (HRTF).** If the host project also has the
`metaxr-audio` GDExtension loaded *and* its native library initialized, core audio is
routed through two spatialized voices instead, giving real HRTF placement rather than
amplitude panning. The dependency is one-way and soft: resolved by name through
`Engine.get_singleton("MetaXRAudio")`, never linked, so this extension builds and runs
identically without it. On Linux it is always the default path, since Meta ships no
Linux binary.

### Voice ids

A *voice* is one spatialized mono source slot in the Meta XR Audio mixer. When the SDK
path is active, `AudioHandler` claims two of them (a console's sound comes out of a TV,
and a TV has two speakers) and pushes the core's left and right channels into them
separately.

`Libretro.GetAudioVoiceIds()` exposes those slot indices to GDScript as a
`PackedInt32Array`:

```gdscript
var voices: PackedInt32Array = libretro.GetAudioVoiceIds()
if voices.is_empty():
    # fallback path: position the AudioStreamPlayer3D child instead
else:
    # SDK path: place each voice in 3D, e.g. at the TV's speaker positions
    mx.set_voice_position(voices[0], origin - right * separation)
    mx.set_voice_position(voices[1], origin + right * separation)
```

**An empty array means the fallback path is in use**, and that is the check callers
should branch on. The C++ side owns the voices' lifetime (created in `AudioHandler::Init`,
released in `DeInit`); GDScript only positions them and sets their gain.

### Sample rates and pacing

Cores declare their own rate (32040 Hz SNES, 44100 PSX, 48000 N64) while the SDK context
runs at the device mix rate, so anything that does not match is resampled with
libretro-common's sinc resampler (on the emulation thread, never the audio thread). Note
a Quest reports **44100 Hz** even when the project requests 48000, so a 48000 Hz core is
resampled there although it passes straight through on desktop.

Emulation pacing is gated on audio buffer occupancy: `IsBufferSaturated()` throttles
`retro_run()` so a core that advances more than one display frame per call cannot run
fast. The voice ring is much larger than the `AudioStreamGenerator` it replaces, so
`EffectiveTotalFrames()` deliberately reports the *old* buffer capacity on the SDK path
and treats the rest of the ring as spare headroom; pacing on the physical ring size
would let a core run most of a second ahead.

## Using as a Submodule

When embedding this repo as a submodule in a Godot project, the parent repo's `SConstruct` can override the output directory directly:

```python
# Parent SConstruct example
VariantDir('libretro-godot/Temp', 'libretro-godot', duplicate=0)
env = Environment()
output_dir = '#path/to/godot/project/libretro-godot'

SConscript('libretro-godot/Temp/SConscript', exports=['env', 'output_dir'])
```

The `#` prefix makes the path relative to the parent project root.

## Supported Platforms

All platforms also support software-rendered cores (the common case). The table below
covers **hardware**-render support:

| Platform | Arch | Compiler | HW Rendering |
|----------|------|----------|--------------|
| Windows | x86_64 | MSVC (C++latest) | Vulkan + OpenGL (SDL3) |
| Linux | x86_64 | GCC/Clang (C++20) | Vulkan + OpenGL (SDL3) |
| Android | arm64 | Clang/NDK (C++20) | Vulkan + OpenGL ES 3.0 (EGL) |
| macOS | arm64 / x86_64 | Apple Clang (C++20) | Vulkan (MoltenVK/Metal) |

## Dependencies

All dependencies are included in `external/` or as submodules:

- **godot-cpp** (submodule, 4.5 branch): Godot C++ bindings
- **SDL3**: DLL loading and HW render context on Windows
- **MoltenVK**: Vulkan-on-Metal runtime embedded by Godot on macOS; the pinned
  upstream universal dylib and Apache-2.0 licence are under `external/MoltenVK/`
- **libretro-common**: VFS, audio conversion, pixel format conversion, and the sinc
  audio resampler (core rate → device mix rate)
- **moodycamel::ReaderWriterQueue**: Lock-free SPSC queue for cross-thread communication

## License

MIT; see [LICENSE](LICENSE). Copyright (c) 2026 Ryan McClelland (XenuIsWatching),
and (c) 2025 Skurdt for the SK.Libretro.Godot code this was forked from, whose notice
is retained as the license requires.

Bundled dependencies keep their own licenses: SDL3 (zlib, `external/SDL3/LICENSE.txt`),
MoltenVK (Apache-2.0, `external/MoltenVK/LICENSE`),
godot-cpp (MIT), vulkan-headers (Apache-2.0 OR MIT, see its `LICENSES/`), and
libretro-common, which carries no top-level license file; each source file states its
own terms in its header (MIT, "The following license statement only applies to this
file"), so consult the specific files listed in `Temp/SConscript`.
