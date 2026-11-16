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

Run it with continuous simulator using the CTI directory and SDK3 runtime from the [simulator settings](../docs/guide.md#optional-external-simulators).

```sh
export GENICAM_GENTL64_PATH="/path/to/continuous/cti${GENICAM_GENTL64_PATH:+:$GENICAM_GENTL64_PATH}"
export LD_LIBRARY_PATH="/path/to/continuous/runtime${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
build/examples/iris-example-configure-and-acquire continuousSDK3GenTL.cti test-serial 2000
```

The last argument uses the camera's exposure unit, which is microseconds on this simulator. Use an idle device that can acquire continuously. The program exits with an error if it cannot obtain eight complete frames within five seconds.

## Software-triggered capture with the CLI

Build the in-tree mock CTI and CLI. The mock uses the GenTL header and needs
no camera SDK.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target iris-gentl iris-test-cti
IRIS_FAULT=progressive-trigger build/iris-gentl acquire 2 \
  --cti "$PWD/build/iris-test-cti.cti" --stream-only --buffers 1 \
  --set TriggerSelector=FrameStart --set TriggerMode=Off \
  --set TriggerSource=Software --set TriggerMode=On \
  --trigger TriggerSoftware
```

The CLI starts the host stream, issues `TriggerSoftware` before each wait,
and releases each completed frame. The mock returns two 2 × 4 Mono16
images, each 16 bytes. Add `--output frames.raw` to save them in a new file.

## Software-triggered capture in C

[software_trigger.c](software_trigger.c) uses `iris_feature_write_batch` to
apply the same settings and reads them back. It verifies that the stream
times out before the trigger, releases the completed frame, and waits for
another timeout to confirm exactly one completion. It then stops and
closes the stream, device, and context. [GenICam SFNC trigger controls](https://www.emva.org/wp-content/uploads/GenICam_SFNC_v2_8.pdf)

```sh
cmake --build build --target iris-example-software-trigger iris-test-cti
IRIS_FAULT=progressive-trigger \
  build/examples/iris-example-software-trigger "$PWD/build/iris-test-cti.cti"
ctest --test-dir build -R '^example-mock-software-trigger$' --output-on-failure
```

The C example also runs with the triggered mock copy path and its test CLProtocol
library. Configure `IRIS_triggered_CTI` and `IRIS_triggered_CLPROTOCOL` as described in
the [optional simulator settings](../docs/guide.md#optional-external-simulators)
and run `ctest --test-dir build -R '^example-software-trigger$' --output-on-failure`.
