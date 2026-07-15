# Chapter 3 — Alignment and Pointer Arithmetic

> **In one sentence:** an allocator is, at bottom, a function that rounds an address upward. This chapter is that function.

**What you will learn**

- Why the hardware insists on aligned addresses — and what it costs you when you ignore it.
- How the CPU actually reads memory, and why scattered data is slow regardless of how fast your allocator is.
- The bit trick that rounds an address up to a boundary without a single division.
- How to do pointer arithmetic in C++ **legally** — and why the obvious way is not.

By the end you will have everything required to write `Allocate(size, alignment)`. **You will write it.**

---

## 3.1 Why the Hardware Wants Aligned Addresses

Begin with the definition, because it is simpler than it sounds.

> An address is **aligned to N** when it is an exact multiple of N — that is, when `address % N == 0`.

A 4-byte `int` "wants" to sit at an address divisible by 4. An 8-byte `double` wants one divisible by 8. This is the type's **natural alignment**, and on x86-64 it happens to equal the type's size — a convenient coincidence, not a law. (It does not hold on every ABI; ask `alignof`, never `sizeof`.)

### Why does the CPU care?

Because **the CPU does not read memory one byte at a time.** It reads in fixed-size chunks, aligned to their own boundaries — 8 bytes at a time on a 64-bit machine, moving whole *words* across the memory bus.

Consider a 4-byte `int` sitting at address `0x1006`, and a CPU that fetches 8-byte chunks at addresses `0x1000`, `0x1008`, `0x1010`…

```
             0x1000                     0x1008                    0x1010
   chunk A  ├────────────────────────┤  chunk B ├──────────────────────┤
                              ╔═══════════════╗
   the int                    ║ 1006 ... 1009 ║   ← straddles both chunks
                              ╚═══════════════╝
```

The integer spans **two chunks**. To read four bytes, the CPU must:

1. fetch chunk A,
2. fetch chunk B,
3. shift and merge the halves into a single register.

**Two memory transactions instead of one**, plus arithmetic to stitch them together. Had the `int` been at `0x1008`, it would have fit entirely inside chunk B: one fetch, no merging.

That is the whole of it. Alignment is not a formality; it is the difference between one memory transaction and two.

> [!NOTE]
> **The modern refinement.** The picture above is the classic one, and it is the right mental model — but on current x86-64 hardware it is not the whole truth, and the difference matters when you go to measure it.
>
> Since roughly 2008, Intel and AMD cores handle a misaligned access that stays **inside a single cache line** at essentially **no cost**. The penalty appears when the access **crosses a cache-line boundary** — and grows sharply when it crosses a **page** boundary.
>
> So the unit that actually matters is not the 8-byte word. It is the **cache line**, which is §3.2. Alignment matters because it keeps objects from straddling lines — not because the CPU cannot read an odd address.

### What actually happens when you get it wrong

This is where the architectures part company, and it matters more than most tutorials admit.

| | Ordinary load/store | Atomic / SIMD instructions |
|---|---|---|
| **x86-64** | Works. **Penalty**, not a crash. | **Crashes.** `MOVAPS` and friends *require* 16-byte alignment. |
| **ARM64** | Works for normal memory. | **Crashes.** Exclusive/atomic instructions (`LDXR`/`STXR`) fault on misaligned addresses. |

> [!CAUTION]
> The x86-64 column is a trap for the unwary. Misaligned ordinary access *works* on the machine you are developing on — slowly, silently. You will never notice. Then you use an aligned SIMD instruction, or you port to ARM, and the program dies.
>
> **"It runs on my machine" is not evidence of correct alignment.** It is evidence that x86-64 is forgiving.

---

## 3.2 Cache Lines, and Why Alignment Is Really About Locality

We have been speaking of 8-byte words. But there is a larger unit, and it dominates real performance.

RAM is slow — hundreds of cycles away. So the CPU keeps a small, fast copy nearby: the **cache**. And it does not fill that cache byte by byte. It fills it in blocks called **cache lines**, typically **64 bytes**.

**Touch one byte, and the CPU fetches all 64.**

This single fact drives everything.[^drepper]

### The consequence for your allocator

Ask `malloc` for a thousand small objects and it will scatter them across the heap, interleaved with headers and free-list bookkeeping. Iterate over them, and each one likely lives on a **different cache line**. A thousand objects, a thousand cache misses, the CPU stalled at every step.

