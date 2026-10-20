# DataBuffer design

Replace the TCP string payload API with an owning DataBuffer used for encoding and decoding binary messages. Keep named event callbacks and the existing four-byte big-endian TCP frame length.

DataBuffer owns a vector of uint8_t bytes and an independent read cursor. Supply named write_uint8/int8/uint16/int16/uint32/int32/uint64/int64 methods and corresponding read methods. Each operation accepts ByteOrder::big_endian (default) or ByteOrder::little_endian. Encode fixed-width integer values without struct padding or alignment assumptions; preserve signed two's-complement representations, including minima. Writes append and do not advance the read cursor. Reads return values and advance only on success. A short read throws std::out_of_range without consuming bytes.

Provide construction from a byte span or an owned vector, a read-only bytes() span, size/empty/read_position/remaining, reserve, clear, rewind, write_bytes, and read_bytes. Byte reads return read-only views whose lifetime ends on buffer mutation/destruction. Appending bytes from the buffer itself must be safe. Copying preserves bytes and cursor. Moving leaves the source empty with cursor zero. Equality compares encoded bytes, independent of the read cursor.

TcpConnection::send accepts DataBuffer by value. TcpEvents::on_message receives DataBuffer by value with cursor zero. Sending uses the complete byte array even when its cursor has advanced. Incoming storage and outbound frames remain alive until asynchronous operations complete. Preserve all existing message size and queue limits, empty payload support, and cancellation guarantees.

Update both echo programs, the binary smoke program, GoogleTest/GoogleMock observers, raw framing tests, README, and the original project design. The smoke program should demonstrate actual typed field encoding/decoding. No new dependencies, internal threads, or callback lambdas.

Verify literal wire bytes for both byte orders, mixed-order fields, signed limits, raw bytes, read bounds/cursor invariants, self-append, copy/move behavior, and a structured message sent through the real TCP library. Run Debug and Release CTest and an installed-package consumer.
