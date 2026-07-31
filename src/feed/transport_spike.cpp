#include "feed/transport_spike.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "feed/mt5_protocol.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>

namespace {

using feed::mt5::SpikeMessage;
using Clock = std::chrono::steady_clock;

SpikeMessage make_message(std::uint64_t sequence) noexcept {
    SpikeMessage message{};
    message.prefix = {feed::mt5::protocol_magic, feed::mt5::protocol_version,
                      static_cast<std::uint16_t>(feed::mt5::RecordType::spike_message), 1, sequence};
    message.payload_sequence = sequence;
    for (std::size_t i = 0; i < message.payload.size(); ++i)
        message.payload[i] = static_cast<std::uint8_t>((sequence + i) & 0xffU);
    message.checksum = feed::mt5::spike_checksum(message);
    return message;
}

bool valid_message(const SpikeMessage& message, std::uint64_t sequence) noexcept {
    return message.prefix.magic == feed::mt5::protocol_magic &&
           message.prefix.version == feed::mt5::protocol_version &&
           message.payload_sequence == sequence &&
           message.checksum == feed::mt5::spike_checksum(message);
}

double percentile(std::vector<double>& values, double q) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(q * static_cast<double>(values.size() - 1));
    return values[index];
}

double process_cpu_seconds() noexcept {
    FILETIME create{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user)) return 0.0;
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return static_cast<double>(k.QuadPart + u.QuadPart) / 10000000.0;
}

feed::transport_spike::Metrics finish_metrics(const char* name, std::size_t samples,
                                               std::vector<double>& latencies,
                                               Clock::duration elapsed,
                                               double cpu_before, double cpu_after,
                                               std::uint64_t corruptions,
                                               bool publication_proven) {
    const double seconds = std::chrono::duration<double>(elapsed).count();
    return {name, static_cast<std::uint64_t>(samples),
            seconds > 0.0 ? static_cast<double>(samples) / seconds : 0.0,
            percentile(latencies, 0.50), percentile(latencies, 0.95),
            percentile(latencies, 0.99),
            seconds > 0.0 ? ((cpu_after - cpu_before) / seconds) * 100.0 : 0.0,
            corruptions, true, publication_proven};
}

bool read_exact(HANDLE handle, void* data, DWORD size) noexcept {
    DWORD done = 0;
    return ReadFile(handle, data, size, &done, nullptr) && done == size;
}
bool write_exact(HANDLE handle, const void* data, DWORD size) noexcept {
    DWORD done = 0;
    return WriteFile(handle, data, size, &done, nullptr) && done == size;
}

struct alignas(64) FenceFreeSlot final {
    volatile LONG state;
    SpikeMessage message;
};

}  // namespace
#endif

namespace feed::transport_spike {

Metrics benchmark_named_pipe(std::size_t samples) noexcept {
#if defined(_WIN32)
    Metrics failed{"named_pipe"};
    if (samples == 0) return failed;
    const std::string name = "\\\\.\\pipe\\mme_spike_" + std::to_string(GetCurrentProcessId());
    HANDLE server = CreateNamedPipeA(name.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
        sizeof(SpikeMessage) * 4, sizeof(SpikeMessage) * 4, 0, nullptr);
    if (server == INVALID_HANDLE_VALUE) return failed;

    std::atomic<std::uint64_t> corruptions{0};
    std::thread responder([&] {
        const bool connected = ConnectNamedPipe(server, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected) { corruptions.fetch_add(1, std::memory_order_relaxed); return; }
        for (std::size_t i = 0; i < samples; ++i) {
            SpikeMessage message{};
            if (!read_exact(server, &message, sizeof(message)) ||
                !valid_message(message, i) ||
                !write_exact(server, &message, sizeof(message)))
                corruptions.fetch_add(1, std::memory_order_relaxed);
        }
    });
    HANDLE client = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
    if (client == INVALID_HANDLE_VALUE) {
        CloseHandle(server); responder.join(); return failed;
    }
    std::vector<double> latencies;
    latencies.reserve(samples);
    const double cpu_before = process_cpu_seconds();
    const auto benchmark_start = Clock::now();
    for (std::size_t i = 0; i < samples; ++i) {
        const auto message = make_message(i);
        const auto start = Clock::now();
        SpikeMessage reply{};
        if (!write_exact(client, &message, sizeof(message)) ||
            !read_exact(client, &reply, sizeof(reply)) || !valid_message(reply, i))
            corruptions.fetch_add(1, std::memory_order_relaxed);
        latencies.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
    }
    const auto elapsed = Clock::now() - benchmark_start;
    const double cpu_after = process_cpu_seconds();
    CloseHandle(client);
    responder.join();
    DisconnectNamedPipe(server);
    CloseHandle(server);
    return finish_metrics("named_pipe", samples, latencies, elapsed,
                          cpu_before, cpu_after, corruptions.load(std::memory_order_relaxed), true);
#else
    (void)samples;
    return {"named_pipe", 0, 0, 0, 0, 0, 0, 0, false, false};
#endif
}

Metrics benchmark_fence_free_shared_memory(std::size_t samples) noexcept {
#if defined(_WIN32)
    Metrics failed{"fence_free_shm"};
    if (samples == 0) return failed;
    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, sizeof(FenceFreeSlot), nullptr);
    if (!mapping) return failed;
    auto* slot = static_cast<FenceFreeSlot*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS,
                                                            0, 0, sizeof(FenceFreeSlot)));
    if (!slot) { CloseHandle(mapping); return failed; }
    std::memset(slot, 0, sizeof(*slot));
    std::atomic<std::uint64_t> corruptions{0};
    std::thread reader([&] {
        for (std::size_t i = 0; i < samples; ++i) {
            while (slot->state != 1) std::this_thread::yield();
            SpikeMessage copy{};
            std::memcpy(&copy, &slot->message, sizeof(copy));
            if (!valid_message(copy, i)) corruptions.fetch_add(1, std::memory_order_relaxed);
            slot->state = 0;  // Deliberately no atomic or fence: spike candidate only.
        }
    });
    std::vector<double> latencies;
    latencies.reserve(samples);
    const double cpu_before = process_cpu_seconds();
    const auto benchmark_start = Clock::now();
    for (std::size_t i = 0; i < samples; ++i) {
        while (slot->state != 0) std::this_thread::yield();
        const auto message = make_message(i);
        const auto start = Clock::now();
        std::memcpy(&slot->message, &message, sizeof(message));
        slot->state = 1;  // Deliberately no atomic or fence: MQL5 cannot provide one.
        while (slot->state != 0) std::this_thread::yield();
        latencies.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
    }
    const auto elapsed = Clock::now() - benchmark_start;
    const double cpu_after = process_cpu_seconds();
    reader.join();
    UnmapViewOfFile(slot);
    CloseHandle(mapping);
    // Empirical success cannot prove publication safety. This must remain false
    // because the MQL5 writer has no atomic store or memory-fence primitive.
    return finish_metrics("fence_free_shm", samples, latencies, elapsed,
                          cpu_before, cpu_after, corruptions.load(std::memory_order_relaxed), false);
#else
    (void)samples;
    return {"fence_free_shm", 0, 0, 0, 0, 0, 0, 0, false, false};
#endif
}

}  // namespace feed::transport_spike
