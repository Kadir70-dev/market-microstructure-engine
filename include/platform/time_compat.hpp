#pragma once

#include <ctime>

namespace platform {

inline bool gmtime_utc(const std::time_t& value, std::tm& result) noexcept {
#if defined(_MSC_VER)
    return ::gmtime_s(&result, &value) == 0;
#else
    return ::gmtime_r(&value, &result) != nullptr;
#endif
}

inline std::time_t timegm_utc(std::tm& value) noexcept {
#if defined(_MSC_VER)
    return ::_mkgmtime(&value);
#else
    return ::timegm(&value);
#endif
}

}  // namespace platform