Now allocate those same thousand objects **contiguously**, from one block. Each 64-byte cache line now holds *several* of them. Touch the first, and the next few arrive **for free** — already in cache. The CPU even notices the pattern and begins **prefetching** lines ahead of you.

> [!IMPORTANT]
> This is the real prize, and it is worth stating plainly.
>
> Your allocator will be faster than `malloc` at the moment of allocation — that is the easy win, and it is the one everyone measures. But the larger victory is that the memory it hands out is **contiguous**, and therefore the code that *uses* that memory runs faster too.
>
> **An allocator does not only allocate. It decides your data layout.** And data layout is what the cache rewards or punishes.

### Cache-line straddling and false sharing

Two more consequences, both direct products of alignment:

- **Straddling.** An object crossing a cache-line boundary occupies two lines. Reading it costs two potential cache misses instead of one. Align it, and it fits in one.
- **False sharing.** Two threads writing to two *different* variables that happen to share one cache line will fight over it, bouncing the line between their caches — even though neither touches the other's data. The fix is to force each onto its own line, with `alignas`.

> [!TIP]
> **Portability.** Cache lines are 64 bytes on x86-64 and most ARM64 — but **128 bytes on Apple Silicon**. Do not hard-code the number. C++17 provides `std::hardware_destructive_interference_size`.

---

## 3.3 The C++ View: `alignof`, `alignas`, and Struct Padding

C++ exposes alignment directly.

- **`alignof(T)`** yields the alignment `T` requires. On x86-64, `alignof(int)` is 4 and `alignof(double)` is 8. (These are ABI-defined, not universal: on 32-bit x86 Linux, `alignof(double)` is 4. Never hard-code them — that is what `alignof` is for.)
- **`alignas(N)`** forces a stricter alignment on a type or variable.

And now the property on which the entire rest of this chapter depends:

> [!IMPORTANT]
> **Every valid alignment is a power of two.** The standard guarantees it.
>
> Remember this. In §3.4 it stops being a curiosity and becomes the trick.

### Padding: the waste you are already paying

The compiler must ensure every member of a struct sits at a properly aligned offset. When they do not naturally fall into place, it inserts **padding** — dead bytes.

```cpp
struct Bad {
    char   a;   // 1 byte  ... then 7 bytes of padding, so that:
    double b;   // 8 bytes, lands on an 8-aligned offset
    char   c;   // 1 byte  ... then 7 more bytes of tail padding
};              // sizeof(Bad) == 24
```

Reorder the members — changing nothing else — and:

```cpp
struct Good {
    double b;   // 8 bytes
    char   a;   // 1 byte
    char   c;   // 1 byte  ... then 6 bytes of tail padding
};              // sizeof(Good) == 16
```

**Same data. Same fields. Eight bytes saved, purely by ordering.** Multiply that by a hundred thousand entities and the difference is measured in megabytes — and, because more objects now fit per cache line, in frame time.

> [!NOTE]
> Why the *tail* padding? Because `sizeof` must be a multiple of `alignof`. Otherwise, in an array, the second element would land on a misaligned address. The struct is padded so that `arr[1]` is as correctly aligned as `arr[0]`.

Sort your members from largest to smallest. It is the cheapest optimization in this book.

> [!WARNING]
> **But not blindly.** Member order is not always yours to choose. A struct that is uploaded to the GPU as a vertex or uniform buffer, written to a save file, or passed across a C ABI has a layout that **something else depends on**. Reorder it and you will not get a compiler error — you will get corrupted rendering, or a save file that no longer loads.
>
> Reorder freely for your own internal types. For anything that crosses a boundary, the layout is a contract.

Note also what this section is *not* about. Padding inside a struct is decided by the **compiler**, before your allocator ever runs. The padding of §3.4 is decided by **you**, between one allocation and the next. Both are internal fragmentation; only the second is yours to control.

---

## 3.4 The Arithmetic: Rounding an Address Upward

Here is the operation at the heart of every allocator ever written.

> Given a current address, and a required alignment, produce **the next address at or above it that satisfies that alignment.**

### The obvious way

```cpp
aligned = ((address + alignment - 1) / alignment) * alignment;
```

This is correct. It is also **slow**: integer division is among the most expensive operations the CPU offers — tens of cycles — and this runs on every single allocation.

