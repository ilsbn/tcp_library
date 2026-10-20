# DataBuffer Implementation Plan

> Execute inline with the executing-plans workflow; preserve the existing networking ownership model.

**Goal:** Encode/decode fixed-width integer fields in owning byte buffers and use them as TCP message payloads.

**Architecture:** DataBuffer wraps vector<uint8_t> and a read cursor. Typed methods share unsigned integer encoding/decoding helpers; TCP frames remain four-byte big-endian length plus bytes.

**Tech stack:** Existing CMake/vcpkg, MSVC latest mode, standard C++ span/bit_cast, GoogleTest/GoogleMock.

**Spec:** ../specs/2026-10-19-data-buffer-design.md

## Global constraints

- Default ByteOrder::big_endian; allow ByteOrder::little_endian on individual operations.
- Support signed and unsigned 8/16/32/64-bit fields.
- Short reads throw std::out_of_range without advancing the cursor.
- TCP send transmits all encoded bytes; received buffers start at cursor zero.
- Keep named class callbacks, serialized writes, queue/message limits, and cancellation buffer ownership.

## Tasks

- [x] Add include/tcp_helper/data_buffer.hpp, tests/data_buffer_tests.cpp, and CMake test/source entries. Verify tests fail against nonfunctional method definitions before implementing the codec.
- [x] Implement src/data_buffer.cpp using fixed-width unsigned shifts and bit_cast for signed representations. Verify known big/little-endian byte fixtures, cursor failure behavior, raw append/read, aliasing, and copy/move invariants.
- [x] Change tcp.hpp, connection_state.hpp, and tcp_connection.cpp to DataBuffer payloads. Convert all test observers and examples. Add a real-socket mixed-endian structured message test; verify framing and existing cancellation tests.
- [x] Update README and project design. Run both build/test presets and install/compile/run a separate consumer using DataBuffer across the DLL boundary. Review the diff for bounds, invalidated spans, and asynchronous buffer lifetimes.

## API and test fixtures

```cpp
DataBuffer message;
message.write_uint32(0x12345678); // bytes 12 34 56 78
message.write_int16(-2, ByteOrder::little_endian); // bytes fe ff
EXPECT_EQ(message.read_uint32(), 0x12345678u);
EXPECT_EQ(message.read_int16(ByteOrder::little_endian), -2);
EXPECT_THROW(message.read_uint8(), std::out_of_range);
EXPECT_EQ(message.read_position(), 6u);
```

```powershell
cmake --build --preset debug
ctest --preset debug
cmake --build --preset release
ctest --preset release
```

Keep changes on feature/data-buffer. Do not commit until the completed code and checks are reviewed.

## Verification results

- Observed expected failures for network-order encoding, little-endian decoding, and short-read cursor behavior against the initial buffer skeleton.
- Debug and Release builds succeeded with 35/35 CTest checks passing in each: 16 codec cases, 17 TCP cases, and two executable integration checks.
- The structured TCP test verifies complete payload transmission after advancing the sender's cursor, received cursor zero, literal mixed-order payload bytes, and decoded fields.
- Installed the Release package, compiled a separate application against its installed headers/import library, and successfully exchanged typed DataBuffer fields through the deployed DLL.
- Independent read-only review found no issues in byte ordering, signed representations, span aliasing, bounds/cursor behavior, exports, network buffer lifetimes, or documentation.
