# Audio

Reference: [Audio](../audio.md).

## Report

On real hardware the sound kept dropping out, and on a fast PC it happened too, less often.

## Causes

- On the Switch every thread started on core 0, and Horizon does not time-slice threads of
  the same priority: Vox's mixer stalled behind busy threads. SDL2's Switch audio thread also
  stalled now and then, and the queue never drained again (the game went silent).
- The backend's pacing let the mixer run ahead of or behind the device, and an underrun paused
  the stream.

## What was done

- Vox is paced by the output device: writes block while the queue is full (96 ms; 128 ms
  on the Switch), as Android's blocking `AudioTrack.write` does, and an underrun no longer
  pauses anything. Underruns are counted and logged (`audio: underrun N`).
- The Switch uses libnx's AudioOut directly: a feeder thread keeps three 21 ms buffers queued
  and mixes the open tracks into each released one. If waiting for a buffer fails, it sleeps
  5 ms and retries.
- Threads are spread across cores and the mixer runs at priority 0x2b (see
  [Switch port](switch-port.md)).

## Checked

The emulators played through the campaign and multiplayer without audio drops.