> [!NOTE]
> **In fairness:** had `alignment` been a compile-time constant, the compiler would have recognized the power of two and emitted the bit trick for you. But in `Allocate(size, alignment)` the alignment arrives **at runtime**, as a parameter. The compiler cannot know it is a power of two, and so it must emit a real division. The optimization below is yours to make, not the compiler's.

### The way it is actually done

```cpp
aligned = (address + (alignment - 1)) & ~(alignment - 1);
```

No division. A subtraction, an addition, a bitwise NOT and an AND — a handful of cycles, all of them single-cycle instructions.

**Why it works** — and this is where "every alignment is a power of two" earns its keep.

Take `alignment = 8`, which is `0b1000`. Then:

```
  alignment       =  8  =  0b...0001000
  alignment - 1   =  7  =  0b...0000111     ← the mask: every low bit set
  ~(alignment - 1)      =  0b...1111000     ← inverted: every low bit CLEARED
```

An address is 8-aligned exactly when its **low three bits are zero**. So `& ~(alignment - 1)` **rounds any address *down*** to a multiple of 8, by erasing those bits.

We want to round *up*. So we first add `alignment - 1`, pushing the address to at-or-above the next multiple, and only then mask the low bits away.

Work it through:

```
  address       = 0x1003      →  ...0000 0011
  + 7           = 0x100A      →  ...0000 1010
  & ~7          = 0x1008      →  ...0000 1000     ✓ 8-aligned, and above 0x1003
```

And check the case that must not break — an address that is *already* aligned:

```
  address       = 0x1008
  + 7           = 0x100F
  & ~7          = 0x1008                            ✓ unchanged. No byte wasted.
```

The formula is **idempotent**: aligning an aligned address leaves it exactly where it was.

The gap you skipped — `aligned - address`, five bytes in the first example — is the **adjustment**, or padding. It is memory you can never use. It is internal fragmentation, and now you can see precisely where it comes from.

---

## 3.5 Doing It Legally in C++

You now have the formula. You cannot yet apply it, because **C++ will not let you do that arithmetic on a pointer.**

Three obstacles, and their three fixes.

### Obstacle 1 — `void*` has no arithmetic

```cpp
void* p = block;
p = p + 16;        // ✗ ill-formed. How large is a `void`?
```

The compiler cannot advance a pointer without knowing the size of what it points to, and `void` has none. (GCC permits it as an extension, treating `void` as one byte. It is not standard C++, and MSVC rejects it.)

**Fix:** cast to a **byte-sized type** first. In C++17, the honest choice is `std::byte`; `char*` and `unsigned char*` work equally well.

```cpp
std::byte* p = static_cast<std::byte*>(block);
p = p + 16;        // ✓ advances exactly 16 bytes
```

### Obstacle 2 — bitwise operators do not apply to pointers at all

```cpp
p = p & ~7;        // ✗ ill-formed. `&` and `~` are not defined for pointers.
```

There is no cast that fixes this. The operators simply do not exist for pointer types.

**Fix:** convert the pointer to an **integer**, do the arithmetic there, convert back.

### Obstacle 3 — which integer?

Not `int`. Not `long`. You need an integer type **wide enough to hold a pointer on every platform** — and the standard provides exactly that, in `<cstdint>`:

```cpp
#include <cstdint>

std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(ptr);
addr = (addr + (align - 1)) & ~(align - 1);          // the formula, at last
void* aligned = reinterpret_cast<void*>(addr);
```

`std::uintptr_t` is the unsigned integer type that survives the round trip — pointer → integer → pointer — unchanged. (It is technically an *optional* type in the standard, though no platform you will ever target omits it.)

> [!CAUTION]
> **Watch the type of the mask.** In the line above, `align` is a `std::size_t`, and so `~(align - 1)` is computed *as a `size_t`* before being applied to a `uintptr_t`.
>
> On every mainstream platform the two types are both 64 bits and nothing goes wrong. But the standard does not require them to be the same width — and if `size_t` were the narrower of the two, the mask would be **zero-extended** on its way up, filling the high bits with **zeros instead of ones**. Your `&` would then erase the top half of the address, and you would return a pointer into nowhere.
>
> Make the mask explicit, and the question never arises:
>
> ```cpp
> const auto mask = static_cast<std::uintptr_t>(alignment) - 1;
> addr = (addr + mask) & ~mask;
> ```
>
> This is a bug you will never hit, on hardware you will never see. Write it correctly anyway — it costs one cast, and the habit is the point.

