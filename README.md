# pdwclap

`wclap~` is a Puredata (or my preferred: [plugdata](https://plugdata.org/)) external that loads **WCLAP** plugins, which are CLAP plugins compiled to wasm32.

This is the plugin-system I use in [poketrack](https://konsumer.js.org/poketrack/), so you can use the same plugins in both places, write them in any language (even pd) and they are sandboxed from the host system, and don't need to be recompiled for every host platform. This allows you to share the same compiled plugins with everyone, for easy use.

```
[wclap~ /path/to/thing.wclap.wasm]
[wclap~ /path/to/thing.wclap.wasm com.example.plugin-id]
```

add `[wclap~]` to your puredata patch, then right-click & choose "help", for more info. You can also see some examples in `examples.pd` (use [plugdata](https://plugdata.org/), since it has extended stuff.)

## Build

Needs CMake ≥ 3.28 and a C++20 compiler.

```sh
cmake -B build
cmake --build build
```

## License

zlib, same as the rest of the surrounding projects — see `LICENSE`.
wclap-bridge, Wasmtime, and the CLAP headers are fetched at build time and
carry their own licenses (Apache-2.0 WITH LLVM-exception, Apache-2.0, MIT).
