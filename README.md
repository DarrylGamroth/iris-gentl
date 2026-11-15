# Iris GenTL

Iris GenTL is a GenTL Consumer for discovering and controlling cameras and acquiring frames on Linux x86-64 with glibc. It loads GenTL Producers (`.cti` files) at runtime and can stream from multiple producers in one process. The library uses EMVA GenApi for feature access and exposes a C API callable from C and C++.

- Use bounded callback polling, immediate receive, or a finite blocking wait.
- Receive borrowed image buffers without a per-frame copy or consumer heap allocation during normal acquisition after start.
- Use typed GenApi access for camera, device, interface, system, and stream node maps.
- Browse feature categories and selector relationships, supply host buffers, and inspect GenTL payload metadata.

Read the [user guide](docs/guide.md) for discovery, feature configuration, frame ownership, threading, and error handling. [iris.h](inc/iris.h) is the API reference. Run `doxygen docs/Doxyfile` from the repository root to generate HTML at `build-docs/html/index.html`.

## Build and test

### Obtain the EMVA dependencies

- Download **GenApi 3.5.1** from EMVA and verify the SHA-256 listed in its downloads archive. The ZIP contains separate Linux x86-64 SDK and runtime archives. Extract both into the same directory. [EMVA downloads archive](https://www.emva.org/standards-technology/genicam/genicam-downloads-archive/) · [GenApi 3.5.1 ZIP](https://www.emva.org/wp-content/uploads/GenICam_V3_5_1_public_data.zip)
- Download and extract the **GenTL 1.6 reference header**. Use the directory containing `GenTL.h` for `GENTL_INCLUDE_DIR`. [GenTL 1.6 ZIP](https://www.emva.org/wp-content/uploads/GenTL_v1_6.zip)

Keep the EMVA packages and their notices with your local dependency installation. Iris links to the public GenApi runtime as a shared library. Obtain GenTL Producer CTIs and their dependencies from their respective providers. [EMVA GenICam license](https://www.emva.org/wp-content/uploads/GenICam_License_20231009.pdf)

With both ZIPs downloaded, extract the dependencies from the Iris source directory.

```sh
downloads=/path/to/downloads
deps="$PWD/build-deps"
mkdir -p "$deps/genapi" "$deps/gentl"
for component in SDK Runtime; do
  unzip -p "$downloads/GenICam_V3_5_1_public_data.zip" \
    "Linux64_x64/GenApi-3.5.1-Linux64_x64-$component.tar.gz" |
    tar -xzf - -C "$deps/genapi"
done
unzip "$downloads/GenTL_v1_6.zip" -d "$deps/gentl"
```

### Configure and build

Install the libxml2 and libarchive development packages for XML source inspection. On Debian and Ubuntu they are `libxml2-dev` and `libarchive-dev`.

```sh
export GENICAM_ROOT="$deps/genapi"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DGENTL_INCLUDE_DIR="$deps/gentl"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/install
```

For a custom SDK layout, set `GENAPI_INCLUDE_DIR` and `CMAKE_LIBRARY_PATH` explicitly. The [environment settings](docs/guide.md#environment) cover CTI lookup, the GenApi cache, and runtime library paths.

To enable the simultaneous simulator test, configure `IRIS_triggered_CTI`, `IRIS_triggered_CLPROTOCOL`, `IRIS_continuous_CTI`, and `IRIS_continuous_RUNTIME` with the paths in [VERIFICATION.md](VERIFICATION.md). Configure both simulators in a separate build directory with `-DIRIS_COVERAGE=ON`, then run `cmake --build build-coverage --target coverage`. The target uses gcovr and requires at least 90% line coverage of consumer sources.

## Examples

The [runnable samples](examples/README.md) cover typed feature reads and writes, numeric constraints, enumeration entries, selectors, nonblocking capture, and software-triggered acquisition. The examples build with the default CMake targets.

## Capture

```sh
export GENICAM_GENTL64_PATH="/path/to/producers${GENICAM_GENTL64_PATH:+:$GENICAM_GENTL64_PATH}"
build/iris-capture --list producer.cti
build/iris-capture --capture producer.cti
```

The capture command requires exactly one matching device. Provide an optional serial argument to narrow the selection when the producer publishes a serial. The tool uses current camera settings. Use an idle camera configured for continuous acquisition.

## C API and ABI status

The Linux x86-64 C API and ABI are provisional. Build clients against the matching `<iris.h>` and `libiris` and rebuild them when updating Iris. Development builds can share a SONAME and ABI major even when the public interface changes.

## License

Iris source and documentation are available under the [MIT License](LICENSE). The EMVA GenApi SDK, runtime and GenTL reference header are separate dependencies under their own terms. GenTL Producers and their driver dependencies are supplied by their respective providers. [EMVA GenICam license](https://www.emva.org/wp-content/uploads/GenICam_License_20231009.pdf)