**That is the whole mechanism.** Three lines. Everything else in an allocator is bookkeeping around them.

### Closing the loop: what does the caller pass?

§3.3 gave you `alignof`. §3.4 gave you the arithmetic. Here is where they meet — and it is the reason `Allocate` takes two parameters rather than one:

```cpp
void* mem = allocator.Allocate(sizeof(T), alignof(T));
```

The type itself declares how many bytes it needs and how it must be aligned. You never guess. Ask the language.

> [!TIP]
> This matters immediately in a game. Vector and matrix types are routinely declared `alignas(16)` so that SIMD instructions can operate on them — and, as §3.1 warned, a *misaligned* SIMD load does not run slowly. It **crashes**. An allocator that ignores `alignof(T)` will work perfectly on your `Enemy` struct and then die on your `Matrix4x4`.

<details>
<summary>The standard library already has this — <code>std::align</code></summary>

<br>

For honesty's sake: `<memory>` provides `std::align`, which performs exactly this computation and additionally checks that enough space remains.

```cpp
void* std::align(std::size_t alignment,
                 std::size_t size,
                 void*&      ptr,     // adjusted in place
                 std::size_t& space); // reduced by the adjustment
```

We do not use it, and the reason is not pride. An allocator you cannot explain is an allocator you cannot debug, tune, or defend in an interview. Write it once, understand it completely, and *then* reach for the standard version if you prefer.

</details>

---

## 3.6 Fragmentation, Formally

Chapter 1 promised a formal treatment. Here it is — three distinct ways an allocator wastes memory, which are routinely confused.

| | What it is | Where the bytes go | How you eliminate it |
|---|---|---|---|
| **Internal fragmentation** | Padding *inside* an allocated block | Empty, unusable | Know the alignment your objects actually need |
| **Allocator overhead** | The allocator's own bookkeeping | Occupied by headers, free-list links | Do not need a header — possible only when sizes are known |
| **External fragmentation** | Free holes *between* blocks, too small to reuse | Free, but unreachable | Do not free individual blocks at all |

Read the right-hand column again, because it describes the allocator you are about to build.

A **linear allocator** has **zero external fragmentation** — it never creates holes, because it never frees individual blocks. It has **zero overhead** — it stores no headers, because it never needs to look a block up again. Its *only* waste is the alignment padding of §3.4, which is a handful of bytes per allocation.

It achieves this by **giving something up**: the ability to free one object. That is the trade. It is not a trick, and it is not free — it is an engineering decision, and Chapter 4 is where you make it deliberately.

---

## 3.7 Build It

You have every piece. Here is the specification.

> [!NOTE]
> **The bodies are deliberately absent.** This is not an oversight, and the missing code is not available elsewhere in this repository. An allocator you have typed from a solution is an allocator you cannot defend under questioning. Write it.

```cpp
#include <cstddef>
#include <cstdint>

class LinearAllocator {
public:
    // Wraps a pre-allocated block of `size` bytes.
    // It does NOT own the block: it neither allocates nor frees it.
    // Whoever supplied the block is responsible for releasing it.
    LinearAllocator(void* block, std::size_t size);

    // Returns `size` bytes, aligned to `alignment`.
    // Returns nullptr if insufficient space remains.
    // Precondition: `alignment` is a power of two.
    void* Allocate(std::size_t size, std::size_t alignment);

    // Releases everything at once. O(1).
    void Reset();

    // For your benchmarks: how many bytes were lost to alignment padding?
    std::size_t WastedBytes() const;

private:
    std::byte*  m_start;    // beginning of the block
    std::size_t m_size;     // its total size
    std::size_t m_offset;   // bytes handed out so far
    std::size_t m_wasted;   // padding lost to alignment
};
```

**The invariants you must uphold**

1. Every pointer returned satisfies the requested alignment. No exceptions.
2. No allocation ever extends beyond `m_start + m_size`.
3. When space is insufficient, you return `nullptr`. You do not crash, and you do not return an address you do not own.
4. `Reset()` is O(1). It touches one integer. It does *not* loop.

**The traps, in the order you will fall into them**

