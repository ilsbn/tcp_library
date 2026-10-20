# Typed Events and Boost.Endian Implementation Plan

> Execute inline using executing-plans; preserve named class callbacks and Allman formatting.

**Goal:** Applications exchange Heartbeat and ShoppingCart through transport-independent interfaces; Boost.Endian handles wire conversions.

**Architecture:** Keep RawTcpEvents for byte transport, adapt MessageEvents/TcpEvents internally, and implement IMessageSender in TcpConnection. Validate before typed dispatch. Application headers use std::error_code.

**Tech stack:** MSVC latest C++, CMake/vcpkg, Boost.Asio/CRC/Endian, spdlog, GoogleTest/GoogleMock.

**Spec:** ../specs/2026-10-20-typed-events-design.md

## Global constraints

- Preserve the existing big-endian packet layout and CRC-32.
- One io_context thread; retain current weak-observer and cancellation guarantees.
- Application interfaces have no TCP/Boost includes. No new lambdas in library or examples.
- Invalid received typed messages close with protocol_error before application delivery.

## Tasks

- [x] Add the interface declarations and failing tests for typed dispatch, automatic checksum rejection, and transport-independent mocks. Retain existing low-level coverage with RawTcpEvents.
- [x] Implement typed sending and internal observer adaptation; run focused tests, including reentrant shutdown and weak observer lifetime.
- [x] Replace manual byte conversion with Boost.Endian and add its private build dependency. Check existing literal packet fixtures and raw network-order frames in both directions.
- [x] Migrate examples and README, run all Debug/Release checks and installed consumer validation, review changes, and commit locally.

## Validation results (2026-10-20)

- Observed three expected assertion failures against the nonfunctional typed sender/receive baseline: typed exchange, corrupted packet rejection, and CRC-valid malformed payload rejection. These passed after implementation.
- All 73 CTest checks passed in both Debug and Release after the final ownership change. MSVC builds completed without warnings.
- Existing codec/CRC fixtures remained unchanged. Raw-peer tests verified exact network-order Heartbeat and ShoppingCart frames, including float bits, length prefix, and checksum, in both directions.
- Application interface and mock-sender tests compile without TCP/Boost headers. A separate CMake consumer linked only tcp_helper::tcp_helper, compiled a transport-independent application handler against installed headers, and exchanged both types through the final installed Release DLL.
- Independent review identified adapter expiry during callback-driven server destruction. A two-connection regression reproduced one notification instead of two before the fix. State-level adapter retention covers both explicit stop and accept-error shutdown; the regression and full suites pass. Focused follow-up review found no remaining issues.
- clang-format --style=file --dry-run --Werror and git diff --check passed.
