# Iris GenTL development

Iris GenTL is a Linux x86-64 GenTL Consumer. The public C API is in
`inc/iris.h`. The C++17 implementation is in `src/`, with private headers
in `inc/internal/`. Use GenTL and GenApi terminology in code and docs.

## API and acquisition

- Keep public headers usable from C and C++. Document ownership, lifetimes,
  threading, and return codes for every public declaration. The C ABI is
  provisional. Record public changes so clients can rebuild against the
  matching header and library.
- Keep C++ exceptions inside the C API boundary. Preserve producer diagnostics
  and the ability to retry a failed teardown.
- Preserve one reader per stream, bounded nonblocking polling, and zero Iris
  heap allocations during steady-state capture. Keep producer and callback
  allocations separate when measuring.
- Verify buffer ownership on success, rejection, and stop. Progressive capture
  requires a producer guarantee that the reported image prefix is CPU-visible
  and stable until the buffer is requeued.

## Build and validation

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

- GitHub CI uses the in-tree mock CTI. Use the
  [optional simulator settings](docs/guide.md#optional-external-simulators)
  for local triggered mock and continuous simulator qualification. Keep simulator and
  hardware results distinct.
- Put coverage build directories outside the workspace to keep gcov files there.
  The CMake `coverage` target requires above 90% line coverage for both the consumer
  and CLI. The public API reference can be checked with `doxygen docs/Doxyfile`.
- CMake downloads the public EMVA dependencies and checks their hashes. Keep
  EMVA runtime binaries unmodified in release packages and include their
  notices. Verify package installation with the target distribution before
  publishing. The release workflow downloads and tests the published assets.
  To repeat that check, dispatch `release.yml` with `verify_tag` set to a
  published tag. The workflows and packaging driver live in `.github/`.
- Keep local plans, benchmark logs, and generated build output out of commits.
  Record reproducible commands or user-facing behavior in the maintained docs.
