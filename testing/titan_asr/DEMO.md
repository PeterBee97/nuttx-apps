# Titan Mini local voice camera demo

Build NuttX `titan-mini:asrdemo` with the matching apps branch. The demo uses
the onboard Ethos-U55 models and has no network dependency. Its optional
implementation is controlled by `CONFIG_TESTING_TITAN_DEMO`, default `n`.
The profile starts NSH; run the demo explicitly after mounting storage.

Mount writable storage and start:

```text
mkdir /sdcard
mount -t vfat /dev/mmcsd0 /sdcard
titan_asr demo /sdcard &
titan_asr demo-status
titan_asr demo-stop
```

The mount requires an existing formatted card; the demo never formats it.
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
