# Iris GenTL user guide

Iris GenTL is a Linux x86-64 C library for camera discovery, GenApi feature access, and image acquisition through GenTL Producers. The public header, `iris.h`, works in both C and C++. Include it directly in either language and link with `-liris`.

## Build and install

Follow the [build instructions](../README.md#build-and-test) to configure CMake. The default configure step downloads any missing EMVA dependencies. To use a custom SDK layout, set `GENAPI_INCLUDE_DIR` and `CMAKE_LIBRARY_PATH` to the GenApi headers and runtime library directories.

### Simulator tests

The default CTest suite uses the mock CTI built by Iris. Configure these CMake options to add the triggered mock and continuous simulator tests.

| CMake option | Input |
| --- | --- |
| `IRIS_triggered_CTI` | Mock-capable triggered GenTL producer file. |
| `IRIS_triggered_CLPROTOCOL` | Matching synthetic triggered CLProtocol library file. |
| `IRIS_continuous_CTI` | continuous SDK3 GenTL producer file with simulator support. |
| `IRIS_continuous_RUNTIME` | Matching continuous SDK3 runtime directory. |

Use a simulator runtime exposing serial `test-serial` and include the directory
containing `ldconfig` in `PATH`.

For line coverage, configure both simulators with the options above and use a
build directory outside the workspace. For example, set the four simulator
paths in the shell and run:

```sh
coverage_build=$(mktemp -d /tmp/iris-coverage.XXXXXX)
cmake -S . -B "$coverage_build" -DIRIS_COVERAGE=ON \
  -DIRIS_triggered_CTI="$IRIS_triggered_CTI" -DIRIS_triggered_CLPROTOCOL="$IRIS_triggered_CLPROTOCOL" \
  -DIRIS_continuous_CTI="$IRIS_continuous_CTI" -DIRIS_continuous_RUNTIME="$IRIS_continuous_RUNTIME"
cmake --build "$coverage_build" --target coverage --parallel
```

The coverage target keeps gcov data in that build directory and requires at
least 90% line coverage separately for the consumer and the CLI.

## Environment

| Variable | Purpose |
| --- | --- |
| `GENICAM_ROOT` | CMake hint for the unpacked EMVA SDK. Iris searches the SDK's `library/CPP/include` and `bin/Linux64_x64` directories. |
| `GENICAM_GENTL64_PATH` | Colon-separated CTI directories on Linux x86-64. Pass the CTI filename to Iris to use this search path. An absolute path selects one CTI directly. |
| `GENICAM_CACHE_V3_5` | Writable directory for preprocessed GenApi XML, read by the EMVA 3.5 runtime. Create the directory and set the variable before launching the application to use the cache. |
| `GENICAM_LOG_CONFIG_V3_5` | Optional logging properties file read by the EMVA 3.5 runtime. |
| `GENICAM_CLPROTOCOL` | CLProtocol registration roots for producers that support them. Configure the producer's camera selection separately when required. |
| `LD_LIBRARY_PATH` | Linux dynamic loader search path. Use it when Iris, the EMVA runtime, or a CTI's shared libraries are outside the configured system library paths. |

## Command-line tool

`iris-gentl info` lists camera identities across the CTIs on
`GENICAM_GENTL64_PATH`. Repeat `--cti` to query specific producers.
Camera commands use one CTI and require a unique device. Select by
`--serial`, or use the `--interface` and `--device` IDs shown by `info`.

```sh
iris-gentl get --cti producer.cti Width Height PixelFormat
iris-gentl get --cti producer.cti --set TriggerSelector=FrameStart TriggerSource
iris-gentl xml --cti producer.cti > camera.xml
iris-gentl grab 3 --cti producer.cti --timeout 1000 --output frames.raw
iris-gentl play 100 --cti producer.cti --timeout 1000 --json
```

- `features [GLOB]` lists feature names, principal types, and current access
  modes. `get` reads values, `set NAME=VALUE ...` applies writes in the given
  order, and `exec NAME` issues a command. `--set` applies ordered writes
  before the selected operation. Writes take effect immediately.
- `describe NAME` shows the node's access, limits and units, increment mode,
  enum entries, or string length as applicable. It lists the nodes that
  select the feature and any nodes that the feature selects.
- `--module` selects `remote`, `device`, `interface`, `system`, or `stream`
  for node-map commands. `xml` writes the active description to stdout.
- `event [COUNT]` subscribes to the selected map and prints event IDs,
  payload sizes, available timestamps, and the GenApi features captured for
  each event. The host timestamp is monotonic time at dequeue in nanoseconds.
  The device timestamp uses the device's units and epoch. Set the camera's
  event configuration with `--set` when needed.
- `grab [COUNT]` uses current camera settings and prints frame dimensions,
  PFNC format, stride, and byte count. A new `--output` file contains the raw
  bytes of each complete image in order, including row padding. The command
  stops on a rejected image or timeout. Use `--stream-only` to leave remote
  acquisition under external control, and `--trigger NAME` to issue a remote
  command before each wait.
- `play [COUNT]` summarizes capture over a fixed number of waits (100 by
  default). It reports complete, incomplete, unsupported, and invalid
  completions, timeouts, and elapsed capture time. When timestamps are
  available, it reports the minimum, mean, and maximum host dequeue interval
  between complete frames. Intervals can include time spent waiting for
  rejected or missing frames. A completed run returns 0 even when it counts
  timeouts or rejected frames. It accepts the same acquisition settings as
  `grab`.

Run `iris-gentl --help` for the complete option list.

Use `--json` for newline-delimited JSON output from `info`, `features`,
`describe`, `get`, `snapshot`, `diff`, `event`, `grab`, and `play`.
Feature values in `get`, `snapshot`, and `event` use the GenApi text
representation. Event timestamps are JSON integers. Use an integer-capable
JSON parser to preserve all 64 bits.

### Compare camera settings

`snapshot NAME ...` records the selected module, requested feature values,
and current direct and indirect selector values. Use `--set` to choose the
bank before reading it. Run one snapshot for each selector configuration
you need to inspect.

```sh
iris-gentl snapshot --cti producer.cti Width Height > before.snapshot
iris-gentl snapshot --cti producer.cti Width Height > after.snapshot
iris-gentl diff before.snapshot after.snapshot
```

`diff` reads the default snapshot file format and compares entries by module,
feature name, and selector values. A different selector context appears as a
removed entry and an added entry. The exit code is 0 for matching snapshots,
1 for differences, and 2 for an invalid input file. Use `diff --json` for
machine-readable changes.

## Discovery and ownership

1. Create one context with `iris_context_create`.
2. Call `iris_discover` for each CTI you want to query. Supply an absolute path or a bare filename. A bare name searches the colon-separated `GENICAM_GENTL64_PATH` and returns `IRIS_AMBIGUOUS` when it resolves to distinct files.
3. Inspect each snapshot with `iris_device_list_count` and `iris_device_list_get`. An empty list is a successful scan. Use the producer path, interface ID, and device ID for exact selection. Alternatively, supply a serial number and optionally an interface ID. Open rechecks the selection, so it can fail even after discovery succeeds.
4. Call `iris_device_open` with the appropriate access mode, then close the snapshot. Copy metadata strings you need beyond snapshot close.
5. Open a data stream by zero-based index. Query the available count with `iris_device_stream_count`. One context can open devices through multiple producers. Different devices can acquire concurrently, and each device can acquire on one stream at a time.

On shutdown, stop each stream, release outstanding frames, close streams, close devices, then close the context. Close functions returning a status clear the handle on success and preserve it on failure. Retry recoverable teardown failures with the same handle. A device close rejected because streams are open leaves the device usable. Otherwise, discard its borrowed node maps before calling close. If teardown begins and then fails, only close may be retried on that device.

## Configure features

The [C feature example](../examples/README.md) demonstrates metadata, access, constraints, typed writes, readback, and selector-dependent settings. The C++ example configures exposure and acquisition mode before polling frames.

Use `iris_node_map_browse(map, NULL, IRIS_CATEGORY_FEATURES, &list)` to list the direct children of `Root`. For entries of type `IRIS_FEATURE_CATEGORY`, use the entry name to browse the next level. `IRIS_SELECTED_FEATURES` lists the features a selector controls. `IRIS_SELECTING_FEATURES` lists the selectors that control a feature. The returned names can be used with the typed accessors. A snapshot includes display name, description, tooltip, visibility, type, and access. Access is captured at query time. Requery it when a selector or device state changes. Close each snapshot with `iris_feature_list_close`.

Use `iris_feature_metadata` to look up the same fields by feature name, plus the GenApi display representation for Integer and Float nodes and the notation and precision for Float nodes. A size query returns the scalar metadata. Supply a character buffer on the next call to copy the four strings into caller-owned storage. [GenApi display metadata](https://www.emva.org/wp-content/uploads/GenICam_Standard_v2_1_1.pdf)

Use `iris_node_map_list_features` for a flat, name-sorted snapshot. By default it lists the features reachable from `Root`. Set `IRIS_FEATURE_LIST_INCLUDE_INTERNAL` to include backing nodes, or `IRIS_FEATURE_LIST_AVAILABLE_ONLY` to filter on the current selector state. Filters can combine a case-sensitive glob or regular expression with the principal feature type. Use `iris_feature_alias`, `iris_feature_event_id`, and `iris_feature_string_max_length` to inspect individual nodes from the list. Event IDs are hexadecimal strings and can exceed 64 bits.

Call `iris_device_node_map(device, IRIS_REMOTE_DEVICE, &map)` for camera features. The other module selectors reach the system, interface, or local device. Use `iris_stream_node_map` for a data stream. Borrowed maps follow their owner's lifetime. The system and interface maps are shared with other open devices that use those modules.

Iris tries the indexed GenTL XML URLs in producer order, then the legacy URL list if needed. It uses the first local register or file XML/ZIP that loads and connects to the port. Relative file URLs resolve from the CTI directory. Vendor web URLs are skipped. Use `iris_node_map_xml_info` to inspect the selected URL and any schema and file versions supplied through the indexed GenTL API. [GenTL 1.6, §4.1.2](https://www.emva.org/wp-content/uploads/GenICam_GenTL_1_6.pdf)

To supply the remote device's GenApi description, set `remote_xml_path` in `iris_device_open_options_t` and open with `iris_device_open_with_options`. Pass a local XML file or a ZIP containing one XML document. Relative paths resolve from the process working directory. Iris loads and connects the file during open, and a failed override returns an error. Choose a description that matches the device's remote port and register layout. Each device can choose its own description. Feature access, remote events, and chunk decoding use that device's remote map.

Source inspection reports `IRIS_XML_IS_OVERRIDE` and an absolute file URL for an override. Producer URL indices and advertised versions apply only to descriptions selected through GenTL. The file can be removed after open because Iris retains the loaded XML.

Use `iris_node_map_xml` to copy the loaded description, including the XML member of a ZIP with one XML document. Use `iris_feature_xml` for a UTF-8 element by its XML `Name` attribute. XML names can differ from the names GenApi generates at runtime. A repeated XML name returns `IRIS_AMBIGUOUS`. Source inspection uses the bytes retained at map load, so it works even if the original file is replaced.

- Use `iris_feature_info` to check type and access in the current selector state. After changing a selector, query access, range, or enumeration entries again before using a dependent node. Serialize multi-call selector sequences when another thread can change the same selector.
- Use the typed accessors for Integer, Float, Boolean, String, and Enumeration nodes. Numeric values use the node's units. For example, read the unit before interpreting an exposure value. Range calls describe continuous, fixed, or list increments. Query discrete values only for list mode.
- Pass enumeration symbols such as `Continuous` exactly as the camera reports them. Entry queries include unavailable entries and report access for each one. Use the entry symbol to select a value.
- Use the GenApi text conversion functions for textual configuration. Use the String functions for direct String access. Commands use `iris_feature_execute` and optionally `iris_feature_command_done`.
- For variable-length text or numeric arrays, pass NULL and a zero size to query capacity. Allocate or select storage, then call again with its capacity. Text sizes include the terminating NUL. Array sizes count elements. On `IRIS_BUFFER_TOO_SMALL`, resize using the reported requirement and retry. The required capacity may change with the feature or selector.
- Producers that support `EVENT_FEATURE_INVALIDATE` can notify Iris of remote feature changes. Enable the subscription on the remote node map and call `iris_node_map_poll_invalidation` to apply queued events. Iris tries the standard local device handle first and can fall back to the remote port for producers such as the tested continuous CTI. A known feature name invalidates that map's cache so backing registers refresh in every selector state. This poll can allocate and uses the feature lock.

After an out-of-band change, call `iris_feature_invalidate` with the changed node's name to invalidate it and its GenApi dependents. Use `iris_node_map_invalidate` if the affected nodes are unknown. Later queries refresh invalidated cache entries as needed. Both calls work without an event subscription. [GenApi caching](https://www.emva.org/wp-content/uploads/GenICam_Standard_v2_1_1.pdf)

Use `iris_feature_write_batch` when several feature writes on one node map must run in order without interleaving with other Iris feature calls. List the selector write before its dependent writes. The batch stops on the first error and reports how many writes succeeded. Earlier writes and selector changes remain in effect. A failing write can itself affect the device. The [triggered software-trigger example](../examples/README.md#software-triggered-capture-in-c) shows how to apply a batch and verify the final settings.

To use the producer's native GenTL port stack, call `iris_node_map_start_stacking`, stage named GenApi text values with `iris_node_map_stack_add`, then call `iris_node_map_write_stack`. Call `iris_node_map_cancel_stack` to discard pending writes. Each node map has its own stack. Iris preserves entry order, including repeated writes to a selector, and applies the stack under the feature lock. Ordinary feature setters still take effect immediately.

GenApi groups register writes and can submit a group before a read required by a later feature. The producer reports the number of completed register writes. A feature can touch several registers, and GenApi can continue after rejecting a staged value. Use the returned status and `iris_last_error` to detect failure, then read back the affected settings before retrying. The stack is consumed even when execution fails. If the producer lacks stacked writes, applications can use `iris_feature_write_batch` for ordered settings.

For binary GenApi features, use `iris_feature_register_info` to query the current byte address and length. Pass caller storage to `iris_feature_read_register` or `iris_feature_write_register` to transfer the whole register in port byte order. Reads bypass the GenApi value cache, and writes use the node's access rules and dependency invalidation. Selectors can change the register's address or length between calls. [GenApi register interface, §2.9.7](https://www.emva.org/wp-content/uploads/GenICam_Standard_v2_1_1.pdf)

### Poll for camera and module events

Configure `EventSelector` and `EventNotification` on the camera or module as required. Enable the host subscription with `iris_node_map_enable_events(map, 1)`. On a remote map, Iris registers `EVENT_REMOTE_DEVICE` on the local device and delivers the payload to the remote XML's event ports. The system, interface, local device, and stream maps use `EVENT_MODULE` on their respective handles. Shared maps keep one subscription. [GenTL 1.6, §4.2](https://www.emva.org/wp-content/uploads/GenICam_GenTL_1_6.pdf)

Call `iris_node_map_try_get_event` once per iteration. On `IRIS_OK`, use the returned hexadecimal ID to identify the event features in the XML and read them through the usual GenApi getters before polling again. Event features can retain the last values for their own ID. Serialize poll and readback together when several threads use the same map. `IRIS_NO_EVENT` means the queue is empty. `IRIS_BUSY` means the feature lock is held, so retry on a later iteration. An event envelope larger than 1 MiB is discarded with `IRIS_OUT_OF_RANGE`, and a later poll can receive the next event. Event polling is a control operation and can allocate. Disable the subscription explicitly or let the map owner's close operation unregister it.

Use `iris_node_map_try_get_event_snapshot` when you need to keep an event or pass it to another thread. It copies the raw GenTL payload and captures the matching GenApi feature values before returning. Browse with `iris_event_snapshot_features` and read a captured value with `iris_event_snapshot_feature_to_string`. The feature list and raw payload pointer remain valid until `iris_event_snapshot_close`, including after subsequent polls and map close.

`iris_event_snapshot_info` includes the host's monotonic dequeue timestamp. A device timestamp is available when exactly one matching SFNC-style `Event…Timestamp` Integer node can be decoded. Check the validity bits before using either field. The device value uses the device's units and epoch. [GenICam SFNC event examples](https://www.emva.org/wp-content/uploads/GenICam_SFNC_v2_8.pdf)

## Acquire frames

Configure geometry, format, and triggering before `iris_stream_start`. With NULL start options, Iris allocates and queues the pool and manages both the host stream and remote acquisition. Use `IRIS_ACQUISITION_STREAM_ONLY` when the application manages the remote commands or a simulator has a separate trigger. Use a zero buffer count for the default, which respects the producer minimum.

Some producers require a fixed pool size. For the triggered native ring, set `trigger_fixture_ZERO_COPY=1` and use the ring depth reported by `iris_stream_buffer_requirements` as the explicit buffer count. Leave `buffers` NULL so the producer can announce its native slots. Iris sets `TLParamsLocked` before announcing the pool and clears its lock after revoking the buffers. Release outstanding frames and finish cleanup before changing the next acquisition's geometry.

### Caller-owned host buffers

After configuration, call `iris_stream_buffer_requirements` and prepare at least the reported minimum number of writable, aligned host buffers. Give each `iris_buffer_t` its address, capacity, and an optional application pointer. Set `iris_start_options_t.buffers` to the array and `buffer_count` to the exact array length. Iris announces the supplied memory directly and returns the corresponding pointer in `frame.user_data`. Each buffer range must be reserved for that stream and must remain valid until revocation. The descriptor array itself is only needed during start.

After a successful start or a failed start, release any borrowed frames and call `iris_stream_stop` successfully before freeing the storage. Retry stop if cleanup fails. If you stop while holding a frame, release it and stop again, or close the stream successfully, before freeing the storage. A producer can reject application memory or require reopening the stream before changing allocation mode. triggered mock and continuous simulator accept caller memory on their tested host capture paths.

Space caller buffers on separate cache lines so another thread or the producer can touch a queued buffer while you read a borrowed one. On Linux x86-64, align each base to 64 bytes and the producer's required alignment. If carving buffers out of an arena, round the stride up to meet both alignments. Standalone buffers allocated by Iris as a fallback are cache-line aligned. Producers control the alignment of any buffers they allocate.

Iris accepts CPU-accessible host addresses through standard GenTL announcement. Direct DMA-BUF file descriptor import and device-only GPU buffers require producer extensions. If a producer accepts a CPU mapping through standard announcement, the application still owns the mapping and any required synchronization.

See the [triggered mock example](../examples/README.md#software-triggered-capture-in-c) for configuring software triggering, executing `TriggerSoftware` after stream start, and receiving and releasing one frame.

| Receive API | Ownership and scheduling |
| --- | --- |
| `iris_stream_try_get_frame` | Immediate attempt. `IRIS_NO_FRAME` means the queue is empty. An `IRIS_OK` completion requires exactly one release on its issuing stream. |
| `iris_stream_wait_frame` | Waits with a positive, finite timeout. `IRIS_TIMEOUT` means no completion arrived before the timeout. An `IRIS_OK` completion requires exactly one release. |
| `iris_stream_poll` | Calls the handler inline for at most the requested number of completions. An empty queue returns `IRIS_OK` with zero deliveries. Iris releases each frame when its handler returns. |

`IRIS_OK` from explicit receive means a buffer completed. Check `frame.status` before using pixels. Only `IRIS_FRAME_COMPLETE` provides an image. Incomplete, unsupported, and invalid images may still carry metadata. Check `frame.valid` before using optional fields, and honor `stride_bytes` when traversing rows. Use `pixel_format_namespace` alongside `pixel_format`. Producer timestamps and host monotonic timestamps can have different clock origins.

Use `frame.payload.valid` to check which GenTL payload fields the producer reported. The payload view includes type, announced capacity, bytes filled, data size, image offset, delivered height, and chunk indicators. When `frame.payload.data` is present, its byte extent is `frame.payload.size_filled`. This view can include padding, partial data, or non-image payloads. Use `frame.data` and `frame.image_size` for validated single-image pixels. Use the per-part views for multipart and GenDC, where the bytes written can be separated by gaps.

### Multipart and GenDC parts

For multipart or contiguous GenDC 1.x, inspect `frame.parts_status` and then iterate `frame.parts[0..part_count)`. Each part gives a bounded byte view and reports format, dimensions, and identifiers where available. Interpret `part.type` according to `frame.payload.type`. The top-level image status still describes the single-image view. Use the parts even when that status is `IRIS_FRAME_UNSUPPORTED_PAYLOAD`. The array and its pointers expire together with the frame borrow, including when a poll handler returns.

Iris stores up to `IRIS_MAX_FRAME_PARTS` descriptors per buffer at start. Excess parts, incomplete completions, malformed descriptors, and discontiguous composite payloads yield an error in `parts_status` and an empty array. GenDC views use the linear data offsets in the final container descriptor. Part data is exposed as delivered, with no format conversion. [GenTL 1.6](https://www.emva.org/wp-content/uploads/GenICam_GenTL_1_6.pdf) · [GenDC 1.2](https://www.emva.org/wp-content/uploads/GenICam_GenDC_v1_2.pdf)

### Browse and read frame chunks

Keep the frame borrowed from `iris_stream_try_get_frame` or `iris_stream_wait_frame` and call `iris_frame_chunk_list` for a name-sorted snapshot of its readable chunk features. Use `iris_feature_list_get` to inspect names and types. Match the type to the corresponding `iris_frame_chunk_get_*` function for Integer, Float, Boolean, String, or Enumeration. Use `iris_frame_chunk_read_register` for raw register bytes, or `iris_frame_chunk_to_string` for GenApi text conversion. String, enumeration, and register reads accept a size query with a NULL destination and zero input size. Close the metadata snapshot with `iris_feature_list_close` whenever you finish using it. The snapshot can outlive the frame.

Iris attaches the validated chunks only during each browse or read. The frame must remain borrowed while reading values. For GenDC, this uses one contiguous chunk metadata part described by the remote XML. Chunk operations can allocate and take control locks. Call them outside poll handlers.

Process a borrowed image before release, or copy it into application-owned storage if it must outlive the borrow. The descriptor's token belongs to the issuing stream. A valid release consumes the borrow even if requeue fails. If requeue fails, stop the stream to recover. After stop, frames already borrowed remain valid until released.

### Poll rows during acquisition

Use `iris_stream_poll_rows` only when the producer guarantees that the fill count on an acquiring buffer identifies contiguous bytes that are already visible to the CPU and remain stable until requeue. The supported layout is contiguous little-endian Mono10, Mono12, Mono14, or Mono16 in unpacked 16-bit storage, with zero image offset and no padding or chunks.

Supply an `iris_row_target_t` with application-owned storage separate from the announced buffers. Keep the destination and geometry fixed until a successful update sets `complete`. Each successful update reports the first row and count copied during that call. Completion can report zero new rows if an earlier poll copied the entire image. Treat `IRIS_NO_FRAME` as an empty poll. If an error invalidates a partial frame, discard its published rows and stop the stream before switching destinations.

### Poll multiple streams

Call the following from the reader after starting both streams. Schedule the next iteration with the application's idle policy when both delivery counts are zero.

```c
#include <iris.h>

static void count_complete(void *user, const iris_frame_t *frame)
{
    if (frame->status == IRIS_FRAME_COMPLETE)
        ++*(size_t *)user;
}

static iris_status_t poll_pair(iris_stream_t *first, iris_stream_t *second,
                               size_t completed[2], size_t delivered[2])
{
    delivered[0] = delivered[1] = 0;
    iris_status_t status = iris_stream_poll(first, count_complete,
                                            &completed[0], 4, &delivered[0]);
    if (status != IRIS_OK)
        return status;
    return iris_stream_poll(second, count_complete,
                            &completed[1], 4, &delivered[1]);
}
```

The same example compiles as C or C++. A C++ callback can use an `extern "C"` function and must return normally. Keep callbacks short and call Iris outside them. The callback and its frame pointer are used synchronously by `iris_stream_poll`.

## Threading and allocation

Use one reader per stream across try, wait, and poll. Try and poll call the producer with a zero event timeout and return `IRIS_BUSY` on consumer contention. An admitted call holds its stream lock through the producer calls and any inline callback. The producer and the callback can still introduce latency. Explicit release and stats may take the stream lock.

Serialize control operations across a device and its sibling streams. Serialize feature operations with stream start and stop for the associated device. Stop may overlap receive and release on its stream. Quiesce users of any handle before closing it. Iris leaves reader scheduling to the application.

To hand off an explicitly borrowed frame, publish the descriptor with application synchronization and finish all reads before releasing it. Iris keeps stream objects, buffer slots, and borrowed part arrays on separate cache-line boundaries. The stream mutex and global token counter coordinate shared state. Space concurrently written caller-owned descriptors on separate lines as well.

Iris provisions its buffers and bookkeeping during startup. From the first receive after a successful start, normal try, wait, poll, and release make zero consumer heap allocations and lend image storage directly. The scope includes empty queues, receive timeouts, and rejected images. Startup, feature operations, teardown, and exceptional recovery can allocate. Producer and application callback allocations are separate from the consumer budget.

## Status and diagnostics

Zero is success. Positive receive statuses represent an empty queue, timeout, or stopped stream. After a negative status, read `iris_last_error` immediately on the same OS thread and copy any strings needed later. A numeric native code is present only when `native_code_valid` is nonzero. Handle contention and retryable teardown according to the operation that returned the status.

For `IRIS_NOT_FOUND` or `IRIS_AMBIGUOUS` during discovery or open, check the CTI path and select a device by its exact interface and device IDs. For a feature error such as `IRIS_WRONG_TYPE`, `IRIS_ACCESS_DENIED`, or `IRIS_OUT_OF_RANGE`, query the current feature type, access, and constraints. `IRIS_DISCONNECTED` is reserved. A GenTL I/O failure alone does not identify a disconnect.

C++ exceptions that unwind through a compatible producer ABI are translated to C status at the Iris boundary. A CTI runs inside the process. Isolate it in a separate process if the application must survive a producer crash or abort.

## API reference and compatibility

Every public declaration and its ownership, parameter, and return contract are in [iris.h](../inc/iris.h). To generate the browsable reference from the repository root, run `doxygen docs/Doxyfile` and open `build-docs/html/index.html`.

The Linux x86-64 C API and ABI are provisional. Use the header that matches the library you build against and rebuild clients when updating Iris.
