#include "persist/wal_writer.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <vector>

#include <zstd.h>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace persist {
namespace {

#pragma pack(push, 1)
struct BoundarySlot final {
    std::array<char, 8> magic;
    std::uint64_t generation;
    std::uint64_t committed;
    std::uint32_t crc;
    std::array<std::uint8_t, 4> reserved;
};
struct BoundaryMetadata final { std::array<BoundarySlot, 2> slots; };
#pragma pack(pop)
static_assert(sizeof(BoundarySlot) == 32);
static_assert(sizeof(BoundaryMetadata) == 64);

BoundarySlot make_slot(std::uint64_t generation, std::uint64_t committed) noexcept {
    BoundarySlot slot{{'M','M','E','B','N','D','0','1'}, generation, committed, 0, {}};
    slot.crc = crc32(&slot, offsetof(BoundarySlot, crc));
    return slot;
}
bool valid_slot(const BoundarySlot& slot) noexcept {
    constexpr std::array<char, 8> magic{'M','M','E','B','N','D','0','1'};
    return slot.magic == magic && slot.crc == crc32(&slot, offsetof(BoundarySlot, crc));
}
std::filesystem::path metadata_path(const std::filesystem::path& path) {
    return std::filesystem::path(path.string() + ".meta");
}
bool truncate_file(const std::filesystem::path& path, std::uint64_t size) noexcept {
    std::error_code error;
    std::filesystem::resize_file(path, size, error);
    return !error;
}
bool write_recovered_metadata(const std::filesystem::path& path,
                              std::uint64_t generation, std::uint64_t boundary) noexcept {
    BoundaryMetadata metadata{{make_slot(generation + 1, boundary), make_slot(generation + 2, boundary)}};
    std::ofstream output(metadata_path(path), std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(&metadata), sizeof(metadata));
    output.flush();
    return output.good();
}

}  // namespace

struct WalWriter::Impl final {
    std::byte* data{nullptr};
    BoundaryMetadata* metadata{nullptr};
    std::size_t mapped_size{0};
#if defined(_WIN32)
    HANDLE file{INVALID_HANDLE_VALUE};
    HANDLE mapping{nullptr};
    HANDLE metadata_file{INVALID_HANDLE_VALUE};
    HANDLE metadata_mapping{nullptr};
#else
    int file{-1};
    int metadata_file{-1};
#endif
};

WalWriter::WalWriter() noexcept : impl_(new (std::nothrow) Impl{}) {}
WalWriter::~WalWriter() noexcept { close(); delete impl_; }

bool WalWriter::open(const WalConfig& config, const WalFileHeader& header,
                     std::uint64_t monotonic_now_ns) noexcept {
    close();
    if (impl_ == nullptr || config.segment_data_bytes < max_frame_size ||
        config.segment_data_bytes > default_segment_data_bytes) return false;
    config_ = config;
    header_ = finalize_header(header);
    segment_index_ = config.first_segment_index;
    std::error_code error;
    std::filesystem::create_directories(config_.directory, error);
    return !error && open_segment(monotonic_now_ns);
}

