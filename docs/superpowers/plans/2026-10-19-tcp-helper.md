# TCP Helper Implementation Plan

> Execute inline using the executing-plans workflow. Track each deliverable below.

**Goal:** Create a usable MSVC TCP helper DLL, examples, and automated tests.

**Architecture:** One application-owned Asio event loop; named member handlers; weak event observers and shared asynchronous state. Length-prefixed connections are shared by the client and server.

**Tech stack:** CMake 3.30+, MSVC latest language mode, vcpkg x64-windows, Boost.Asio, spdlog, GoogleTest/GoogleMock.

**Spec:** ../specs/2026-10-19-tcp-helper-design.md

## Global constraints

- Use named class callback methods instead of nested lambdas.
- Build tcp_helper as SHARED with explicit Windows exports.
- Use /MDd in Debug and /MD in Release.
- All public operations, callbacks, and destruction use one event-loop thread.
- Default message/queue limits: 1 MiB / 4 MiB.

## Deliverables

- [x] Add manifest, Debug/Release presets, exported API, and tests for loopback exchange, exact framing, and failure callbacks. Configure/build and establish a failing baseline before implementing network behavior.
- [x] Implement shared connection framing/read/write/close and client resolve/connect plus multi-client accept/stop. Use std::bind for named handlers; retain in-flight buffers until cancellation completes. Build and run tests.
- [x] Add standalone echo server, echo client, and automatic smoke executable. Add CTest example smoke coverage, usage instructions, and packaging/install support. Build Debug and Release, run CTest, and review ownership/shutdown and DLL consumption.

## Verification commands

```powershell
cmake --preset windows-msvc
cmake --build --preset debug
ctest --preset debug
cmake --build --preset release
ctest --preset release
```

Review specifically for premature buffer release, duplicate disconnect callbacks, handler reference cycles, leaked server connections, unbounded test waits, DLL search paths, and accidental static MSVC runtime use.

## Verification results

- MSVC 19.51.36256.0, Windows SDK 10.0.26100.0, CMake 4.3.1-msvc1.
- Observed three expected failures against the initial nonfunctional API baseline.
- Final Debug and Release builds succeeded with 18/18 CTest checks passing in each configuration (16 GoogleTest cases and two executable integration checks).
- Independent code review checked cancellation, observer ownership, callback reentrancy, and packaging. The installed package now requires the exact build-time Boost version; explicit client/server destruction inside callbacks is tested.
- Installed the Release package and built a separate find_package consumer. After deploying tcp_helper/spdlog/fmt DLLs beside it, the consumer ran successfully.
- Confirmed generated MSVC projects select latest language mode and DLL runtimes; dumpbin confirms tcp_helper is a DLL with the expected runtime dependencies.
