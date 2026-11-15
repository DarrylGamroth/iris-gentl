# Iris GenTL examples

Build with CMake from the repository root. The examples link to Iris and use the public `iris.h` header.

```sh
cmake --build build --target iris-example-features iris-example-configure-and-acquire iris-example-software-trigger
```

## Inspect and change camera features in C

[features.c](features.c) opens the camera by serial number, borrows its remote node map, and queries the feature's type and access. It prints the current value, numeric limits or enumeration entries as applicable, and the readback after a write. It also shows how to apply an enumeration selector before inspecting or changing a dependent feature, and how to execute a command and query completion.

The register-backed test CTI exposes writable nodes with deterministic values. Use it to try these commands without a camera. It is built when `BUILD_TESTING=ON`.

```sh
cmake --build build --target iris-test-cti
export GENICAM_GENTL64_PATH="$PWD/build${GENICAM_GENTL64_PATH:+:$GENICAM_GENTL64_PATH}"
export IRIS_FAULT=example
features=build/examples/iris-example-features
"$features" iris-test-cti.cti test-serial browse Root
"$features" iris-test-cti.cti test-serial browse AcquisitionControl
"$features" iris-test-cti.cti test-serial get Width
"$features" iris-test-cti.cti test-serial set Width 48
"$features" iris-test-cti.cti test-serial set ExposureTime 450
"$features" iris-test-cti.cti test-serial set ReverseX 1
"$features" iris-test-cti.cti test-serial set DeviceUserID iris-demo
"$features" iris-test-cti.cti test-serial --select TriggerSelector FrameStart set TriggerMode On
"$features" iris-test-cti.cti test-serial execute TriggerSoftware
unset IRIS_FAULT
```

Each invocation opens a fresh fixture instance. In the selector example, the selector write and the dependent read/write/readback all happen in one session. On a real device, use features and values supported by its node map. The tool leaves any written values in effect and only issues a command when explicitly requested.

The fixture reports `TriggerSoftware pending` on the first completion query. For commands that need to finish before the next operation, continue calling `iris_feature_command_done` with an application deadline.

## Configure and acquire in C++

[configure_and_acquire.cpp](configure_and_acquire.cpp) opens a camera by serial number, checks `ExposureTime` type and access, reports its limits and units, writes the requested exposure, and reads it back. It sets and reads back `AcquisitionMode=Continuous`, starts the stream, then polls in batches of four. The callback sees the frame only while Iris lends it. The example stops and closes the stream, device, and context.

Run it with continuous simulator using the CTI directory and SDK3 runtime from the [fixture settings](../VERIFICATION.md):

```sh
export GENICAM_GENTL64_PATH="/path/to/continuous/cti${GENICAM_GENTL64_PATH:+:$GENICAM_GENTL64_PATH}"
export LD_LIBRARY_PATH="/path/to/continuous/runtime${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
build/examples/iris-example-configure-and-acquire continuousSDK3GenTL.cti test-serial 2000
```

The last argument uses the camera's exposure unit, which is microseconds on this simulator. Use an idle device that can acquire continuously. The program exits with an error if it cannot obtain eight complete frames within five seconds.

## Software-triggered capture in C

[software_trigger.c](software_trigger.c) uses `iris_feature_write_batch` to select `FrameStart`, turn `TriggerMode` off, select `TriggerSource=Software`, then turn `TriggerMode` on. It reads back the final settings before starting the host stream. The mock supports one trigger selector and source. It uses `IRIS_ACQUISITION_STREAM_ONLY` because the fixture has no remote `AcquisitionStart` or `AcquisitionStop`. [GenICam SFNC trigger controls](https://www.emva.org/wp-content/uploads/GenICam_SFNC_v2_8.pdf)

The example first verifies that the stream times out without a trigger. It executes `TriggerSoftware`, waits up to one second for a completion, and releases the borrowed frame even if the image is rejected. A second wait must time out, confirming that the mock delivered one frame for the command. The example then stops and closes the stream, device, and context.

Rebuild the triggered mock CTI and test CLProtocol library with the SFNC trigger nodes before running the example.

```sh
triggered="$(cd ../trigger_fixtureGenTL/build-genapi351 && pwd)"
GENICAM_GENTL64_PATH="$triggered" \
trigger_fixture_GENTL_MOCK=1 trigger_fixture_ZERO_COPY=0 trigger_fixture_CLPROTOCOL="$triggered/libCLProtocol_test.so" \
  build/examples/iris-example-software-trigger trigger_fixture.cti
```

To run it through CTest, configure `IRIS_triggered_CTI` and `IRIS_triggered_CLPROTOCOL` with the fixture paths from [VERIFICATION.md](../VERIFICATION.md), then run `ctest --test-dir build -R '^example-software-trigger$' --output-on-failure`. The example uses the triggered mock's copy path. Its native DMA ring requires an exact buffer count and rejects Iris's `TLParamsLocked` write once native buffers are announced.
