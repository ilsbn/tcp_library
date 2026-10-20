# Message Recovery and Logging Implementation Plan

> Execute inline with executing-plans and test-driven development. Preserve Allman style and named callbacks.

**Goal:** Log structured invalid-message diagnostics, retain connections, and resynchronize using validated magic headers.

**Architecture:** Injectable diagnostics in TcpOptions; structured codec exceptions; a bounded MessageStreamDecoder used by the typed ConnectionState receive path. Preserve raw transport and the current wire format.

**Tech stack:** MSVC latest C++, Boost.Asio/Endian/CRC, spdlog, GoogleTest/GoogleMock, CMake/vcpkg.

**Spec:** ../specs/2026-10-20-message-recovery-design.md

## Tasks

- [x] Add diagnostics/logger interfaces and assertion-failing tests for precise codec errors, file logging, and recovery from corrupted streams.
- [x] Implement structured codec errors and the file logger. Verify exhaustive single-byte replacements and malformed CRC-valid field diagnostics.
- [x] Implement bounded stream resynchronization and integrate typed receive/dispatch with ConnectionState. Verify fragmented input, false magic, misleading lengths, and logger injection.
- [x] Update real TCP tests to require recovery on the same connection, including a valid message after corruption in both directions. Update README/design documentation.
- [x] Run Debug/Release checks and installed-consumer validation, request independent review, resolve findings, and commit locally.

## Validation results

- Observed five initial assertion failures before implementing diagnostics and recovery.
- Checked 28,050 single-byte replacements across both message types, 2,040 frame-length byte replacements, bit bursts, and insertion/deletion/truncation/swap cases.
- Real TCP checks cover all 880 single-bit message errors followed by valid delivery on the same connection, plus both receive directions and end-of-stream recovery.
- Independent review found repeated candidate diagnostics and missed end-of-stream recovery behind a large incomplete frame. Regression tests reproduced both failures; both fixes passed and the follow-up review had no remaining findings.
- Final MSVC Debug: 100/100 CTest checks passed in 6.93 seconds.
- Final MSVC Release: 100/100 CTest checks passed in 6.04 seconds.
- Reinstalled the Release DLL and rebuilt/ran the independent CMake consumer successfully.
- All C++ sources passed clang-format verification; local documentation links and Git whitespace checks passed.

The wire format is unchanged. Recovery is best effort, with bounded buffering and a final scan when the peer ends the stream. See README.md for the incomplete-frame and embedded-candidate limitations.
