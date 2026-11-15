# Implementation and qualification


## Fixtures

Configure the CMake options below to enable simulator coverage in the [build and test commands](README.md#build-and-test). Use the SDK3 runtime that matches the continuous CTI. Include the system directory containing `ldconfig` in `PATH` for that runtime.

| CMake option | Input |
| --- | --- |
| `GENTL_INCLUDE_DIR` | Directory containing the GenTL 1.6 reference header |
| `GENAPI_INCLUDE_DIR` | GenApi SDK include directory, or set `GENICAM_ROOT` |
| `CMAKE_LIBRARY_PATH` | GenApi runtime library directory if the SDK layout needs an explicit search path |
| `IRIS_triggered_CTI` | Path to the mock-capable triggered GenTL producer |
| `IRIS_triggered_CLPROTOCOL` | Path to the synthetic triggered CLProtocol library |
| `IRIS_continuous_CTI` | Path to the continuous SDK3 GenTL producer with simulator support |
| `IRIS_continuous_RUNTIME` | Directory containing the matching continuous SDK3 runtime libraries |

The triggered synthetic CLProtocol map exposes `TriggerSoftware` and lacks remote `AcquisitionStart`, so the test uses stream-only mode and issues the trigger separately. continuous simulator uses `AcquisitionMode=Continuous` and `ExposureTime=2000` µs. An earlier continuous CTI build returned an SDK3 FIFO identity error at stop. The tested CTI passed both stop orders in the mixed test.

## Acquisition and allocation

Release CTest passed with triggered mock and continuous simulator capturing in one context. One representative run received 1,346 triggered completions and 50 simulator completions while both were active. Capture used zero-timeout try on triggered and a poll limit of two on simulator. A cross-stream release was rejected. After closing triggered, simulator delivered five more frames. After reopening triggered and closing simulator, triggered delivered five more.

The allocation probe reported zero consumer calls and bytes in the mixed capture interval. It counted 3,145 calls and 99,052 bytes elsewhere in the process during that interval. An triggered-only interval reported zero in both buckets. The probe intercepts glibc heap entry points and checks call origins, including C++ allocator stack frames and worker threads. It calibrates with a known allocation and excludes the triggering thread while it calls GenApi. The other bucket includes producer and runtime activity. Private allocators and unattributed stacks limit the scope of the measurement.

## capture producer bench

The capture producer CTI discovered a GE34GM. The bench used the camera's existing 640 × 480 Mono8 settings without persistent configuration writes. The CTI required updating the device list on the interface used for `IFOpenDevice` after reopening it.

The release consumer received 100 complete capture producer frames in 3.964 s while also receiving triggered mock frames. It then received 25 capture producer frames after triggered closed and 25 more after restarting the camera stream. The 25-frame capture producer-only segment reported no frame-ID gaps and a maximum host dequeue interval of 40.459 ms. The intervals measure host dequeue timing.

The consumer allocation count remained zero. During the capture producer-only 25-frame segment the probe observed 21,942 calls and 2,132,992 bytes elsewhere in the process. Functional capture producer capture and the consumer allocation check passed. The probe groups producer and runtime allocations together, so this run cannot establish which component accounts for the observed allocations outside the consumer. Higher-rate settings, external trigger timing, a long-duration soak and multiple physical cameras remain unqualified.

## Build and coverage

Release CTest passed both the simultaneous simulator and fault-injection suites. The coverage build recorded 92.9% line coverage (763 of 821 lines), 98.2% function coverage and 53.4% branch coverage of the consumer. The fault CTI exercises pool setup and rollback, bad or incomplete completions, timeout and cancellation, requeue failure and retryable stop and revoke. The installed executable and library also passed mixed capture producer and triggered capture with an explicit GenApi runtime path.

## Post-split capture producer rerun

The release build with implementation files in `src/` and headers in `inc/` acquired 100 capture producer frames and 6,338 triggered mock frames in the same process over 3.964 s. capture producer delivered 25 more frames after triggered closed and 25 after restarting the capture producer stream. The probe measured zero consumer heap allocations in all three intervals. It counted 81,237 calls elsewhere in the process during the mixed interval.

An earlier attempt after the translation-unit split reached 125 complete capture producer frames, then returned one `IRIS_FRAME_INVALID_IMAGE` after restart and the harness exited. The subsequent run completed. The cause of the invalid completion remains unverified.

The release used GCC 14.2 in its default GNU C++17 mode. CMake specifies C++17 as the minimum.


## capture producer restart investigation

The diagnostic release build ran 60 capture producer acquisition cycles, including 59 stop/start transitions, with triggered mock continuously open and streaming. It delivered 300 complete capture producer images and 15,966 triggered images. Cycle 52 also delivered three rejected capture producer completions. [GenTL metadata with addresses removed](capture producer-cycle-52.log). The allocation probe reported zero consumer heap calls in the capture intervals.

| capture producer frame ID | Announced and reported buffer size | Filled size and reported data size | Nominal height | Delivered image height | Incomplete flag |
| --- | ---: | ---: | ---: | ---: | ---: |
| 4 | 307,200 B | 301,584 B | 480 | 471 | 0 |
| 5 | 307,200 B | 301,584 B | 480 | 471 | 0 |
| 6 | 307,200 B | 304,512 B | 480 | 475 | 0 |

All three reported a 640-pixel width, Mono8 (PFNC `0x01080001`, namespace 4), image present, image offset 0, no X/Y padding and an image payload. Those queries returned success. `BUFFER_INFO_DATA_LARGER_THAN_BUFFER` returned `GC_ERR_NOT_IMPLEMENTED` (-1003). The filled sizes cannot contain the nominal 307,200-byte image. The consumer now reports these short images as incomplete and keeps the pixel pointer null. The producer or network cause of the short delivery has not been established.

Set `capture-producer_CTI`, `IRIS_triggered_CTI`, and `IRIS_triggered_CLPROTOCOL` to the installed paths. Configure a release build with `-DIRIS_TRACE_INVALID_IMAGE=ON` and run the capture tool.

```sh
trigger_fixture_GENTL_MOCK=1 \
trigger_fixture_CLPROTOCOL="$IRIS_triggered_CLPROTOCOL" \
  build-release/iris-capture --restart-bench \
  "$capture-producer_CTI" \
  "$IRIS_triggered_CTI" 60
```

The trace build prints bounded metadata on rejected completions before releasing them. Its diagnostic I/O can perturb capture timing. The regular build keeps tracing off. After the change, 25 additional cycles delivered 125 capture producer images and 6,203 triggered images with zero rejected completions and zero observed consumer allocations. The injected short-image test establishes the classification change independently of recurrence at the bench.

## Threads and exception boundaries

Keep the caller-driven acquisition model. It lets an application alternate bounded polls or dedicate its own reader per stream. Threads inside the same process would share the producer's failure domain. If surviving producer faults is a deployment requirement, run that producer in a supervised process and design the shared frame lifetime and restart protocol explicitly.

Public status-returning entry points that can throw translate exceptions to C status and thread-local diagnostics. The fault CTI deliberately throws during event retrieval, requeue and stop. A failed receive marks the stream for recovery, a failed requeue consumes the borrow and clears in-flight accounting, and stop preserves retryable cleanup. This catches C++ exceptions that can unwind into the consumer with a compatible ABI. A producer that aborts, crashes, terminates internally, or corrupts process memory requires process isolation for containment.

## Extended capture producer restart run

Iris ran 160 cycles with one GE34GM at 640 × 480 Mono8, continuous acquisition, trigger off, approximately 4 ms exposure, and 1,500-byte GigE packets. Each cycle rebuilt the eight-buffer pool and captured five complete images. triggered mock stayed active in the same process.

Iris received 800 complete capture producer frames and 45,821 complete triggered mock frames. No short capture producer images occurred in this run. The allocation probe measured zero Iris heap calls during capture. Repeat the diagnostic command above with `160` cycles to run the same workload.

## GenApi C API tests

The three release tests exercise all 24 public GenApi entry points across the register-backed mock CTI, triggered mock, and continuous simulator. They check typed read/write, feature type and access, ranges and units, text conversion, enumeration entry metadata, selector banks, and command pending/completed status. The triggered test also checks that changing Width updates PayloadSize. Numeric list queries are tested on fixed-increment features and return IRIS_NOT_SUPPORTED. A later XML fixture verified successful list queries for both types. The tests cover the consumer API and selected GenApi behavior. EMVA's XML engine, all GenApi node classes, and asynchronous feature invalidation require separate coverage.

The follow-up adds caller-driven GenTL feature invalidation. The register-backed fixture observes a stale maximum before polling and the refreshed maximum after polling. continuous simulator confirms event subscription and consumption through the public API. The numeric-list unit tests exercise size query, insufficient capacity and successful integer and float copy with typed node doubles. The follow-up fixture below verifies the XML-loaded path. All four release tests passed with 93.5% consumer line coverage.

The review follow-up confirmed successful list queries through EMVA GenApi 3.5.1. The simulator exposes an Integer with a semicolon-separated ValidValueSet and a Converter that produces a Float list. The C API returns the exact values, reports required capacity, and leaves undersized output unchanged.

## Iris GenTL v1 rename

All seven CTest checks passed in the release and coverage builds. Coverage was 93.2% of consumer lines (1,004 of 1,077). triggered mock and continuous simulator streamed concurrently with zero measured consumer heap calls or bytes. The probe also confirmed that it could attribute a known context allocation to the renamed library.

The ABI checks verified the 46 `iris_` exports, the `IRIS_1` symbol version, the `libiris.so.1` SONAME, and the Linux x86-64 layouts. Temporary negative checks rejected a changed signature, status constant, packing, version node, and SONAME. Strict C11 and C++17 clients linked against the staged installation, and the installed capture utility passed the same mixed simulator test. Doxygen generated the guide and API reference with warnings treated as errors. The guide's polling example compiled as C11 and C++17.

## Standard environment and examples

A fresh CMake build found the EMVA 3.5.1 headers and libraries through `GENICAM_ROOT`. The mixed simulator test found both CTIs by filename through `GENICAM_GENTL64_PATH` and measured zero consumer heap calls and bytes during acquisition. The C feature example verified readback for integer, float, Boolean, string, and selector-dependent enumeration writes. It also queried command completion and verified that EMVA populated the directory specified by `GENICAM_CACHE_V3_5`. The C++ example read back `ExposureTime=2000` µs and `AcquisitionMode=Continuous` and polled eight complete frames from continuous simulator.

All nine tests passed in the release and coverage builds. Consumer line coverage was 93.7% (1,009 of 1,077). Doxygen generated the guide, examples, and API reference.

## triggered software triggering

The example read back `TriggerSelector=FrameStart`, `TriggerSource=Software`, and `TriggerMode=On`. The triggered mock copy path timed out before the trigger, returned one complete image after `TriggerSoftware`, and timed out again after frame release. Five consecutive runs passed. All ten Iris tests and all 17 triggered build tests passed with simulator fixtures. Iris consumer line coverage was 93.7%, and simultaneous triggered mock and continuous simulator capture measured zero consumer heap allocations.

## Feature browsing and caller-owned buffers

The C feature example walks the register-backed CTI's category tree. Tests check category and selector relationships, feature metadata, snapshot lifetime after device close, and the root category on triggered mock and continuous simulator. The fault fixture also checks caller-buffer alignment, capacity, overlap, failed announcement and revocation, unsupported announcement, and payload metadata for incomplete and non-image completions.

triggered mock and continuous simulator streamed simultaneously with both library-managed and caller-owned pools. The caller-owned test checks the returned application pointer, buffer address, capacity, and filled extent for each complete image. The calibrated allocation probe measured zero consumer calls and bytes during nonblocking try/release and bounded poll from the first capture interval. Producer and runtime allocation counts are reported separately. All nine tests passed in release and coverage builds.

## Chunk decoding and payload parts

The fault CTI returns transport chunk descriptors, GenTL multipart buffers, and contiguous GenDC containers in caller-owned memory. The payload test checks named GenApi readback from both chunk layouts, part offsets and bounds, the poll callback view, the part-count limit, rejection of composite buffers, and stale-token rejection. It measures zero consumer calls and bytes around nonblocking receive, release, and multipart polling. Named chunk lookup is a separate operation and was outside those measurement intervals.

All ten tests passed in release and coverage builds. Consumer line coverage was 94.3% (1,346 of 1,427). triggered mock and continuous simulator streamed simultaneously into caller-owned pools with zero measured consumer heap allocations. The mixed simulator run delivered 1,590 triggered and 50 continuous frames while both streams were active.

## Payload review and cache-line layout

All ten simulator tests passed in release, coverage, and UBSan builds. Consumer line coverage was 94.9% (1,409 of 1,485). The release caller-buffer test received 1,571 triggered mock and 50 continuous simulator completions while both were streaming. That interval measured zero Iris heap calls and bytes. The fault CTI measured zero Iris allocations in nonblocking receive, release, and multipart poll. Named chunk reads were outside the allocation intervals. Strict C11 and C++17 header checks and Doxygen passed.

Layout checks used GCC 14.2 on Linux x86-64 with a 64-byte data cache line. The release library build ID was `98d38513182406714da87a06367554427b3de5ff`. The public frame descriptor stayed at 208 bytes and now groups image data, geometry, format, user pointer, status, and validity in its first 64 bytes. Its field offsets changed. The payload and part descriptors stayed at 80 and 104 bytes. The internal buffer slot dropped from 3,584 to 3,456 bytes and its part array starts at offset 128. Stream objects and buffer slots have 64-byte alignment. The contention test checks alignment of heap-allocated streams, buffer slots, and part arrays. ELF symbol inspection confirms separate cache-line placement of the token counter and the feature and registry mutexes. Caller and producer memory placement remains their responsibility. No cache-coherence counters or physical cameras were used.

## Ordered feature writes

The feature-batch test verified separate selector banks, a mid-batch range error with its failing index and completed prefix, and a competing selector writer. The fault CTI recorded both batch writes before the competing write. The test passed 30 consecutive release runs. The triggered software-trigger example configured its trigger with the batch API and captured one complete frame per command.

All 11 tests passed in release and coverage builds. Consumer line coverage was 94.9% (1,428 of 1,504). While both simulators streamed into caller-owned buffers, the release test delivered 1,520 triggered mock frames and 50 continuous simulator frames and measured zero Iris heap calls and bytes. Strict C11 and C++17 header checks and Doxygen passed.

## GenTL XML URL discovery and named registers

The register-backed CTI test skipped a web URL, an unreadable register extent, and malformed XML before selecting indexed URL 3. It verified the selected URL and advertised schema and file versions. Separate cases verified fallback to URL 1 in the legacy list and an escaped local file URL with a schema query. Register tests verified raw-byte reads and writes, two selector-dependent addresses, buffer sizing, node and device access rules, and rejection of a node without IRegister. The node-settings test also passed with UBSan.

All 11 tests passed in release and coverage builds. Consumer line coverage was 95.5% (1,586 of 1,660). The release caller-buffer run delivered 1,465 triggered mock frames and 50 continuous simulator frames while both streamed, with zero Iris heap calls and bytes during nonblocking capture. Strict C11 and C++17 header checks and Doxygen passed. The four new functions were present in the shared library's `IRIS_1` exports.

## Remote-device and module events

The register-backed simulator verified delivery through the remote device, system, interface, local device, and stream GenApi maps. It checked zero-timeout retrieval, lock contention, numeric and extended hexadecimal event IDs, decoded register readback, empty queues, missing payloads, malformed IDs, retryable subscription cleanup, and owner close. Its stream capture test measured zero Iris allocations with a module event subscription active. The focused event and contention tests also passed under UBSan.

All 12 tests passed in release and coverage builds. Consumer line coverage was 96.3% (1,721 of 1,788). Simultaneous caller-buffer capture delivered 1,566 triggered mock and 50 continuous simulator frames with zero measured Iris heap calls and bytes. An additional simulator probe found that the installed triggered and continuous CTIs returned `IRIS_NOT_SUPPORTED` for event subscriptions on all five maps, so event delivery and decoding were verified with the register-backed simulator. Strict C11 and C++17 header checks and Doxygen passed.

## Direct feature metadata and cache invalidation

The register-backed simulator verified lookup by feature name, integer hexadecimal representation, Float notation and precision, and the absence of numeric display metadata on an Enumeration. It checked that insufficient storage leaves the outputs untouched and that copied strings survive node-map closure. After out-of-band changes to two registers, named invalidation refreshed only the affected width constraint, and map invalidation refreshed the other cached constraint. The node-settings test also passed under UBSan.

A simulator probe verified named metadata and both invalidation calls on triggered mock `TriggerSelector` and continuous simulator `ExposureTime`. All 12 tests passed in release and coverage builds. Consumer line coverage was 95.9% (1,774 of 1,850). The simultaneous caller-buffer run delivered 1,551 triggered mock and 50 continuous simulator frames with zero Iris heap calls and bytes during nonblocking capture. Strict C11 and C++17 header checks and Doxygen passed.

## Independent GenApi review verification

The added regressions exercised standard and legacy feature-invalidation registration, stale backing-register values, retryable registration cleanup, malformed producer strings, read-only port access, oversized event queues, and chunk bounds when the producer reports DATA_SIZE only. The new event queue check covers both device events and feature invalidation. Four targeted tests passed under UBSan. Strict C11 and C++17 header checks and Doxygen passed.

The final release and coverage suites passed all 12 tests. Consumer line coverage was 95.9% (1,824 of 1,901). triggered mock and continuous simulator streamed simultaneously with caller-owned buffers, delivering 1,591 and 50 complete frames respectively with zero measured Iris heap calls or bytes. The buffer descriptor cache-line placement also matched the previously measured Linux x86-64 layout.

One earlier release run aborted in the continuous configuration example with `free(): invalid pointer`. The cause is unconfirmed. The example passed 50 subsequent isolated runs and both final suites. The mixed capture allocation checks passed in the run that observed the abort.

## Flat feature browsing and XML source inspection

The register-backed CTI verified name-sorted snapshots, glob and regex matching, combined type and availability filters, inclusion of internal nodes, and snapshot ownership after close. It verified a named alias, narrow and wider-than-64-bit event IDs, ambiguous event dependencies, String maximum length, and named XML lookup including a nested element and a repeated name. A deflated file-backed ZIP returned byte-identical XML after the original archive was removed. The browsing, node settings, and node-map event tests also passed under UBSan.

Direct simulator probes listed 13 triggered mock and 42 continuous simulator remote features and verified named source lookup for `TriggerSelector` and `ExposureTime`, respectively. All 13 tests passed in release and coverage builds. Consumer line coverage was 96.2% (1,998 of 2,076). The release caller-buffer run delivered 1,483 triggered mock and 50 continuous simulator frames while both streamed, with zero measured Iris heap calls or bytes during nonblocking capture. Strict C11 and C++17 header checks and Doxygen passed.

## Frame-scoped chunk browsing and typed reads

The payload fixture verified a sorted snapshot of readable chunk features for a live frame and read Integer, Float, Boolean, String, Enumeration, and Register values. It checked a 64-bit Integer above the exact double-precision range, caller buffer sizing, wrong-type and missing-feature errors, and exclusion of a feature with an unattached chunk dependency. The fixture changed the chunk layout on the next completion, rejected the old frame token, and verified that the original metadata snapshot survived frame release and stream close. It exercised listing and typed Integer reads on transport chunks, gapped and chunk-only multipart payloads, and GenDC metadata, including a GenDC borrow held through stop. Payload, node-settings, and feature-browse tests also passed under UBSan.

All 13 tests passed in release and coverage builds. Consumer line coverage was 96.4% (2,042 of 2,119). The release caller-buffer run delivered 1,560 triggered mock and 50 continuous simulator frames concurrently with zero measured Iris heap calls or bytes during nonblocking capture. The payload fixture independently measured zero Iris allocations during frame receive, release, and multipart poll. Chunk inspection ran outside those allocation intervals. Strict C11 and C++17 header checks and Doxygen passed.

## Owned event snapshots

The register-backed simulator polled two events with the same ID and verified that both snapshots retained their own raw bytes, GenApi values, and timestamps after subsequent polls, subscription close, and device teardown. It checked the monotonic host timestamp against the polling interval, a valid zero device timestamp, an exact device timestamp above the double-precision integer range, absent timestamps, 64-bit and extended hexadecimal event IDs, and per-feature errors on a short payload. The contention test verified that snapshot polling returns BUSY under the feature lock. The stream fixture captured with zero Iris allocations while a module event subscription was active, then checked the module snapshot after stream close.

All 13 tests passed in release and coverage builds. Consumer line coverage was 96.5% (2,151 of 2,229). The caller-buffer run delivered 1,545 triggered mock and 50 continuous simulator frames while both streamed, with zero Iris heap calls and bytes during nonblocking capture. Four focused simulator tests passed under UBSan. Strict C11 and C++17 header checks and Doxygen passed.

## Native GenApi write stacking

The register-backed simulator received four selector-dependent register writes in one `GCWritePortStacked` call and verified both selector banks. It exercised cancellation, empty stacks, immediate setters while a stack was active, a GenApi value error between groups, a producer failure after two writes, a short success count, an unsupported call, and a producer exception. No staged write went through the scalar port in these tests.

triggered mock and continuous simulator held separate stacks at the same time and confirmed one and two register writes respectively, with feature readback. The subsequent caller-buffer run delivered 1,612 triggered mock and 50 continuous simulator frames concurrently. The allocation probe measured zero Iris heap calls and zero bytes during nonblocking capture.

All 14 tests passed in release and coverage builds. Consumer line coverage was 96.4% (2,232 of 2,316). Write stacking, feature batch, and node settings passed under UBSan. Strict C11 and C++17 header checks and Doxygen passed.

## triggered native-ring mock

With `trigger_fixture_ZERO_COPY=1` and `trigger_fixture_RING_BUFFERS=4`, the regression originally reached the four-slot announcement and failed the subsequent `TLParamsLocked` write with `GC_ERR_ACCESS_DENIED`. triggered rejects idle settings writes while native slots are announced and manages the transport lock internally at stream start and stop. Iris now sets the lock before announcement and clears it after revocation. An oversized five-buffer request also exercised startup rollback and left the stream ready for a four-buffer start.

The release test captured 100 software-triggered triggered frames over 20 acquisition cycles while continuous simulator stayed started and delivered 30 frames with no observed ID gaps. The test verified no untriggered completions, distinct addresses for concurrent borrows, reuse of a released slot, readable held data after stop, and rejection of wrong-stream and stale releases. It checked both explicit pool retirement and retirement on restart after the final release. The allocation probe measured zero Iris heap calls and bytes during nonblocking receive, poll, and release windows. Trigger dispatch and start/stop ran outside those measurements.

The release test passed another 20 consecutive runs. All 15 tests passed in release and coverage builds, with 96.4% consumer line coverage (2,235 of 2,319). The native-ring test and failure recovery also passed under UBSan.

## Per-device remote GenApi XML override

The triggered mock used a temporary override with an extra Width feature. The test changed Width through that feature and read it back through the original feature, then inspected the loaded XML after deleting the file. continuous simulator used its producer description and reported the added feature as absent. The pair delivered 1,402 triggered mock and 50 simulator frames concurrently with caller-owned buffers. Nonblocking capture measured zero Iris heap calls and bytes.

The register-backed CTI verified a relative ZIP override, byte-identical source inspection, read-only access, and per-device isolation from a peer on the same producer and from the system map. Empty, malformed, and unavailable files failed during open and left the valid device open retryable. All 16 tests passed in release and coverage builds. Consumer line coverage was 96.5% (2,290 of 2,374). Five focused tests passed under UBSan. Strict C11 and C++17 header checks and Doxygen passed.
