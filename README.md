# Squeezelite - Multichannel PCM & IEC 61937 Bitstream Passthrough Branch

This fork of [Squeezelite](https://github.com/triode/squeezelite) adds native support for high-bitrate multichannel PCM (up to 7.1 channels) and direct IEC 61937 HDMI bitstream passthrough for Dolby Digital Plus Atmos (`E-AC-3`), Dolby TrueHD Atmos, and DTS-HD Master Audio / Auro-3D. It operates seamlessly with Logitech Media Server (LMS) and the `lms-multichannel-audio` plugin. It has been tested only on a NUC 11 i3 machine, with a LibreELEC installation.

---

## Key Features

- **IEC 61937 Bitstream Passthrough:** Direct encapsulation of E-AC-3, TrueHD, and DTS-HD MA streams via FFmpeg's `spdif` muxer (`spdifenc`).
- **Persistent Muxer Architecture:** Keeps the FFmpeg `spdif` context active across natural track boundaries to eliminate AVR re-lock delay and pops during track transitions.
- **Direct TrueHD & E-AC-3 Gapless Framing:** Custom raw frame extraction from `streambuf` bypasses `libavformat` / `mlp_parser` teardown and resync drops on chapter and track boundaries, enabling gapless TrueHD Atmos and E-AC-3 Atmos playback.
- **Zero-Delay Gapless Track Transitions:** Continues writing audio immediately across `track_start` boundaries when no rate, channel, or format change delay is required, preventing 10 ms period stalls.
- **Automatic HDMI Channel Status Control:** Sets ALSA `AES0=0x06` (IEC 60958 Consumer / Non-Audio) automatically when outputting raw bitstreams, forcing HDMI receivers directly into bitstream decoding mode.
- **Multichannel LPCM Routing:** Supports discrete 5.1 and 7.1 LPCM output over 8-channel High-Bit-Rate (HBR) ALSA interfaces.
- **Comprehensive Codec Support:** Includes standard stereo and hi-res formats: FLAC, ALAC, DSD, PCM, Ogg Vorbis, Opus, WMA, MP3 (`libmad`/`libmpg123`), and AAC (`libfaad`).

---

## Architectural Modifications

| Source File | Functionality & Changes |
| :--- | :--- |
| `slimproto.c` | Registers extended opcodes (`e` for E-AC-3, `t` for TrueHD, `h` for DTS-HD MA) and handles 6ch/8ch stream allocation. |
| `output.c` | Manages multichannel buffer sizing, channel mapping, bitstream mode flag transitions, and zero-delay gapless `track_start` continuation. |
| `output_alsa.c` | Dynamically updates ALSA channel status controls (`AES0=0x06`) for non-PCM bitstreams and handles HBR 8-channel HDMI layout. |
| `ffmpeg.c` | Implements persistent `spdif` stream encapsulation, major-sync alignment on track skips, and direct `streambuf` TrueHD AU and raw E-AC-3 (`0x0B77`) frame extraction. |
| `squeezelite.h` | Header declarations for multichannel frame buffers, bitstream flags, and codec state structs. |

---

## Building

### Standard Linux Build

Ensure dependencies are installed (`libasound2-dev`, `libavcodec-dev`, `libavformat-dev`, `libavutil-dev`, `libflac-dev`, `libsoxr-dev`, `libmad0-dev` or `libmpg123-dev`, `libfaad-dev`):

```bash
make clean
make OPTS="-DFFMPEG -DRESAMPLE"
```

### LibreELEC Build (Static Codecs + Dynamic FFmpeg)

LibreELEC ships FFmpeg shared libraries (`libavcodec`, `libavformat`, `libavutil`) but omits several standalone codec libraries such as `libfaad.so.2`. `Makefile.libreelec` builds a self-contained x86_64 binary that statically links FLAC, Vorbis, Opus, Ogg, MAD, `libsoxr`, and `libfaad` (including native client-side AAC decoding via `faad.c`) while keeping `ffmpeg.o` in `dlopen()` mode to use LibreELEC's system FFmpeg.

1. Install static codec packages on the build host (Debian/Ubuntu/Mint):
   ```bash
   sudo apt install libasound2-dev libflac-dev libvorbis-dev libopus-dev libopusfile-dev libogg-dev libmad0-dev cmake
   ```

2. Build static `libsoxr.a` and `libfaad.a` alongside the repository (or point `SOXR_DIR` / `FAAD_DIR` to existing prefixes):
   ```bash
   # Static libfaad2
   git clone --depth 1 https://github.com/knik0/faad2.git ../faad2-src
   cmake -S ../faad2-src -B ../faad2-src/build \
     -DCMAKE_BUILD_TYPE=Release \
     -DBUILD_SHARED_LIBS=OFF \
     -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
     -DCMAKE_INSTALL_PREFIX=../faad2-static
   cmake --build ../faad2-src/build -j$(nproc)
   cmake --install ../faad2-src/build
   ```

3. Build `squeezelite` using `Makefile.libreelec` (override `FFMPEG_INC`, `SOXR_DIR`, `FAAD_DIR`, or `LIBDIR` as needed):
   ```bash
   make -f Makefile.libreelec clean
   make -f Makefile.libreelec FFMPEG_INC=../ffmpeg-master
   ```

### Build Flags

Standard Squeezelite Makefile flags apply:
- `-DFFMPEG`: Enables FFmpeg decoding and bitstream `spdif` encapsulation.
- `-DALSA`: Enables ALSA hardware output driver with HDMI `AES0` controls.
- `-DRESAMPLE`: Enables client-side SoX resampling (`libsoxr`) for `-R` / `-u` flags (e.g., locked sample-rate pipelines such as CamillaDSP).
- `-DMAD` / `-DFAAD`: Enable MP3 and AAC decoding libraries.
- `-DLINKALL`: Statically links codec libraries instead of loading them via `dlopen()`.

---

## Command Line Usage Notes

When outputting bitstreams or multichannel PCM over HDMI, target the raw hardware device (`hw:X,Y` or `hdmi:CARD=X,DEV=Y`) to bypass ALSA software mixing (`dmix`):

```bash
# Example: List available ALSA output devices
squeezelite -L

# Example: Run squeezelite targeting HDMI output with direct hardware passthrough
squeezelite -o hw:0,1 -b 4096:8192
```

---

## License

This software is released under the GNU General Public License v3 (GPLv3).
