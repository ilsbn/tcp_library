# TCP helper

A small C++ DLL for asynchronous, message-based TCP clients and servers. Built with MSVC, CMake, vcpkg, Boost.Asio/CRC/Endian, and spdlog. Application callbacks are virtual class methods, and internal Asio handlers are named member functions bound with `std::bind`.

See the [design overview](docs/design.md) for the class relationships, message flow, and ownership rules.

C++ formatting follows `.clang-format`: four-space indentation and Allman braces on separate lines. Use clang-format or your editor's format command to keep this style consistent.

## Build on Windows

Install Visual Studio 2026 with **Desktop development with C++**, its Windows SDK and CMake tools. The supplied VS 2026 preset requires CMake 4.2 or newer; the project itself supports CMake 3.30 or newer with a compatible generator. Set `VCPKG_ROOT` to a vcpkg installation (Visual Studio's bundled vcpkg works). Run in a Developer PowerShell where `cmake` and `ctest` are on PATH:

```powershell
cmake --preset windows-msvc
cmake --build --preset debug
ctest --preset debug

cmake --build --preset release
ctest --preset release
```

The first configure downloads/builds dependencies. The manifest pins their versions to a vcpkg baseline. Boost.Asio optional features are disabled; GoogleTest includes GoogleMock through the `tests` manifest feature. MSVC uses `/std:c++latest`, `/W4`, `/permissive-`, and the DLL runtime (`/MDd` or `/MD`).

To use Visual Studio 2022, make a local `CMakeUserPresets.json` inheriting `windows-msvc`, override `generator` with `Visual Studio 17 2022`, and choose a different `binaryDir`. In Visual Studio, open this folder and select the configure preset.

## Run and debug in VS Code

Open this folder in VS Code with Microsoft's **C/C++** and **CMake Tools** extensions installed. Set `VCPKG_ROOT` as described above and select the `windows-msvc` configure preset in CMake Tools. If CMake Tools cannot find CMake, set its **CMake Path** setting to your CMake executable.

Choose a target in **Run and Debug**, then press **F5** to debug or **Ctrl+F5** to run. The entries in [.vscode/launch.json](.vscode/launch.json) cover all six executables: the message demo server/client, echo server/client, smoke test, and GoogleTest/GoogleMock tests. Each uses [.vscode/tasks.json](.vscode/tasks.json) to build the `debug` preset before starting. Output appears in the integrated terminal; the working directory is the project root, so message error logs appear in `logs/`.

For the message demos, start **TCP: message demo server (Debug)** first, then **TCP: message demo client (Debug)** as another debug session. Both use port `10101`. The separate echo entries use port `7777`; start the echo server before its client. Edit each entry's `args` to change connection details. The smoke test runs its own client and server. Stop the demos with Ctrl+C in their terminals or the debugger's Stop button. To run selected unit tests, add a flag such as `--gtest_filter=MessageStream.*` to the test entry's `args`.

## Run the programs

In one terminal:

```powershell
.\out\build\windows-msvc\bin\Debug\tcp_echo_server.exe 7777
```

In another:

```powershell
.\out\build\windows-msvc\bin\Debug\tcp_echo_client.exe 127.0.0.1 7777 "Hello TCP"
```

The server binds to loopback by default. Press Ctrl+C to stop it. Port `0` selects a free port; `--once` exits after a client disconnects. The client sends a Heartbeat with the text in its login name, checks the typed echo, and exits with a nonzero code on failure or timeout.

`tcp_smoke_test.exe` runs a server/client exchange in one process. `tcp_helper_tests.exe` runs GoogleTest/GoogleMock checks and supports normal flags such as `--gtest_filter=TcpTest.*`. CTest runs both, and also exercises the separate example executables when Python 3 is available.

The message demo is split into [message_demo_server.cpp](examples/message_demo_server.cpp) and [message_demo_client.cpp](examples/message_demo_client.cpp). Start the server in one terminal:

```powershell
.\out\build\windows-msvc\bin\Debug\tcp_message_demo_server.exe 10101
```

Start the client in another terminal:

```powershell
.\out\build\windows-msvc\bin\Debug\tcp_message_demo_client.exe 127.0.0.1 10101
```

Both sides send a Heartbeat every second. The client sends one ShoppingCart, the server processes and echoes it, and the client verifies the returned values. Heartbeats are collected rather than echoed, so the peers do not generate an echo loop. The demos keep running until Ctrl+C; the client also exits when its server disconnects. Server port `0` selects a free port and prints `LISTENING <port>`.

Each program follows the same polling structure: `AsioContext` runs one background TCP event thread, callbacks collect messages under a mutex in `TcpDataCollection`, and the main loop calls `fetch()` every 100 ms to retrieve a connection snapshot and clear the collected messages. Put application processing in that loop. The example server tracks one active client's snapshot. Log lines include thread IDs.

The shared [message_demo_common.hpp](examples/message_demo_common.hpp) keeps context management and snapshot types consistent. Its named send handlers run on the TCP thread, so the main loop uses `context.send_heartbeat()` and `context.send_cart()` instead of calling sender methods directly. `TcpSession` stops the client/server on the TCP thread, releases the work guard, drains cancellation handlers, and joins the threads before the transport object is destroyed.

For a finite exchange, add `--once` after the server port and after the client's host and port. The client then exits after receiving a Heartbeat and matching cart reply, with a five-second deadline; the server exits after its client disconnects. With Python 3 available, CTest runs this two-process exchange and checks connection failure handling and separation of TCP callbacks from the main polling loops.

## Typed API and ownership

Application code includes `<tcp_helper/message_channel.hpp>` and implements `MessageEvents`. This header uses message types and standard-library interfaces without including TCP or Boost. `TcpEvents` is an alias for `MessageEvents`, available from `<tcp_helper/tcp.hpp>`.

```cpp
#include <tcp_helper/message_channel.hpp>

class EchoHandler final : public tcp_helper::MessageEvents
{
public:
    void on_connected(tcp_helper::MessageSenderPtr sender) override
    {
        // Retain sender if the application wants to send later.
    }
    void on_heartbeat(tcp_helper::MessageSenderPtr sender, const tcp_helper::Heartbeat& message) override
    {
        if (!sender->send(message))
        {
            sender->close();
        }
    }
    void on_shopping_cart(tcp_helper::MessageSenderPtr sender, const tcp_helper::ShoppingCart& message) override
    {
        if (!sender->send(message))
        {
            sender->close();
        }
    }
    void on_disconnected(tcp_helper::MessageSenderPtr, std::error_code reason) override
    {
        // Transport closure is reported here; invalid messages are logged and skipped.
    }
    void on_error(std::string_view operation, std::error_code error) override
    {
    }
};
```

`MessageSenderPtr` owns an `IMessageSender`. It exposes `send(const Heartbeat&)`, `send(const ShoppingCart&)`, `close()`, and `is_open()`. It can be mocked or implemented by another transport. The application receives already decoded, checksum-verified messages. Message references last for the callback; copy values that need to be retained. Sending encodes synchronously and owns the resulting bytes, so the caller may change or destroy its message after `send()` returns.

Transport setup stays in bootstrap code, which includes `<tcp_helper/tcp.hpp>` and links `tcp_helper::tcp_helper`:

```cpp
boost::asio::io_context context;
auto handler = std::make_shared<EchoHandler>();
tcp_helper::TcpServer server{context, handler};
if (const auto error = server.start("127.0.0.1", 7777))
{
    throw boost::system::system_error(error);
}
context.run();
```

The [application echo handler](examples/echo_message_events.hpp) uses only the message interface. See the complete [server](examples/echo_server.cpp), [client](examples/echo_client.cpp), and polling message demo [server](examples/message_demo_server.cpp)/[client](examples/message_demo_client.cpp) for transport setup, shutdown, and timeouts. The [mock sender test](tests/message_channel_tests.cpp) compiles and tests the application interface without TCP headers.

- Run **one thread** in the TCP `io_context`. Call sender methods and destroy client/server objects on that thread, or before/after it runs. From another thread, use `boost::asio::post` with a named member handler, as shown in the message demos. Keep the context alive until all clients, servers, and retained senders have been destroyed.
- Retain your shared `MessageEvents` object while you want callbacks. The library holds a weak reference so retaining a sender in a handler does not create a cycle. Callbacks must return promptly and must not throw.
- Each client/server starts once. Create another instance to reconnect or restart. A second start/connect is rejected. `stop()` and `close()` are idempotent.
- `on_connected` supplies a shared sender. The client/server retains active connections; asynchronous work retains its state until completion. `on_disconnected` reports one terminal reason per connection, including local close. Typed callbacks use `std::error_code`; resolve/connect failures arrive through `on_error`. Bind/listen setup errors return directly from `start()` as `boost::system::error_code`.
- `stop()` cancels pending operations and closes established connections. Continue running/polling the context to drain cancellation completions before destroying it. Some callbacks, including local disconnection, run synchronously during `close()`/`stop()`.
- A true `send()` result means queued, not acknowledged by the peer. False means closed, encoded size exceeds the configured limit, or the queue is full; no partial message is queued. Encoding may throw for allocation failure or an unrepresentable wire length.
- The typed receiver validates messages before dispatch. Invalid data is logged and skipped; the connection stays open while the receiver searches for the next valid message. Socket errors, peer closure, and explicit `close()`/`stop()` still terminate connections.

For custom byte protocols, implement `RawTcpEvents` and use the matching client/server constructor. Its `on_message(ConnectionPtr, DataBuffer)` callback and `TcpConnection::send(DataBuffer)` retain the low-level buffer API and Boost error codes. Raw observers perform their own application-protocol validation.

Migration: typed handlers use `on_heartbeat` and `on_shopping_cart`, `MessageSenderPtr`, and `std::error_code`. Existing byte handlers can change their base class from `TcpEvents` to `RawTcpEvents`. Rebuild applications against the updated DLL and headers.

## Invalid data logging and resynchronization

Typed receive errors go to `logs/tcp_helper_errors.log`, relative to the working directory. The default spdlog logger appends and flushes each error. Choose another path or inject your own implementation through `TcpOptions`:

```cpp
tcp_helper::TcpOptions options;
options.error_logger = tcp_helper::make_file_message_error_logger("logs/server-errors.log");
tcp_helper::TcpServer server{context, handler, options};
```

Implement `IMessageErrorLogger::log(const MessageErrorDetails&) noexcept` to capture errors in tests or forward them to another logger. Assign the implementation to `options.error_logger`. The library retains the shared logger. See the [capturing and GoogleMock loggers](tests/stream_decoder_tests.cpp) and [TCP recovery checks](tests/typed_tcp_tests.cpp).

Each record contains a stable `MessageErrorCode`, description, `connection_id`, `stream_offset`, `byte_offset`, `received_size`, and optional `expected`/`actual` numeric values. `stream_offset` identifies the start of the message or frame prefix in the received byte stream. `byte_offset` identifies the field being validated relative to that start; a checksum failure points to the checksum field rather than identifying which byte was damaged. `received_size` records bytes available to that validation.

| Error code | Meaning |
| --- | --- |
| `invalid_message_size` | Standalone encoded packet has an invalid total size. |
| `checksum_mismatch` | Computed and received CRC-32 differ. |
| `invalid_magic` | Expected message magic is absent. |
| `unknown_message_type` | Message header declares an unsupported type. |
| `payload_size_mismatch` | Payload length disagrees with available bytes or the configured limit. |
| `truncated_field` | A required sequence/user/count field is incomplete. |
| `invalid_login_length` | Login byte count exceeds the available field data. |
| `invalid_item_count` | Product count exceeds the available product bytes. |
| `trailing_payload` | Extra bytes remain after decoding the declared message type. |
| `invalid_frame_length` | Outer frame length is outside the configured limits. |
| `frame_length_mismatch` | Outer frame length disagrees with the message header. |
| `truncated_frame` | The peer ends the stream with incomplete frame/recovery data. |

Recovery searches for the existing `TCPMSG01` magic after data fails validation. A mismatching magic byte or unsupported type starts recovery as soon as those bytes arrive, without waiting for the rest of the advertised frame. A candidate must have a supported type, bounded size, valid CRC, and valid payload before reaching an application callback. A damaged outer frame length can be bypassed using a valid message header. No wire-format change is required. Receive storage is bounded to the configured message limit (at least 24 bytes), a four-byte prefix, and a 4096-byte read chunk.

Resynchronization is best effort: a plausible incomplete frame waits for more bytes; end-of-stream performs a final scan for valid messages behind it. Magic bytes alone inside a payload do not trigger delivery. While out of alignment, a complete valid encoded message embedded in arbitrary data is indistinguishable from a recovery candidate.

`decode_message()` throws `MessageDecodeError`, derived from `std::invalid_argument`; `details()` exposes the same structured diagnostic. File-open failures throw during logger setup. Runtime file-write failures are reported to stderr without escaping the receive callback. `RawTcpEvents` retains its custom-protocol framing and validation policy.

## Encode and decode with DataBuffer

`DataBuffer` owns a `std::vector<std::uint8_t>` and tracks an independent read position. Include `<tcp_helper/data_buffer.hpp>` for the buffer alone, or `<tcp_helper/tcp.hpp>` for the TCP API as well.

```cpp
using tcp_helper::ByteOrder;
using tcp_helper::DataBuffer;

DataBuffer message;
message.write_uint32(42);                         // big-endian by default
message.write_int16(-7, ByteOrder::little_endian);
message.write_uint8(0);
// Encoded bytes: 00 00 00 2a f9 ff 00

// These same reads apply to the DataBuffer received in on_message().
const auto id = message.read_uint32();
const auto value = message.read_int16(ByteOrder::little_endian);
const auto marker = message.read_uint8();
// id == 42, value == -7, marker == 0, message.remaining() == 0
```

In the raw API, pass the buffer to `connection->send(std::move(message))`. The whole byte array is sent even if you have already read some fields from it. `RawTcpEvents::on_message` receives an owning buffer with its read position at zero.

- Named read/write methods support signed and unsigned 8-, 16-, 32-, and 64-bit integers. Every method accepts an optional byte order. The format contains exactly those bytes; it does not add type or byte-order tags. Readers must use the same field order, widths, and byte orders as writers.
- `write_float32()` and `read_float32()` preserve IEEE-754 binary32 float bits, with the same byte-order and cursor rules as integer operations.
- Writes append bytes and leave the read position alone. Reads advance it only on success. A short read throws `std::out_of_range` without consuming any bytes. Raw protocol handlers should catch decoding exceptions inside callbacks and close/reject malformed messages.
- `bytes()` returns a read-only span of the entire array. `write_bytes(span)` appends raw bytes, including views into the same buffer. `read_bytes(count)` returns a read-only view and advances the cursor; it adds no field length. Views remain valid until the buffer is mutated or destroyed.
- Construct from a byte span to copy existing data, or from `std::vector<std::uint8_t>` to take ownership of a moved vector. Copying a buffer preserves its cursor and owns separate storage. Moving leaves the source empty and reusable. Equality compares bytes regardless of cursor position.
- `size()`, `empty()`, `read_position()`, and `remaining()` inspect the buffer. `rewind()` resets its read position; `clear()` removes all bytes and resets it. `reserve()` preallocates storage for encoding.

## TCP frame format

Every message is `[4-byte unsigned big-endian payload length][payload bytes]`. Empty raw messages and embedded zero bytes work. The receiver reconstructs a complete message independently of TCP packet/read boundaries. Typed observers receive validated message callbacks; raw observers receive `on_message`. Both peers must use this framing; a plain telnet/netcat stream does not implement it.

`TcpOptions` defaults to a 1 MiB payload limit and a 4 MiB outbound queue per connection (including frame headers). Limits apply to each peer's configuration. An oversized incoming typed frame is logged and triggers resynchronization; raw framing closes it with `message_size`. An oversized outgoing message is rejected. Backpressure is explicit through the return value of `send()`.

Encode structure fields with `DataBuffer` and decode them after receipt. Payload fields can mix byte orders; the TCP frame length always stays big-endian. Native struct padding and pointer fields are not part of this wire format.

## Typed messages and checksum

Include `<tcp_helper/messages.hpp>` for `Header`, `UserIdentifier`, `Heartbeat`, `ProductItem`, `ShoppingCart`, and the encode/decode functions. Field names follow the `m_` convention in these types. `SequenceNumber` is `std::uint64_t`, and `PriceDollar` is `float`. The scoped `MessageType` enum uses `MessageType::Heartbeat` (0) and `MessageType::ShoppingCart` (1).

```cpp
using namespace tcp_helper;

Heartbeat heartbeat{1, {"alice", 42}};
ShoppingCart cart{{"alice", 42}, {{10001, 12.50F}, {10002, 3.25F}}};

auto encoded = encode_message(cart);
auto decoded = decode_message(encoded);
const auto& received_cart = std::get<ShoppingCart>(decoded.m_body);
```

Application code calls `sender->send(heartbeat)` or `sender->send(cart)` on the TCP thread. The library encodes, computes the checksum, verifies incoming packets, and dispatches `on_heartbeat` or `on_shopping_cart`. The message demo [server](examples/message_demo_server.cpp) and [client](examples/message_demo_client.cpp) use named class callbacks without manual encoding/decoding. The standalone codec functions above remain available for file storage, diagnostics, and raw protocol tools.

`encode_message` builds the header and checksum automatically. `decode_message` returns a `DecodedMessage` containing `m_header`, `m_body` (`std::variant<Heartbeat, ShoppingCart>`), and verified `m_checksum`. Encoding also accepts this `MessageBody` variant, which is useful for forwarding a decoded message. Decoding examines all bytes and preserves the input buffer's read position.

Each encoded message has this layout, enclosed in the existing TCP frame:

| Field | Wire bytes | Representation |
| --- | ---: | --- |
| `Header::m_magic_number` | 8 | `0x5443504d53473031`, ASCII `TCPMSG01` |
| `Header::m_type` | 4 | Unsigned message type: 0 or 1 |
| `Header::m_size` | 8 | Unsigned payload byte count |
| Payload | variable | Fields described below |
| Checksum | 4 | CRC-32 of the header and payload |

All protocol fields use network (big-endian) byte order. [Boost.Endian](https://www.boost.org/doc/libs/latest/libs/endian/doc/html/endian.html) performs integer and TCP frame-length loads/stores; float bits use the same uint32 conversion. Encoded send/receive buffers stay in network order. Decoded message fields contain ordinary C++ numeric values in host order. DataBuffer still permits explicit little-endian fields for other byte formats. `Header::m_size` is a `size_t` in C++ and a fixed `uint64_t` on the wire; it excludes the 20-byte header and four-byte checksum. The encoded buffer must fit the transport's configured `max_message_size` (1 MiB by default).

- `UserIdentifier`: uint32 login byte count, raw login bytes, uint64 user ID. Empty names and embedded zero bytes are supported; string bytes are preserved without adding a terminator.
- `Heartbeat`: uint64 sequence number, then `UserIdentifier`.
- `ProductItem`: uint64 product ID, then IEEE-754 binary32 price bits (12 bytes per item).
- `ShoppingCart`: `UserIdentifier`, uint32 item count, then the ordered list of `ProductItem` values. Empty carts work. Float prices retain their bits; application-specific price rules belong in the receiving application.

The checksum uses [Boost.CRC's `crc_32_type`](https://www.boost.org/latest/doc/html/crc/reference.html): CRC-32/ISO-HDLC with polynomial `0x04c11db7`, initial/final XOR `0xffffffff`, and reflected input/output. It checks accidental byte corruption. The decoder also rejects unknown magic/type, incorrect lengths, incomplete fields, impossible item counts, and trailing payload bytes. Lengths and counts are checked against the available data before string/list allocation. Standalone decoding of invalid messages throws `MessageDecodeError` (also a `std::invalid_argument`); outbound lengths exceeding the uint32 TCP frame limit throw `std::length_error`.

## Use in another CMake project

Inside a parent project:

```cmake
add_subdirectory(path/to/tcp_helper)
target_link_libraries(my_app PRIVATE tcp_helper::tcp_helper)
```

Or install the library:

```powershell
cmake --install out/build/windows-msvc --config Release --prefix out/install
```

Then configure the consumer with the same vcpkg toolchain and the install prefix in `CMAKE_PREFIX_PATH`:

```cmake
find_package(tcp_helper CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE tcp_helper::tcp_helper)
```

Deploy `tcp_helper.dll` and its spdlog/fmt runtime dependencies alongside your executable. vcpkg automatically copies required DLLs beside the built examples/tests. Consumers must match the MSVC toolset, architecture, runtime, Debug/Release configuration, and Boost version because this is a C++ API. The installed CMake package requires the exact Boost version used to build the DLL. To build only the DLL, configure a fresh build directory with `-DBUILD_TESTING=OFF -DTCP_HELPER_BUILD_EXAMPLES=OFF`; the GoogleTest dependency is enabled only when testing is on.

TLS, automatic reconnect, raw-stream mode, and internal worker threads are deliberately outside this starter's scope.
