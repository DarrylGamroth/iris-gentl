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

- The default tests use the repository's mock CTI. Use the simulator options in
  [the user guide](docs/guide.md#simulator-tests) when a change needs triggered mock
  and continuous simulator coverage. Keep simulator and hardware results distinct.
- For coverage builds, the CMake `coverage` target enforces 90% consumer line
  coverage. The public API reference can be checked with
  `doxygen docs/Doxyfile`.
- CMake downloads the public EMVA dependencies and checks their hashes. Keep
  EMVA runtime binaries unmodified in release packages and include their
  notices. Verify package installation with the target distribution before
  publishing. The GitHub workflows and packaging driver live in `.github/`.
- Keep local plans, benchmark logs, and generated build output out of commits.
  Record reproducible commands or user-facing behavior in the maintained docs.
