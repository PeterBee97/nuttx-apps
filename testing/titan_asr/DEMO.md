# Titan Mini local voice camera demo

Build NuttX `titan-mini:asrdemo` with the matching apps branch. Recognition uses
the onboard Ethos-U55 models. Its optional
implementation is controlled by `CONFIG_TESTING_TITAN_DEMO`, default `n`.
The profile starts NSH, automatically registers the GPT partitions, mounts
the first FAT32 partition at `/sdcard` and starts the continuous voice demo.
Media is saved in `/sdcard/media`.

Ethernet obtains an IPv4 address through DHCP. The console prints
`DEMO_FILES url=http://ADDRESS/ root=/sdcard`; browse that address or
`http://ADDRESS/media/` to list and download files. The file service uses
plain HTTP and has no authentication. Stop an active recording before
downloading it so its header and size are final.
The profile sends 4 KiB chunks with 20 ms pauses to protect realtime capture
and inference; measured download speed is about 106 KiB/s. Unpaced downloads
triggered U55 timeouts during concurrent capture.

For the card initialized during the 2026-10-19 session, the driver reports 112,640,000
512-byte sectors (57.67 GB). The GPT FAT32 partition spans sectors 2048
through 112,639,966. This measured capacity differs from the card's 64 GB
label. Its FAT volume has 3,518,216 16 KiB blocks.

The following commands initialize a replacement card and **erase its data**:

```text
titan_asr demo-stop
umount /sdcard
mkgpt write /dev/mmcsd0 :1031168 sdmedia:0
titan_asr demo-mount
mkfatfs -F 32 /dev/mmcsd0p1
titan_asr demo-boot &
```

The first `demo-mount` registers the GPT partition device but reports a FAT
mount error until `mkfatfs` completes. If `/dev/mmcsd0p1` already exists,
reboot immediately after `mkgpt` and run `mkfatfs` and `demo-boot` after boot.
Initialization tools are enabled only in this profile.
Normal boot and `demo-mount` never format storage.

For normal operation and manual restart:

```text
titan_asr demo-status
titan_asr demo-stop
titan_asr demo /sdcard/media &
```

`CONFIG_TESTING_TITAN_DEMO_STORAGE` and
`CONFIG_TITAN_MINI_VOICE_DEMO_AUTOSTART` independently control storage/service
integration and boot startup; both default to `n`.
If no card is present, temporary SDRAM storage can be used:

```text
mkdir /ram
mount -t tmpfs tmpfs /ram
mkdir /ram/media
titan_asr demo /ram/media &
```

Temporary files disappear on reset. Uncompressed video is approximately
750 KiB/s at the default 320 x 240, 5 fps; use a card for long recordings.
The default per-recording limit is 32 MiB. Allocation or storage errors stop
the affected recording, finalize its complete data and indicate an error.
Files use collision-safe 8.3 names: `t0000001.bmp`, `.avi` or `.wav`.

Say the complete phrase **豆包豆包**, pause until the cyan wake indication,
then say **拍照**, **开始录像**, **停止录像**, **开始录音** or **停止录音**.
Each completed wake opens an eight-second window for one subsequent utterance.
Single 豆包, extended wake sentences and bare commands do not authorize an
action. Unknown utterances consume the window. “停止” stops the most recently
started active recording. Companion mode is intentionally unavailable in this
demo because it would bypass the fresh-wake requirement.

The camera saves 24-bit BMP photos and standard single-stream YUY2 AVI files.
AVI contains video only; simultaneous recording produces a separate WAV,
16 kHz, signed 16-bit mono. Recording reuses the recognition frontend PCM
instead of opening a second microphone reader. Camera/file and LED workers
run below microphone capture and decoder priority. Mode states are reconciled
with actual media operations, including failures and automatic stops.
The camera uses the NuttX ring queue so decoder bursts do not exhaust the
capture FIFO and leave the sensor stopped.

The profile enables `CONFIG_RA8P1_I2C_SCHEDLOCK` for the polling IIC driver.
Without it, capture and decoder preemption during camera register reads caused
chip-ID timeouts while live microphones were running. The option keeps
interrupts enabled and protects only the active transfer.

The realtime AEC frontend is allocated before the media queue. With Ethernet
enabled, the reverse order pushed its 64 KiB arena into external SDRAM and
made each 10 ms audio block take about 20 ms, starving the decoder. Allocating
the frontend first keeps the arena in internal SRAM in this profile.

The RGB LED uses the following priority, highest first:

| State | Indication |
| --- | --- |
| Error | Red flashes every 100 ms for two seconds |
| Taking photo | White |
| Wake authorized / listening for command | Cyan |
| VAD speech | Green |
| Video and audio recording | Red / blue alternate every 500 ms |
| Video recording | Red |
| Audio recording | Red flashes every 500 ms |
| Idle | Blue for 200 ms every two seconds |

Before capture starts, storage initialization shows amber. Mount failure
leaves solid red and keeps the voice demo stopped.

Stop waits for workers, closes the camera and finalizes media headers. Avoid
`kill -9` or resetting while recording because those bypass normal file
finalization. `demo-status` is available while the main task is running.

`titan_asr demo DIRECTORY SECONDS MODE` allows a bounded run (0 seconds means
continuous), with 0=raw microphone mean, 1=AEC mean, 2=AEC beam. The default is
2. Raw mode is useful when the onboard speaker intentionally plays test
commands; AEC would suppress those commands as playback echo. The verified
speaker volume remains codec register 0x14 = 0x80.

For integration diagnostics, `titan_asr demo-replay PCM BLOCKS MODE DIRECTORY`
feeds interleaved 16 kHz L/R/reference blocks to the same strict gate and real
media worker. It does not automatically authorize commands. This validates
local inference, gate behavior and actual peripherals, but is not evidence of
live microphone or acoustic double-talk accuracy.

Board validation on 2026-10-19 confirmed all five actions with digital PCM,
including normal voice stops, rejected bare photo, real 320 x 240 BMP,
63-frame AVI and 6.42-second WAV. Onboard-speaker-to-microphone checks also
confirmed wake-controlled photo and audio start/stop after the IIC fix.
With concurrent PDM capture, the final ring queue completed a 62-frame AVI
through normal voice stop without camera timeout.
Acoustic wake confidence is still variable; digital success does not establish
human speech accuracy or default AEC-beam double-talk acceptance.

GPT/FAT validation during the 2026-10-19 session confirmed all five actions
with digital PCM and concurrent live PDM capture. The card retained a
320 x 240 BMP, a 124-frame YUY2 AVI and a 12.87-second WAV after reboot.
HTTP downloads matched their complete sizes and repeated SHA-256 hashes.
A 200.6-second live AEC-beam capture continued through paced downloads
without overruns, inference failures or a queue peak above one.
