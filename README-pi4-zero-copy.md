# Raspberry Pi 4: zero-copy 4K video (branch `pi4-zero-copy`)

Smooth 4K HEVC (8- and 10-bit, including HDR10 sources) on a Raspberry Pi 4
running Android 15 (raspberry-vanilla / KonstaKANG LineageOS 22.2).

The stock codec2 FFmpeg service copies every hardware-decoded picture from the
decoder's SAND buffer into a YV12 buffer on the CPU. At 4K 10-bit that copy
takes most of the Pi's four cores, which shows up as dropped frames, a slow UI
and HDMI audio underruns. This branch hands the decoder's buffer straight to
the display hardware instead.

## Repositories

All four forks have a `pi4-zero-copy` branch on top of the raspberry-vanilla
commit they were tested against.

| Fork | Base | Changes |
|---|---|---|
| [android_external_ffmpeg_codec2](https://github.com/ayooooo123/android_external_ffmpeg_codec2/tree/pi4-zero-copy) | `61a9d9e` | faster copy path and error handling; zero-copy output |
| [android_external_ffmpeg](https://github.com/ayooooo123/android_external_ffmpeg/tree/pi4-zero-copy) | `48fcbd4` (android-15.0-7.1) | SAND copy fixes, banded multi-threaded copy, helper priority; HEVC decode-queue fix on allocation failure |
| [android_external_minigbm](https://github.com/ayooooo123/android_external_minigbm/tree/pi4-zero-copy) | `f1cf1d0` | import of 10-bit P030 buffers |
| [android_device_brcm_rpi4](https://github.com/ayooooo123/android_device_brcm_rpi4/tree/pi4-zero-copy) | `2f96272` | TV build: 1080p SurfaceFlinger client target (saves ~49 MB of CMA at 4K) |

To build with them, copy [`pi4-zero-copy.xml`](pi4-zero-copy.xml) into
`.repo/local_manifests/` and run `repo sync` for the four paths.

## How zero-copy works

When the app renders to a SurfaceView, the decoder buffer is wrapped as a
minigbm gralloc handle with the Broadcom SAND128 modifier, attached to the
app's BufferQueue by the codec service, and scanned out by the vc4 display
planes. A picture goes back to the decoder only after the display's release
fence has signalled. Details are in the commit message of the zero-copy
commit.

Whenever zero-copy can't apply (ByteBuffer mode, TextureView or ImageReader,
non-SAND output, or a failed import), the copy path is used automatically.

## Switches and limits

- `setprop persist.vendor.ffmpeg_codec2.zerocopy 0` turns zero-copy off, then
  restart the codec service.
- PQ/HLG are output as SDR on this path: the GPU can't read SAND buffers, and
  an HDR dataspace on a non-HDR display makes SurfaceFlinger compose on the
  GPU.
- Screenshots and screen recordings of the video come out black, for the same
  reason.
- One 4K decoder at a time fits in the default 512 MB CMA.
- Logs: `logcat | grep ZeroCopy` shows the mode and a once-per-second line of
  buffer counters.

## Results (Pi 4, 3840x2160 display)

| | Copy path | Zero-copy |
|---|---|---|
| 2160p HDR10 episode in ExoPlayer: frames dropped | ~300 at start, bursts while playing | 0 |
| Codec service CPU | 250-310% | ~18% |
| HDMI audio underruns | up to 21 in 2 min | 0 |
| Test player, 8/10-bit 4K, 30 s with 5 seeks | | every frame on time |
