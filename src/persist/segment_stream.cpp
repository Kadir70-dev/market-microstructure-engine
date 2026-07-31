#include "persist/segment_stream.hpp"

namespace persist {

RawFileStream::~RawFileStream() noexcept { close(); }

bool RawFileStream::open(const std::filesystem::path& path) noexcept {
    close();
    std::error_code error;
    const auto file_size = std::filesystem::file_size(path, error);
    if (error) return false;

    // Fixed buffer installed before open() so libstdc++/MSVC use it rather than
    // allocating their own. Keeps steady-state memory O(1) in segment size.
    input_.rdbuf()->pubsetbuf(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
    input_.open(path, std::ios::binary | std::ios::in);
    if (!input_.is_open()) return false;

    size_ = file_size;
    position_ = 0;
    open_ = true;
    return true;
}

std::size_t RawFileStream::read(void* destination, std::size_t size) noexcept {
    if (!open_ || destination == nullptr || size == 0) return 0;
    input_.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
    const auto produced = static_cast<std::size_t>(input_.gcount());
    position_ += produced;
    // A short read sets failbit alongside eofbit; clear it so a caller may keep
    // querying position() and size() after reaching the end.
    if (produced < size) input_.clear(input_.rdstate() & ~std::ios::failbit);
    return produced;
}

bool RawFileStream::skip(std::uint64_t size) noexcept {
    if (!open_) return false;
    if (size == 0) return true;
    if (position_ + size > size_) return false;
    input_.seekg(static_cast<std::streamoff>(size), std::ios::cur);
    if (!input_) return false;
    position_ += size;
    return true;
}

void RawFileStream::close() noexcept {
    if (input_.is_open()) input_.close();
    input_.clear();
    position_ = 0;
    size_ = 0;
    open_ = false;
}

}  // namespace persist
