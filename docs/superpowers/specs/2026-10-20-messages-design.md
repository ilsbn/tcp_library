# Typed message protocol

Add Header, UserIdentifier, Heartbeat, ProductItem, and ShoppingCart with the requested m_ field names. MessageType is scoped (Heartbeat=0, ShoppingCart=1) so its values can coexist with the structure names. SequenceNumber is uint64_t; PriceDollar remains float. Header exposes its fields publicly, including a size_t payload size.

The encoded buffer is a 20-byte header, payload, then four-byte CRC-32. Header fields are uint64 magic 0x5443504d53473031 (ASCII TCPMSG01), uint32 message type, and uint64 payload byte count. The wire size is fixed-width even though the C++ field uses size_t. All integer fields and IEEE-754 binary32 float bits use big-endian order. The existing TCP four-byte frame length encloses this complete encoded buffer.

Heartbeat payload: uint64 sequence, UserIdentifier. UserIdentifier: uint32 login byte count, raw string bytes, uint64 user ID. ShoppingCart payload: UserIdentifier, uint32 item count, then ordered ProductItems (uint64 product ID, binary32 price). Empty strings/carts and embedded zero bytes are supported. The codec preserves price bits and leaves application-level price policy to callers.

Use Boost.CRC crc_32_type (CRC-32/ISO-HDLC, polynomial 0x04c11db7, initial/xor 0xffffffff, reflected input/output). Its big-endian checksum trailer covers header plus payload. This checks accidental byte corruption. Decode verifies the checksum and exact lengths, known magic/type, string lengths, item count, complete fields, and no trailing bytes. Lengths/counts are checked against available bytes before allocating strings/list nodes. Decoder errors are std::invalid_argument and do not alter the caller's DataBuffer cursor. Outbound sizes must fit a uint32 TCP frame; oversized values throw std::length_error.

Expose encode_message overloads for Heartbeat, ShoppingCart, and MessageBody (variant), plus decode_message returning DecodedMessage with header, typed body, and verified checksum. Expose compute_crc32 for verification/diagnostics. Keep the generic TCP DataBuffer API. Add DataBuffer write_float32/read_float32 with existing per-call endian options and short-read guarantees.

Add an executable that sends both typed messages over loopback, validates at the server, and returns validated responses. Retain named class callbacks and Allman formatting. Tests include independent literal wire/checksum fixtures, malformed CRC-valid packets, corruption in every byte, round trips, real TCP delivery, and rejection of corrupted messages.
