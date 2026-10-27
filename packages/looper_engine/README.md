# looper_engine

Real-time audio engine for [Looper](../../README.md): a portable C++17 core
(audio I/O via [miniaudio](https://miniaud.io)) exposed to Dart through `dart:ffi`.

* `src/engine.*` — transport, tracks, metronome. Pure DSP, no I/O.
* `src/api.cpp` — C API (`src/looper_api.h`) + device handling.
* `lib/looper_engine.dart` — typed Dart wrapper.

Regenerate bindings after changing the header:

```sh
dart run ffigen --config ffigen.yaml
```

Run the engine tests (no audio device needed):

```sh
cmake -S test_native -B build/test_native && cmake --build build/test_native --target engine_test
./build/test_native/engine_test
```

`device_smoke` opens the default audio device for one second and prints what
the engine reports — handy when debugging a new interface.
