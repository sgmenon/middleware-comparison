#pragma once

#include <cstddef>
#include <cstdint>

namespace vsomeip_bench {

inline constexpr std::uint16_t kServiceId = 0xBEEF;
inline constexpr std::uint16_t kInstanceId = 0x0001;
inline constexpr std::uint16_t kEventgroupId = 0x0001;
inline constexpr std::uint16_t kEventId = 0x8001;
// Echo method for round-trip checks (request payload is returned unchanged).
inline constexpr std::uint16_t kEchoMethodId = 0x0001;

inline constexpr std::uint16_t kUdpPort = 30510;

// Logical user frame: send_ns [0,8), seq [8,12), fill [12,size).
inline constexpr std::size_t kBenchSendNsOffset = 0;
inline constexpr std::size_t kBenchSeqOffset = 8;
inline constexpr std::size_t kBenchHeaderBytes = 12;

// P04 header on the wire (both stacks); fragments must leave room for it within --max-datagram.
inline constexpr std::size_t kE2eHeaderBytes = 12;

// COVESA in-place P04: every notified payload must start with a 12-byte hole for protect, and the
// subscriber skips it on receive (crc_offset 64 is relative to the request-id field, i.e. payload byte 0).
// sgmenon: the plugin adds/strips E2E itself, so payloads are hole-free.
#if defined(VSOMEIP_BENCH_COVESA_E2E_HOLES)
inline constexpr std::size_t kE2eHoleBytes = kE2eHeaderBytes;
#else
inline constexpr std::size_t kE2eHoleBytes = 0;
#endif

}  // namespace vsomeip_bench
