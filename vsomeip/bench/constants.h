#pragma once

#include <cstdint>

namespace vsomeip_bench {

inline constexpr std::uint16_t kServiceId = 0xBEEF;
inline constexpr std::uint16_t kInstanceId = 0x0001;
inline constexpr std::uint16_t kEventgroupId = 0x0001;
inline constexpr std::uint16_t kEventId = 0x8001;

inline constexpr std::uint16_t kTcpPort = 30509;
inline constexpr std::uint16_t kUdpPort = 30510;

}  // namespace vsomeip_bench
