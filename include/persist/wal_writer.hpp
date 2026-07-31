#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "persist/framing.hpp"

namespace persist {

inline constexpr std::uint64_t default_segment_data_bytes = 256ULL * 1024ULL * 1024ULL;

enum class AppendResult : std::uint8_t { committed, rotation_required, invalid_payload, io_error };

struct WalConfig final {
    std::filesystem::path directory;
    std::uint64_t segment_data_bytes{default_segment_data_bytes};
    std::chrono::nanoseconds rotation_interval{std::chrono::hours{1}};
    std::chrono::milliseconds flush_interval{std::chrono::milliseconds{100}};
    std::uint64_t first_segment_index{0};
};

struct RecoveryResult final {
    bool success{false};
    std::uint64_t committed_boundary{0};
    std::uint64_t valid_boundary{0};
    std::uint64_t valid_records{0};
    bool truncated{false};
};

class WalWriter final {
public:
    WalWriter() noexcept;
    ~WalWriter() noexcept;
    WalWriter(const WalWriter&) = delete;
    WalWriter& operator=(const WalWriter&) = delete;

    [[nodiscard]] bool open(const WalConfig& config, const WalFileHeader& header,
                            std::uint64_t monotonic_now_ns) noexcept;
    [[nodiscard]] AppendResult append(std::uint16_t type, std::uint16_t flags,
        const void* payload, std::uint32_t payload_length) noexcept;
    [[nodiscard]] bool needs_rotation(std::uint64_t monotonic_now_ns,
                                      std::size_t next_payload_length = fixed_event_payload_size) const noexcept;
    [[nodiscard]] bool rotate(std::uint64_t monotonic_now_ns,
                              std::filesystem::path& closed_segment) noexcept;
    [[nodiscard]] bool flush() noexcept;
    [[nodiscard]] bool finalize(std::filesystem::path& closed_segment) noexcept;
    void close() noexcept;

    [[nodiscard]] std::uint64_t committed_bytes() const noexcept {
        return committed_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t segment_capacity() const noexcept { return config_.segment_data_bytes; }
    [[nodiscard]] std::chrono::milliseconds flush_interval() const noexcept { return config_.flush_interval; }
    [[nodiscard]] const std::filesystem::path& current_path() const noexcept { return current_path_; }

    [[nodiscard]] static RecoveryResult recover(const std::filesystem::path& segment) noexcept;
    [[nodiscard]] static bool compress_closed_segment(const std::filesystem::path& segment,
                                                       std::filesystem::path& output) noexcept;

private:
    struct Impl;
    [[nodiscard]] bool open_segment(std::uint64_t monotonic_now_ns) noexcept;
    [[nodiscard]] bool publish_boundary(std::uint64_t boundary) noexcept;
    void close_mapping() noexcept;

    WalConfig config_{};
    WalFileHeader header_{};
    std::filesystem::path current_path_{};
    std::uint64_t segment_index_{0};
    std::uint64_t opened_at_ns_{0};
    std::uint64_t metadata_generation_{0};
    std::atomic<std::uint64_t> committed_{0};
    Impl* impl_{nullptr};
};

}  // namespace persist