bool WalWriter::open_segment(std::uint64_t monotonic_now_ns) noexcept {
    char name[40]{};
    std::snprintf(name, sizeof(name), "%020llu.wal",
                  static_cast<unsigned long long>(segment_index_));
    current_path_ = config_.directory / name;
    const auto meta_path = metadata_path(current_path_);
    const std::uint64_t total_size = wal_header_size + config_.segment_data_bytes;
    impl_->mapped_size = static_cast<std::size_t>(total_size);
#if defined(_WIN32)
    // FILE_SHARE_READ (was 0, exclusive): a live/growing segment must stay
    // readable by a tailer -- including one in this same process -- while
    // the writer still holds it open. FILE_SHARE_WRITE is deliberately not
    // added: this process is the only writer for its own segment.
    impl_->file = CreateFileW(current_path_.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (impl_->file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{}; size.QuadPart = static_cast<LONGLONG>(total_size);
    if (!SetFilePointerEx(impl_->file, size, nullptr, FILE_BEGIN) || !SetEndOfFile(impl_->file)) return false;
    impl_->mapping = CreateFileMappingW(impl_->file, nullptr, PAGE_READWRITE, size.HighPart, size.LowPart, nullptr);
    if (!impl_->mapping) return false;
    impl_->data = static_cast<std::byte*>(MapViewOfFile(impl_->mapping, FILE_MAP_ALL_ACCESS, 0, 0, total_size));
    if (!impl_->data) return false;
    // Same reasoning: WalReader::resolve_boundary() opens this .meta sidecar
    // via a plain std::ifstream while this writer handle is still open.
    impl_->metadata_file = CreateFileW(meta_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                                       nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (impl_->metadata_file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER meta_size{}; meta_size.QuadPart = sizeof(BoundaryMetadata);
    if (!SetFilePointerEx(impl_->metadata_file, meta_size, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(impl_->metadata_file)) return false;
    impl_->metadata_mapping = CreateFileMappingW(impl_->metadata_file, nullptr, PAGE_READWRITE, 0,
                                                  sizeof(BoundaryMetadata), nullptr);
    if (!impl_->metadata_mapping) return false;
    impl_->metadata = static_cast<BoundaryMetadata*>(MapViewOfFile(
        impl_->metadata_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BoundaryMetadata)));
    if (!impl_->metadata) return false;
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    auto* pages = reinterpret_cast<volatile std::byte*>(impl_->data);
    for (std::size_t offset = 0; offset < impl_->mapped_size; offset += info.dwPageSize) {
        const auto value = pages[offset];
        pages[offset] = value;
    }
#else
    impl_->file = ::open(current_path_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (impl_->file < 0 || ftruncate(impl_->file, static_cast<off_t>(total_size)) != 0) return false;
    impl_->data = static_cast<std::byte*>(mmap(nullptr, total_size, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_POPULATE, impl_->file, 0));
    if (impl_->data == MAP_FAILED) { impl_->data = nullptr; return false; }
    impl_->metadata_file = ::open(meta_path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (impl_->metadata_file < 0 || ftruncate(impl_->metadata_file, sizeof(BoundaryMetadata)) != 0) return false;
    impl_->metadata = static_cast<BoundaryMetadata*>(mmap(nullptr, sizeof(BoundaryMetadata),
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, impl_->metadata_file, 0));
    if (impl_->metadata == MAP_FAILED) { impl_->metadata = nullptr; return false; }
#endif
    std::memcpy(impl_->data, &header_, sizeof(header_));
    const BoundaryMetadata metadata{{make_slot(1, 0), make_slot(2, 0)}};
    std::memcpy(impl_->metadata, &metadata, sizeof(metadata));
    metadata_generation_ = 2;
    committed_.store(0, std::memory_order_release);
    opened_at_ns_ = monotonic_now_ns;
    return flush();
}

AppendResult WalWriter::append(std::uint16_t type, std::uint16_t flags,
    const void* payload, std::uint32_t payload_length) noexcept {
    if (impl_ == nullptr || impl_->data == nullptr) return AppendResult::io_error;
    if (!valid_payload_length(payload_length)) return AppendResult::invalid_payload;
    std::array<std::byte, max_frame_size> frame{};
    const auto frame_size = encode_frame(frame.data(), frame.size(), type, flags, payload, payload_length);
    if (frame_size == 0) return AppendResult::invalid_payload;
    const auto boundary = committed_.load(std::memory_order_relaxed);
    if (boundary + frame_size > config_.segment_data_bytes) return AppendResult::rotation_required;
    std::memcpy(impl_->data + wal_header_size + boundary, frame.data(), frame_size);
    if (!publish_boundary(boundary + frame_size)) return AppendResult::io_error;
    return AppendResult::committed;
}

bool WalWriter::publish_boundary(std::uint64_t boundary) noexcept {
    const auto slot = make_slot(++metadata_generation_, boundary);
    std::memcpy(&impl_->metadata->slots[metadata_generation_ & 1U], &slot, sizeof(slot));
    committed_.store(boundary, std::memory_order_release);
    return true;
}

bool WalWriter::needs_rotation(std::uint64_t now_ns, std::size_t payload_length) const noexcept {
    const auto next = frame_prefix_size + payload_length + frame_crc_size;
    return committed_.load(std::memory_order_acquire) + next > config_.segment_data_bytes ||
        (now_ns >= opened_at_ns_ && now_ns - opened_at_ns_ >=
         static_cast<std::uint64_t>(config_.rotation_interval.count()));
}

bool WalWriter::flush() noexcept {
    if (impl_ == nullptr || impl_->data == nullptr || impl_->metadata == nullptr) return false;
    const auto bytes = wal_header_size + committed_.load(std::memory_order_acquire);
#if defined(_WIN32)
    if (!FlushViewOfFile(impl_->data, bytes) || !FlushFileBuffers(impl_->file)) return false;
    if (!FlushViewOfFile(impl_->metadata, sizeof(BoundaryMetadata)) ||
        !FlushFileBuffers(impl_->metadata_file)) return false;
#else
    if (msync(impl_->data, bytes, MS_SYNC) != 0 || fsync(impl_->file) != 0) return false;
    if (msync(impl_->metadata, sizeof(BoundaryMetadata), MS_SYNC) != 0 ||
        fsync(impl_->metadata_file) != 0) return false;
#endif
    return true;
}

bool WalWriter::rotate(std::uint64_t monotonic_now_ns,
                       std::filesystem::path& closed_segment) noexcept {
    if (!flush()) return false;
    closed_segment = current_path_;
    const auto final_size = wal_header_size + committed_.load(std::memory_order_acquire);
    close_mapping();
    if (!truncate_file(closed_segment, final_size)) return false;
    ++segment_index_;
    return open_segment(monotonic_now_ns);
}

void WalWriter::close_mapping() noexcept {
    if (impl_ == nullptr) return;
#if defined(_WIN32)
    if (impl_->metadata) { UnmapViewOfFile(impl_->metadata); impl_->metadata = nullptr; }
    if (impl_->metadata_mapping) { CloseHandle(impl_->metadata_mapping); impl_->metadata_mapping = nullptr; }
    if (impl_->metadata_file != INVALID_HANDLE_VALUE) { CloseHandle(impl_->metadata_file); impl_->metadata_file = INVALID_HANDLE_VALUE; }
    if (impl_->data) { UnmapViewOfFile(impl_->data); impl_->data = nullptr; }
    if (impl_->mapping) { CloseHandle(impl_->mapping); impl_->mapping = nullptr; }
    if (impl_->file != INVALID_HANDLE_VALUE) { CloseHandle(impl_->file); impl_->file = INVALID_HANDLE_VALUE; }
#else
    if (impl_->metadata) { munmap(impl_->metadata, sizeof(BoundaryMetadata)); impl_->metadata = nullptr; }
    if (impl_->metadata_file >= 0) { ::close(impl_->metadata_file); impl_->metadata_file = -1; }
    if (impl_->data) { munmap(impl_->data, impl_->mapped_size); impl_->data = nullptr; }
    if (impl_->file >= 0) { ::close(impl_->file); impl_->file = -1; }
#endif
}

void WalWriter::close() noexcept {
    std::filesystem::path ignored;
    (void)finalize(ignored);
}

bool WalWriter::finalize(std::filesystem::path& closed_segment) noexcept {
    if (impl_ == nullptr || impl_->data == nullptr) return true;
    if (!flush()) return false;
    const auto final_size = wal_header_size + committed_.load(std::memory_order_acquire);
    closed_segment = current_path_;
    close_mapping();
    return truncate_file(closed_segment, final_size);
}

RecoveryResult WalWriter::recover(const std::filesystem::path& segment) noexcept {
    RecoveryResult result{};
    std::ifstream meta(metadata_path(segment), std::ios::binary);
    BoundaryMetadata metadata{};
    meta.read(reinterpret_cast<char*>(&metadata), sizeof(metadata));
    if (!meta) return result;
    BoundarySlot selected{};
    bool found = false;
    for (const auto& slot : metadata.slots) {
        if (valid_slot(slot) && (!found || slot.generation > selected.generation)) {
            selected = slot; found = true;
        }
    }
    if (!found || selected.committed > default_segment_data_bytes) return result;
    result.committed_boundary = selected.committed;
    std::ifstream input(segment, std::ios::binary);
    WalFileHeader header{};
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!input || !valid_header(header)) return result;
    input.seekg(0, std::ios::end);
    const auto file_size = static_cast<std::uint64_t>(input.tellg());
    if (file_size < wal_header_size) return result;
    const auto physical_data = file_size - wal_header_size;
    input.seekg(wal_header_size, std::ios::beg);
    std::array<std::byte, max_frame_size> frame{};
    std::uint64_t position = 0;
    while (position < selected.committed) {
        if (selected.committed - position < frame_prefix_size ||
            physical_data - std::min(position, physical_data) < frame_prefix_size) break;
        FramePrefix prefix{};
        input.read(reinterpret_cast<char*>(&prefix), sizeof(prefix));
        if (!input || !valid_payload_length(prefix.payload_length)) break;
        const auto frame_size = frame_prefix_size + prefix.payload_length + frame_crc_size;
        if (frame_size > selected.committed - position || frame_size > physical_data - position) break;
        std::memcpy(frame.data(), &prefix, sizeof(prefix));
        input.read(reinterpret_cast<char*>(frame.data() + frame_prefix_size),
                   prefix.payload_length + frame_crc_size);
        if (!input) break;
        std::uint32_t stored_crc = 0;
        std::memcpy(&stored_crc, frame.data() + frame_prefix_size + prefix.payload_length, sizeof(stored_crc));
        if (crc32(frame.data(), frame_prefix_size + prefix.payload_length) != stored_crc) break;
        position += frame_size;
        ++result.valid_records;
    }
    result.valid_boundary = position;
    result.truncated = position != selected.committed || file_size != wal_header_size + position;
    input.close();
    if (result.truncated && !truncate_file(segment, wal_header_size + position)) return result;
    if (!write_recovered_metadata(segment, selected.generation, position)) return result;
    result.success = true;
    return result;
}

bool WalWriter::compress_closed_segment(const std::filesystem::path& segment,
                                        std::filesystem::path& output) noexcept {
    std::error_code error;
    const auto source_size = std::filesystem::file_size(segment, error);
    if (error) return false;
    const auto temporary = std::filesystem::path(segment.string() + ".zst.tmp");
    const auto completed = std::filesystem::path(segment.string() + ".zst");
    std::ifstream input(segment, std::ios::binary);
    std::ofstream compressed(temporary, std::ios::binary | std::ios::trunc);
    if (!input || !compressed) return false;
    ZSTD_CCtx* context = ZSTD_createCCtx();
    if (!context) return false;
    bool ok = !ZSTD_isError(ZSTD_CCtx_setPledgedSrcSize(context, source_size)) &&
              !ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, 3));
    // zstd streaming chunks. Heap-owned, not automatic: two 1 MiB arrays are a
    // 2 MiB frame, and every caller runs on a 1 MiB stack (the PE reserve, which
    // the compressor thread inherits too), so stack residency overflows on entry.
    // The 1 MiB chunk size is preserved exactly, keeping read/write boundaries
    // and therefore the compressed output byte-identical.
    using ChunkBuffer = std::array<char, 1 << 20>;
    const std::unique_ptr<ChunkBuffer> in_owner(new (std::nothrow) ChunkBuffer{});
    const std::unique_ptr<ChunkBuffer> out_owner(new (std::nothrow) ChunkBuffer{});
    if (!in_owner || !out_owner) { ZSTD_freeCCtx(context); return false; }
    auto& in_buffer = *in_owner;
    auto& out_buffer = *out_owner;
    while (ok && input) {
        input.read(in_buffer.data(), in_buffer.size());
        ZSTD_inBuffer in{in_buffer.data(), static_cast<std::size_t>(input.gcount()), 0};
        const auto mode = input.eof() ? ZSTD_e_end : ZSTD_e_continue;
        do {
            ZSTD_outBuffer out{out_buffer.data(), out_buffer.size(), 0};
            const auto remaining = ZSTD_compressStream2(context, &out, &in, mode);
            if (ZSTD_isError(remaining)) { ok = false; break; }
            compressed.write(out_buffer.data(), static_cast<std::streamsize>(out.pos));
            if (mode == ZSTD_e_end && remaining == 0) break;
        } while (in.pos < in.size || mode == ZSTD_e_end);
    }
    ZSTD_freeCCtx(context);
    compressed.flush(); compressed.close(); input.close();
    if (!ok) { std::filesystem::remove(temporary, error); return false; }

    std::ifstream verify(temporary, std::ios::binary);
    ZSTD_DCtx* decoder = ZSTD_createDCtx();
    if (!verify || !decoder) return false;
    std::uint64_t decoded_size = 0;
    std::uint32_t source_crc = 0, decoded_crc = 0;
    std::ifstream raw(segment, std::ios::binary);
    while (raw) { raw.read(in_buffer.data(), in_buffer.size()); const auto n = raw.gcount(); if (n > 0) source_crc = crc32(in_buffer.data(), static_cast<std::size_t>(n), source_crc); }
    while (ok && verify) {
        verify.read(in_buffer.data(), in_buffer.size());
        ZSTD_inBuffer in{in_buffer.data(), static_cast<std::size_t>(verify.gcount()), 0};
        while (in.pos < in.size) {
            ZSTD_outBuffer out{out_buffer.data(), out_buffer.size(), 0};
            const auto remaining = ZSTD_decompressStream(decoder, &out, &in);
            if (ZSTD_isError(remaining)) { ok = false; break; }
            decoded_crc = crc32(out_buffer.data(), out.pos, decoded_crc);
            decoded_size += out.pos;
        }
    }
    ZSTD_freeDCtx(decoder);
    // Release the read handle before rename/remove: Windows refuses both with
    // ERROR_SHARING_VIOLATION while the stream is open. POSIX permits it.
    verify.close();
    ok = ok && decoded_size == source_size && decoded_crc == source_crc;
    if (!ok) { std::filesystem::remove(temporary, error); return false; }
    std::filesystem::rename(temporary, completed, error);
    if (error) return false;
    output = completed;
    return std::filesystem::exists(segment) && std::filesystem::exists(completed);
}

}  // namespace persist
