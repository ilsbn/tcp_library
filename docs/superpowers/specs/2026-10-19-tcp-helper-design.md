# TCP helper design

Build a small MSVC DLL using Boost.Asio and spdlog, with vcpkg manifest dependencies and CMake presets. MSVC uses `/std:c++latest`; the code does not require experimental language features. All targets use the dynamic MSVC runtime, matching the x64-windows vcpkg triplet.

The public API contains TcpClient, TcpServer, TcpConnection, TcpEvents, TcpOptions, and DataBuffer. Applications supply one Boost.Asio io_context and implement named virtual callback methods in a TcpEvents subclass. Public operations and destruction run on the same thread as the event loop, or before it starts. There are no internal threads. Callbacks must not throw or block; catch DataBuffer decoding errors inside callbacks. Event handlers are weakly referenced, so applications retain their handler objects. Asynchronous operations retain their connection/state until completion.

Each client/server instance starts once; create a new instance to reconnect or restart. The server accepts multiple clients and removes closed connections. stop/close are idempotent and cancel pending operations. An established connection reports one disconnection callback; resolution and connection failures report an error callback. User-initiated close reports operation_aborted. Operations canceled by shutdown do not report spurious errors.

Messages use a four-byte unsigned big-endian payload length followed by DataBuffer bytes. Empty messages are allowed. DataBuffer supports encoding/decoding signed and unsigned integer fields with byte order selected per operation; see [the buffer design](2026-10-19-data-buffer-design.md). The default maximum message is 1 MiB, with a 4 MiB outbound queue limit. Reads reconstruct complete messages even when TCP splits or combines data. Writes execute serially in FIFO order. send returns false for closed connections, oversized payloads, or a full queue. An invalid inbound length closes the connection with message_size.

The DLL hides implementation details behind forward-declared state types and exports its public functions explicitly. It exposes C++ standard library and Boost types, so consumers use a matching MSVC toolset, runtime, architecture, and configuration.

Deliver two manual test programs (echo server and echo client) and one GoogleTest/GoogleMock executable. Tests use loopback and ephemeral ports, bounded event-loop waits, real sockets, and a mocked event observer. Cover ordered/binary/empty messages, multiple clients, fragmented/coalesced frames, bad lengths, refusal, cancellation, and stop behavior. CTest also runs an automatic smoke program and the command-line examples together.

TLS, automatic reconnect, protocol negotiation, internal thread pools, and raw-stream mode are outside this starter's scope.
