# Squeezelite - Multichannel PCM & IEC 61937 Bitstream Passthrough Branch

This fork of [Squeezelite](https://github.com/triode/squeezelite) adds native support for high-bitrate multichannel PCM (up to 7.1 channels) and direct IEC 61937 HDMI bitstream passthrough for Dolby Digital Plus Atmos (`E-AC-3`), Dolby TrueHD Atmos, and DTS-HD Master Audio / Auro-3D. It operates seamlessly with Logitech Media Server (LMS) and the `lms-multichannel-audio` plugin. It has been tested only on a NUC 11 i3 machine, with a LibreELEC installation.

---

## Key Features

- **IEC 61937 Bitstream Passthrough:** Direct encapsulation of E-AC-3, TrueHD, and DTS-HD MA streams via FFmpeg's `spdif` muxer (`spdifenc`).
- **Persistent Muxer Architecture:** Keeps the FFmpeg `spdif` context active across natural track boundaries to eliminate AVR re-lock delay and pops during track transitions.
- **Direct TrueHD Access-Unit (AU) Framing:** Custom framing bypasses FFmpeg `mlp_parser` resync drops on track boundaries, enabling gapless TrueHD Atmos playback.
- **Automatic HDMI Channel Status Control:** Sets ALSA `AES0=0x06` (IEC 60958 Consumer / Non-Audio) automatically when outputting raw bitstreams, forcing HDMI receivers directly into bitstream decoding mode.
- **Multichannel LPCM Routing:** Supports discrete 5.1 and 7.1 LPCM output over 8-channel High-Bit-Rate (HBR) ALSA interfaces.
- **Comprehensive Codec Support:** Includes standard stereo and hi-res formats: FLAC, ALAC, DSD, PCM, Ogg Vorbis, Opus, WMA, MP3 (`libmad`/`libmpg123`), and AAC (`libfaad`).

---

## Architectural Modifications

| Source File | Functionality & Changes |
| :--- | :--- |
| `slimproto.c` | Registers extended opcodes (`e` for E-AC-3, `t` for TrueHD, `h` for DTS-HD MA) and handles 6ch/8ch stream allocation. |
| `output.c` | Manages multichannel buffer sizing, channel mapping, and bitstream mode flag transitions. |
| `output_alsa.c` | Dynamically updates ALSA channel status controls (`AES0=0x06`) for non-PCM bitstreams and handles HBR 8-channel HDMI layout. |
| `ffmpeg.c` | Implements persistent `spdif` stream encapsulation, major-sync alignment on track skips, and direct streambuf TrueHD AU extraction. |
| `squeezelite.h` | Header declarations for multichannel frame buffers, bitstream flags, and codec state structs. |

---

## Building

### Standard Linux Build

Ensure dependencies are installed (`libalsa-dev`, `libffmpeg-dev`, `libflac-dev`, `libmad0-dev` or `libmpg123-dev`, `libfaad-dev`):

```bash
make clean
make
```

### Build Flags

Standard Squeezelite Makefile flags apply:
- `-DFFMPEG`: Enables FFmpeg decoding and bitstream `spdif` encapsulation.
- `-DALSA`: Enables ALSA hardware output driver with HDMI `AES0` controls.
- `-DMAD` / `-DFAAD`: Enable MP3 and AAC decoding libraries.

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
