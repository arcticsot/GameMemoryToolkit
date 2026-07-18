#pragma once
#ifndef GMT_ALIGN_HPP
#define GMT_ALIGN_HPP

#include <cassert>   // assert
#include <cstdint>   // std::uintptr_t
#include <cstddef>   // std::size_t

/**
 * @brief Rounds an address up to the nearest multiple of the specified alignment.
 *
 * This function uses a highly efficient bitwise trick instead of the modulo operator.
 * It is idempotent, meaning that if the address is already aligned, it remains unchanged.
 *
 * @param address   The memory address to align, represented as an unsigned integer.
 * @param alignment The desired alignment boundary. Must be a power of two.
 * @return          The smallest multiple of `alignment` that is >= `address`.
 * @pre             `alignment` must be strictly greater than 0 and a perfect power of two.
 */
inline std::uintptr_t AlignUp(std::uintptr_t address, std::size_t alignment) {
    assert(alignment > 0 && (alignment & (alignment - 1)) == 0
           && "alignment must be a power of two");

    std::uintptr_t mask = static_cast<std::uintptr_t>(alignment) - 1;
    return (address + mask) & ~mask;
}

#endif
