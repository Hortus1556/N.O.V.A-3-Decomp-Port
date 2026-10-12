# Audio

## How the game makes sound

The Vox engine mixes everything in software (`vox::DriverCallbackInterface::FillBuffer`) and
hands the PCM to one of two Android backends in `vox::DriverAndroid`:

- **OpenSL ES** when its Android API level is above 8;
- otherwise Java's **`android.media.AudioTrack`** through JNI, fed by Vox's own thread
  (`UpdateThreadedAT`).

The API level is a Vox-only variable that `JNI_OnLoad` sets through `VoxSetAndroidAPILevel`.
The host sets it to 8 right after `JNI_OnLoad` (`audio::select_backend`), so the AudioTrack
backend is used, and `host/audio.cpp` implements the few `AudioTrack` methods it calls on top
of SDL3 audio streams.

## The AudioTrack emulation

| Method | Implementation |
|---|---|
| `getNativeOutputSampleRate` | the default output device's rate (without it the game got 0 Hz) |
| `getMinBufferSize` | 40 ms, what Vox sizes its ~32 ms mix chunks from |
| constructor | opens an SDL audio stream in the game's format (SDL resamples) |
| `play` / `pause` / `stop` / `release` / `getPlayState` | device resume / pause and state (the device plays silence when the queue is empty) |
| `write(byte[] or short[], offset, count)` | queues the PCM, blocking while the queue is full (96 ms PC / 128 ms Switch / 80 ms 3DS: with the DSP's 47 ms of buffers, 128 was heard as late sound), as Android's MODE_STREAM write does |

`[audio] volume` is the stream gain. Scripted test runs use SDL's silent `dummy` driver
(`NOVA3_REAL_AUDIO=1` keeps the real device).

## Clicks

Sound first played with occasional clicks. Vox writes a chunk about every 32 ms; whenever its
thread was late, the device queue ran dry. Counted in a test match: 72 underruns. The device
now only starts, or restarts after running dry, once a cushion is queued. Underruns are logged
(`audio: underrun N`).

## Dropouts

The sound still cut out now and then, at random moments. Vox's mixer thread
(`vox::DriverAndroid::UpdateThreadedAT` / `DoCallbackAT`) keeps its own clock: after each
32 ms chunk it sleeps 32 ms (`m_updateTime`), and writes chunks back to back only once it is
behind its clock by more than `m_dataThreshold`. `_InitAT` sets that to
−`m_dataThresholdRatio` × the buffer `getMinBufferSize` gave (40 ms); the ratio is 1.0. So the
mixer drifted up to 40 ms behind before catching up, and the queue sank from the 48 ms cushion
to ~20 ms (measured at each write over a level: never more). Any hiccup longer than that
(the mixer thread waiting behind the busy game threads, the OS scheduler) emptied it, and the
device stopped until a new cushion was queued: a short cut and restart.

That cushion-and-restart scheme (the ratio set to 0, a 64 ms cushion, the device paused when
the queue ran dry) still broke up, more so on slow machines (the Switch). An audit of Vox's
driver showed why:

- `_InitAT` (0x8024e0): chunk = min(`getMinBufferSize` / 4, 32 ms) frames, `m_updateTime`
  = the chunk's duration, `m_dataThreshold` = −ratio × the minimum buffer's duration;
- `UpdateThreadedAT` (0x804748): `new AudioTrack(MUSIC, rate, stereo, PCM16, minbuf,
  MODE_STREAM)`, `play()` before the first write, then `DoCallbackAT` in a loop;
- `DoCallbackAT` (0x803e08): mixes one chunk under the driver's mutex, releases it, then
  `write()` (result ignored), and `usleep(m_updateTime)` whenever it is even slightly ahead of
  its `gettimeofday` clock. So what it has written swings by a whole chunk against the wall
  clock, which left only ~20–35 ms queued at the low point; each run-dry then paused the
  device until 64 ms were queued again (a 35–70 ms gap). Android never pauses on underrun: it
  plays silence.

Now the device's clock paces the mixer, as in a classic blocking audio loop:

- `select_backend` sets `m_dataThresholdRatio` to −10⁶, so `m_dataThreshold` is huge and
  `DoCallbackAT` never sleeps on its own: it only waits in `write()`;
- `write()` blocks while the queue holds more than the limit minus a chunk (96 ms on the PC,
  128 ms on the Switch; `NOVA3_AUDIO_QUEUE_MS` for tests), so the queue stays between the
  limit minus a chunk and the limit: 64–96 ms of slack for a late mixer and a constant
  latency. It waits in short sleeps sized to the missing room, with no lock held;
- the device runs from `play()` to `pause()` / `stop()` and plays silence when the queue is
  empty: no pause and refill on underrun. Underruns are counted where the device actually
  starves (SDL's stream callback, the Switch feeder) and logged as `audio: underrun N (X ms
  in all)`;
- `write()` never returns at once without having queued or waited: a device that stopped
  consuming has the chunk's excess dropped after a chunk + 100 ms (`audio: N ms dropped`); a
  paused track takes up to twice the limit and otherwise waits a chunk;
- the mixer thread runs raised (`rt::os::raise_thread_priority`: time-critical on Windows,
  just above the game's threads on the Switch) from its first `write`.

The Switch build plays through libnx's AudioOut instead of SDL: see [Nintendo Switch](switch.md).

## Verification

Output was recorded with `NOVA3_AUDIO_DUMP=file.pcm` (raw, 44.1 kHz stereo 16-bit):

- Main menu: the music fades in after loading. It is bass-heavy, with ~680 zero crossings/s
  and 0.72 left/right correlation, so music rather than noise.
- Holding fire in a match raises the level from ~1,500 to 6,000–9,000 RMS for the length of
  the burst.
