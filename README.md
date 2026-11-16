# Iris GenTL

Iris GenTL is a C library for discovering cameras, configuring GenApi features, and acquiring frames on Linux x86-64 with glibc. It can stream from multiple GenTL Producers (`.cti` files) in one process.

- Receive borrowed buffers through nonblocking polls or a finite wait. Normal capture after startup makes zero Iris heap allocations.
- Supply caller-owned buffers and inspect image, chunk, multipart, and GenDC metadata.
- Poll for complete rows on producers that expose a stable, CPU-visible image prefix.
- Browse and configure GenApi features on remote devices and GenTL modules.

## Build and test

Use C and C++17 compilers, CMake 3.18 or newer, and the libxml2 and libarchive development packages (`libxml2-dev` and `libarchive-dev` on Debian and Ubuntu).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Building Iris uses the GenApi SDK headers, the GenApi runtime libraries, and the GenTL 1.6 reference header. CMake uses existing copies when it finds them. Otherwise it downloads GenApi 3.5.1 and GenTL 1.6 as needed, verifies their SHA-256 checksums, and caches them under `build/_deps/emva`. [EMVA downloads archive](https://www.emva.org/standards-technology/genicam/genicam-downloads-archive/)

For an offline build, set `GENICAM_ROOT` to an unpacked SDK and runtime, then configure with `-DIRIS_DOWNLOAD_EMVA=OFF -DGENTL_INCLUDE_DIR=/path/to/gentl`.

GitHub Actions builds Iris and runs the bundled mock CTI tests. See the [user guide](docs/guide.md#simulator-tests) for optional triggered mock and continuous simulator tests and for coverage.

## Install and use

```sh
cmake --install build --prefix /path/to/iris
```

Applications that link to a prebuilt Iris library use `iris.h`. They need the Iris library and GenApi runtime on the loader's search path. The downloaded runtime is in `build/_deps/emva/genapi-3.5.1/bin/Linux64_x64`. Get camera CTIs and their runtime dependencies from their providers. See the [environment settings](docs/guide.md#environment).

To try capture from the build directory, point Iris at a producer and use an idle camera configured for continuous acquisition.

```sh
export GENICAM_GENTL64_PATH="/path/to/producers${GENICAM_GENTL64_PATH:+:$GENICAM_GENTL64_PATH}"
build/iris-capture --list producer.cti
build/iris-capture --capture producer.cti [serial]
```

The capture command uses current camera settings and requires one matching device. Supply the optional serial number when several devices match.

## Documentation

See the [user guide](docs/guide.md), [C API reference](inc/iris.h), and [runnable C and C++ examples](examples/README.md). Run `doxygen docs/Doxyfile` to generate the HTML reference under `build-docs/html`.

The C API and ABI are provisional. Use the header that matches `libiris` and rebuild clients when updating Iris.

## License

Iris is available under the [MIT License](LICENSE). EMVA's GenApi SDK, runtime, and GenTL reference header are separately licensed and stay in the build directory when downloaded. [EMVA GenICam license](https://www.emva.org/wp-content/uploads/GenICam_License_20231009.pdf)
