#pragma once

#include <bit>
#include <cstring>
#include <type_traits>

namespace wavplay::core {

static_assert(std::endian::native == std::endian::little, "WAV Player targets little-endian x64");

// RIFF fields and LSB samples are little-endian. memcpy permits unaligned fields
// and compiles to a single load or store for register-sized types.
template<class T>
    requires std::is_trivially_copyable_v<T>
[[nodiscard]] T load_le(const void* bytes) noexcept
{
    T value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

template<class T>
    requires std::is_trivially_copyable_v<T>
void store_le(void* bytes, const T& value) noexcept
{
    std::memcpy(bytes, &value, sizeof(value));
}

} // namespace wavplay::core
