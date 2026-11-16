# Iris GenTL

Iris GenTL is a C library for discovering cameras, configuring GenApi features, and acquiring frames on Linux x86-64 with glibc. It can stream from multiple GenTL Producers (`.cti` files) in one process.

- Receive borrowed buffers through nonblocking polls or a finite wait. Normal capture after startup makes zero Iris heap allocations.
- Supply caller-owned buffers and inspect image, chunk, multipart, and GenDC metadata.
- Poll for complete rows on producers that expose a stable, CPU-visible image prefix.
- Browse and configure GenApi features on remote devices and GenTL modules.

## Binary packages

Download the package for your Linux x86-64 distribution and verify it against
the release's `SHA256SUMS`. [GitHub releases](https://github.com/DarrylGamroth/iris-gentl/releases)

| Platform | Artifact | Install |
| --- | --- | --- |
| Debian 13 amd64 | `*_debian13_amd64.deb` | `sudo apt install ./iris-gentl_VERSION_debian13_amd64.deb` |
| Ubuntu 24.10 amd64 | `*_ubuntu24.10_amd64.deb` | `sudo apt install ./iris-gentl_VERSION_ubuntu24.10_amd64.deb` |
| CentOS 7.9 x86-64 | `*-centos7.9-x86_64.tar.gz` | Extract into a directory of your choice. Run `bin/iris-capture` from the extracted prefix. |

Each package includes `libiris`, `iris.h`, `iris-capture`, and EMVA's unmodified
GenApi runtime with its notices. The Debian and Ubuntu packages use GenApi
3.5.1. The CentOS tarball uses GenApi 3.5.0 for glibc 2.17 and requires the
CentOS `libarchive`, `libxml2`, and `libstdc++` runtime packages. Supply the
GenTL Producer CTI and driver dependencies for your camera separately.

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

Applications that link to a prebuilt Iris library use `iris.h`. A default
source install needs the Iris library and GenApi runtime on the loader's
search path. The downloaded runtime is in
`build/_deps/emva/genapi-3.5.1/bin/Linux64_x64`. The release packages use
`-DIRIS_BUNDLE_GENAPI=ON` to install GenApi beside Iris with a relative loader
path. See the [environment settings](docs/guide.md#environment).

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

Iris is available under the [MIT License](LICENSE). EMVA's GenApi SDK, runtime, and GenTL reference header are separately licensed. [EMVA GenICam license](https://www.emva.org/wp-content/uploads/GenICam_License_20231009.pdf)
