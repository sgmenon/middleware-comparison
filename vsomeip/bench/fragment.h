#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <vector>

namespace vsomeip_bench {

inline constexpr std::uint32_t kFragMagic = 0x56424652u;  // 'VBFR'

#pragma pack(push, 1)
struct FragHeader {
    std::uint32_t magic = kFragMagic;
    std::uint32_t message_id = 0;
    std::uint16_t index = 0;
    std::uint16_t count = 1;
    std::uint32_t total_length = 0;
};
#pragma pack(pop)

inline std::size_t MaxFragmentPayload(std::size_t max_datagram) {
    if (max_datagram <= sizeof(FragHeader)) {
        return 0;
    }
    return max_datagram - sizeof(FragHeader);
}

inline std::vector<std::vector<std::uint8_t>> FragmentPayload(std::uint32_t message_id, const std::uint8_t* data,
                                                              std::size_t length, std::size_t max_datagram) {
    const std::size_t chunk = MaxFragmentPayload(max_datagram);
    if (chunk == 0 || length <= chunk) {
        std::vector<std::uint8_t> single(length);
        if (length > 0) {
            std::memcpy(single.data(), data, length);
        }
        return {std::move(single)};
    }
    const std::uint16_t count =
        static_cast<std::uint16_t>((length + chunk - 1) / chunk);
    std::vector<std::vector<std::uint8_t>> out;
    out.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::size_t off = static_cast<std::size_t>(i) * chunk;
        const std::size_t n = std::min(chunk, length - off);
        std::vector<std::uint8_t> buf(sizeof(FragHeader) + n);
        FragHeader hdr{};
        hdr.message_id = message_id;
        hdr.index = i;
        hdr.count = count;
        hdr.total_length = static_cast<std::uint32_t>(length);
        std::memcpy(buf.data(), &hdr, sizeof(hdr));
        std::memcpy(buf.data() + sizeof(hdr), data + off, n);
        out.push_back(std::move(buf));
    }
    return out;
}

class Reassembler {
public:
    // Returns complete logical payload when all fragments arrived; empty optional otherwise.
    std::optional<std::vector<std::uint8_t>> ingest(const std::uint8_t* data, std::size_t length) {
        if (length < sizeof(FragHeader)) {
            return std::nullopt;
        }
        FragHeader hdr{};
        std::memcpy(&hdr, data, sizeof(hdr));
        if (hdr.magic != kFragMagic || hdr.count == 0) {
            if (length == 0) {
                return std::nullopt;
            }
            std::vector<std::uint8_t> raw(length);
            std::memcpy(raw.data(), data, length);
            return raw;
        }
        const std::size_t payload_len = length - sizeof(FragHeader);
        auto& partial = partial_[hdr.message_id];
        if (partial.total == 0) {
            partial.total = hdr.total_length;
            partial.count = hdr.count;
            partial.parts.resize(hdr.count);
            partial.have = 0;
        }
        if (hdr.index >= partial.parts.size()) {
            return std::nullopt;
        }
        if (partial.parts[hdr.index].empty()) {
            partial.parts[hdr.index].assign(data + sizeof(FragHeader), data + length);
            ++partial.have;
        }
        if (partial.have < partial.count) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> assembled(partial.total);
        std::size_t off = 0;
        for (const auto& p : partial.parts) {
            if (off + p.size() > assembled.size()) {
                partial_ = {};
                return std::nullopt;
            }
            std::memcpy(assembled.data() + off, p.data(), p.size());
            off += p.size();
        }
        partial_.erase(hdr.message_id);
        return assembled;
    }

private:
    struct Partial {
        std::uint32_t total = 0;
        std::uint16_t count = 0;
        std::uint16_t have = 0;
        std::vector<std::vector<std::uint8_t>> parts;
    };
    std::map<std::uint32_t, Partial> partial_;
};

}  // namespace vsomeip_bench
