#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include <gmt/align.hpp>

#include <cstdint>   // std::uintptr_t (used directly in edge cases)
#include <cstddef>   // std::size_t   (used in the property-based loop)

TEST_CASE("base case: an address in the middle rounds up") {
    // 0x1000 is divisible by 8, so 0x1003 must rise to the next multiple (0x1008)
    CHECK(AlignUp(0x1003, 8) == 0x1008);
    // 10 aligned to 16 must become 16
    CHECK(AlignUp(10, 16) == 16);
    // 0x2001 aligned to 4 becomes 0x2004
    CHECK(AlignUp(0x2001, 4) == 0x2004);
}

TEST_CASE("idempotence: an already-aligned address does not move") {
    // A wrong implementation (e.g. addr + align) would fail here.
    // Correct bitwise handling guarantees idempotence.
    CHECK(AlignUp(0x1008, 8) == 0x1008);
    CHECK(AlignUp(16, 16) == 16);
    CHECK(AlignUp(0x2000, 32) == 0x2000);
    CHECK(AlignUp(1024, 1024) == 1024);
}

TEST_CASE("identity: alignment of 1 never changes anything") {
    // Every number is a multiple of 1 (2^0)
    CHECK(AlignUp(0, 1) == 0);
    CHECK(AlignUp(1, 1) == 1);
    CHECK(AlignUp(0x1003, 1) == 0x1003);
    CHECK(AlignUp(0xFFFFFFFF, 1) == 0xFFFFFFFF);
}

TEST_CASE("large alignments hold beyond 8") {
    // 0x1000 (4096) is a multiple of 64, so 0x1001 goes to the next block: 0x1040
    CHECK(AlignUp(0x1001, 64) == 0x1040);
    // Already aligned to 64
    CHECK(AlignUp(0x1040, 64) == 0x1040);

    CHECK(AlignUp(10, 256) == 256);
    CHECK(AlignUp(4000, 4096) == 4096);
    CHECK(AlignUp(4097, 4096) == 8192);
}

TEST_CASE("edge cases") {
    // Address == 0 (zero is a multiple of anything)
    CHECK(AlignUp(0, 8) == 0);
    CHECK(AlignUp(0, 4096) == 0);
    // An address just below a multiple
    CHECK(AlignUp(7, 8) == 8);
    CHECK(AlignUp(15, 16) == 16);
    CHECK(AlignUp(0x0FFF, 4096) == 0x1000);
    // A large but valid address
    std::uintptr_t high_addr = 0xFFFFFFF0; // a multiple of 16
    CHECK(AlignUp(high_addr - 1, 16) == high_addr);
}

TEST_CASE("property: the result holds for ANY input") {
    // Double loop verifying the mathematical invariants of AlignUp.
    for (std::size_t align = 1; align <= 4096; align *= 2) {
        for (std::uintptr_t addr = 0; addr <= 500; ++addr) {
            const auto r = AlignUp(addr, align);

            // 1. Must never go backwards
            CHECK(r >= addr);

            // 2. Must be a perfect multiple of the alignment
            //    (we use standard % in the test to VERIFY the internal bitwise logic)
            CHECK(r % align == 0);

            // 3. Must be the nearest multiple (distance strictly less than alignment)
            CHECK(r - addr < align);
        }
    }
}