- **Align the absolute address, not the offset.** The address you must round up is `m_start + m_offset` — not `m_offset` on its own. Align the bare offset and your code will appear to work, because `malloc` hands back blocks already aligned to 16, so the two happen to agree. Feed it a block that is *not* aligned — from a memory-mapped file, from a stack buffer, from a sub-allocation — and every pointer you return is silently wrong. **This bug hides for weeks.**

- **You must check for space *after* aligning, not before.** The padding consumes bytes too. Aligning first and checking second is the single most common bug in this code.
- **`alignment` must be a power of two**, or the mask trick silently produces garbage. Assert it. `(alignment & (alignment - 1)) == 0` is the check, and understanding *why* is a good five minutes.
- **Beware integer overflow in the bounds check.** The natural test is *"does `offset + size` still fit?"* — but if `size` is enormous (it arrives from a caller, and callers compute it), that addition can **wrap around** past zero. The check then passes, you hand back a pointer, and the caller writes far outside your block.
  Compare using **subtraction**, which cannot wrap: *"is `size` less than or equal to `capacity - offset`?"* This is not a theoretical concern. Integer overflow in an allocator's size check is one of the classic memory-corruption vulnerabilities.

- **`Reset()` does not call destructors.** It resets an integer. If you placed objects with non-trivial destructors in that memory, they are now leaked — silently. Chapter 4 confronts this directly.

**Prove it works before you believe it**

Build the allocator over a block whose address you know. `alignas(64) std::byte buffer[1024];` gives you a 64-aligned block on the stack — no `malloc` needed, and the arithmetic below is then predictable.

| Test | Expected |
|---|---|
| `Allocate(1, 1)`, then `Allocate(4, 8)` | Second pointer is 8-aligned; `WastedBytes() == 7` |
| `Allocate` more than the block holds | `nullptr`. No crash, no corruption, no state change. |
| `Allocate` exactly the remaining capacity | Succeeds. Off-by-one errors live here. |
| `Allocate`, `Reset()`, `Allocate` again | Second pointer equals the first |
| Any `Allocate(n, 1)` | Never wastes a byte |
| `Allocate(SIZE_MAX, 1)` | `nullptr` — not a wrapped-around success |

That last one is the overflow trap above. Most implementations fail it on the first attempt.

> [!TIP]
> Then repeat the whole suite over a **deliberately misaligned** block — `buffer + 1`. Every test must still pass. If they do not, you aligned the offset instead of the address.

When all six pass, on both blocks, you have written a working allocator — and Chapter 4 will make it fast, measure it, and connect it to real C++ objects.

---

## Exercises

1. **Prove the hardware cares — and learn to distrust a single measurement.** Allocate a large buffer. Time a loop reading 8-byte values from an aligned offset, then from `offset + 1`.

   On modern x86-64 you will likely measure **almost no difference**, and the naive conclusion is that alignment does not matter. It is the wrong conclusion. Now force every read to **straddle a cache-line boundary** — place the values at addresses of the form `64k - 4` — and measure again. *That* is where the cost lives.

   The lesson is §3.1's note, learned the hard way: **the unit that matters is the cache line, not the word.**

2. **Shrink a struct.** Take the `Bad` struct of §3.3. Print `sizeof` and, using `offsetof`, the offset of each member. Reorder the fields and repeat. Explain every padding byte.

3. **Write `AlignUp`.** Before the allocator, write the one function it stands on:
   ```cpp
   std::uintptr_t AlignUp(std::uintptr_t address, std::size_t alignment);
   ```
   Test it against alignments of 1, 4, 8, 16, 64 — with addresses that are already aligned, and addresses that are not.

4. **Then build `LinearAllocator`**, to the specification in §3.7. Make all six tests pass — on an aligned block and on a misaligned one.

---

## References

[^drepper]: Ulrich Drepper, *What Every Programmer Should Know About Memory* (2007). The definitive treatment of cache behaviour, and the source for §3.2.

**Primary sources for this chapter**

- Bryant & O'Hallaron, *Computer Systems: A Programmer's Perspective* — data representation and alignment; the memory hierarchy.
- Jason Gregory, *Game Engine Architecture* — Chapter 5, for the allocator's alignment adjustment as it is done in production engines.
- `cppreference.com` — `alignof`, `alignas`, `std::align`, `std::uintptr_t`, `std::byte`.

---

<div align="center">

**[← Chapter 2](./02-cpp-and-memory.md)** · **[Table of Contents](./README.md)** · *Chapter 4 — The Linear Allocator (in progress)*

</div>
