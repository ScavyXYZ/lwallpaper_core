# Core regression checks

Run CTest in Release and Debug. The core has no Qt dependency.

- `performance_unit`: interrupt a one-hour PTS wait, test native-window
  exception cleanup, pooled frame lifetime, timing and queue cancellation.
- `public_api`: idle stop, invalid playback input and cancelled imports.
- `import_and_decoder`: conversion, byte-copy imports, timestamp preservation,
  loop draining and propagation of corrupted-packet failures from the decoder.
- `d3d11_gpu_smoke`: decode/draw/present on a private test window.

The GUI repositories separately test atomic library publication and Qt import
thread shutdown. Automatic core tests never attach to the desktop; the manual
`livewallpaper_selftest` executable does.
