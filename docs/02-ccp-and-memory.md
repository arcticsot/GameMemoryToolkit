# Chapter 2 — How C++ Reaches Memory

> **In one sentence:** Chapter 1 asked *where* memory lives; this chapter asks *who puts it there* — and what `malloc` and `new` really are.

**What you will learn**

- The vocabulary the C++ standard actually uses — and why it never says "stack" or "heap".
- Which of four distinct actors populates each segment, and when.
- Why the stack "manages itself" — and who wrote the code that manages it.
- What separates `malloc` from `new`, and why that separation is the hook every custom allocator hangs on.

---

## 2.1 The Right Vocabulary: Storage Duration

Chapter 1 spoke of "the stack" and "the heap". Everyone does. But **the C++ standard never names them.**

The standard deliberately says nothing about *where* memory lives — that is an implementation detail, varying by operating system and architecture. It speaks instead about **how long an object lives**: its **storage duration**. There are four.

| Storage duration | Lives... | Typically found... |
|---|---|---|
| **automatic** | from entry to exit of a block | on the **stack** |
| **static** | for the entire run of the program | in **`.data` / `.bss`** |
| **thread** (`thread_local`) | for the lifetime of a thread | in **thread-local storage**, one copy per thread |
| **dynamic** | from `new` until `delete` — **you decide** | on the **heap** |

Two consequences worth internalizing.

**"Typically" is not "always."** A local variable — automatic storage — may never touch the stack at all. If the compiler sees that it fits, it will keep the variable **in a CPU register**. The standard permits this precisely because it never promised a stack in the first place.

**Dynamic storage is the only one whose lifetime you control.** It is also the only one that is expensive. And that gap is exactly what this book exists to close: we want the **freedom** of dynamic storage duration at the **cost** of automatic storage duration (§1.4). Every allocator in Part II is an attempt at that trade.

From here on this book will say "stack" and "heap", because that is the working vocabulary of the field. But you now know they are the *usual locations*, not the rule of the language.

---

## 2.2 Four Actors, Not One

The question that opened this chapter — *"the segments get populated automatically, but automatically by whom?"* — has a more interesting answer than most people expect. **Four different actors** are involved, at four different moments.

| Actor | Responsibility | When |
|---|---|---|
| **Kernel loader** | Maps `.text`, `.data`, `.bss`; sets permissions | at launch (`execve`) |
| **C++ runtime (CRT)** | Runs constructors of globals, calls `main`, runs destructors | before and after `main` |
| **Compiler** | Emits the prologue/epilogue code that drives the stack | at compile time, executed at runtime |
| **libc + kernel** | Serve the dynamic heap on demand | at runtime, per request |

The remaining sections take them in order.

---

## 2.3 The Kernel Loader: Populating the Segments

When you run `./game`, the shell issues the **`execve`** system call. From that moment, the **kernel loader** takes charge. It:

1. **Reads the executable header** — ELF on Linux, PE on Windows — which contains the map of segments.
2. **Maps** `.text` and `.data` *directly from the file on disk* into the address space. It does not eagerly copy everything into RAM; pages arrive on demand. (Chapter 3.)
3. **Sets permissions** on each region: `.text` becomes read-execute, `.rodata` read-only, `.data` and `.bss` read-write.

This is the mechanism behind every guarantee in Chapter 1. The text segment is unwritable not by convention but because the loader marked those pages read-only, and the MMU enforces it in hardware.

---

## 2.4 The C++ Runtime: What Happens Before `main`

Here is the actor that programmers forget entirely.

**The real entry point of your program is not `main`.** It is a function in the C runtime, conventionally called `_start`. It performs the setup work, calls `main`, and cleans up afterwards.

```cpp
struct Config {
    Config() { /* this code runs BEFORE main() */ }
};

static Config g_config;   // constructed by the runtime, not by main

int main() { /* ... */ }
```

The distinction that matters:

- **Constant-initialized globals** (`static int x = 128;`) require no code at all. The value is written into `.data` by the compiler, and the loader maps it in. Free.
- **Globals with non-trivial constructors** — like `g_config` above — require code to *run*. The compiler gathers pointers to these constructors into a dedicated list (`.init_array` on ELF systems), and the runtime walks that list, calling each one **before `main` begins**. Destructors run symmetrically, **after `main` returns**.

**Why this matters for your allocator.** Your allocator will very likely be a global, or at least long-lived. Knowing *when* its constructor runs — and in what order relative to other globals — is the difference between a library that works and one that crashes before `main` on some machines and not others.

<details>
<summary><b>Deep dive — The static initialization order fiasco</b> (optional, but it will bite you)</summary>

<br>

Within a single translation unit, globals are constructed **in order of declaration**. Across *different* translation units, the order is **unspecified** — the standard makes no promise whatsoever, and it can vary with link order, compiler, or platform.

So this is a genuine bug, and a subtle one:

```cpp
// allocator.cpp
GlobalAllocator g_allocator;      // must exist first

// world.cpp
World g_world(g_allocator);       // uses g_allocator in its constructor
                                  // — but is g_allocator constructed yet? Unknown.
```

It may work on your machine and crash on the build server. The link order changed; that is all it takes.

**The idiomatic fix: the function-local static.**

```cpp
GlobalAllocator& GetAllocator() {
    static GlobalAllocator instance;   // constructed on first call, never earlier
    return instance;
}
```

A `static` declared inside a function is initialized **the first time control passes through its declaration** — not before. The ordering problem dissolves: the object cannot be used before it is constructed, because *using* it is what constructs it.

Since C++11 this initialization is also **guaranteed thread-safe**: if two threads call `GetAllocator()` simultaneously, exactly one performs the construction and the other blocks until it completes.

</details>

---

## 2.5 The Stack: Who Actually Manages It

The claim that "stack memory manages itself" is true, but it conceals the interesting part.

> [!WARNING]
> **The operating system does not push stack frames at runtime.** This is a persistent misconception. Frames are pushed and popped by **machine code emitted by the compiler**.

Precisely:

- The kernel, at launch, prepares only the **initial page** of the stack — placing `argc`, `argv`, and the environment there — and records the growth limit.
- From then on, every frame is compiler-generated code. Each function carries a **prologue** that reserves space by moving the stack pointer, and an **epilogue** that restores it on exit.

So "the stack manages itself" means, stated precisely: **the compiler has written the instructions that move the stack pointer, and inlined them into your function.** There is no supervisor. There is no bookkeeping. There is a register and some arithmetic. That is the entire mechanism — and it is why stack allocation costs one instruction.

**How the stack grows.** The stack is not pre-allocated in full. When a deep call chain writes below the currently mapped pages, the CPU raises a fault; the kernel recognizes an address just beneath the stack region and **extends the mapping automatically**, up to the configured limit (8 MB by default on Linux). Beyond that limit, the fault is not resolved, and the process dies with a **stack overflow**.

---

## 2.6 The Heap, from C++: `malloc` and `new` Are Not the Same Thing

This is the most common confusion in the language. There are two distinct layers.

### The C layer — `malloc` / `free` / `realloc`

Library functions operating on **raw bytes**. They know nothing of types, constructors, or objects.

- **`malloc(n)`** returns a `void*` to `n` **uninitialized** bytes — they contain garbage — or `nullptr` on failure.
- **`free(p)`** returns the block beginning at `p` to libc. Passing a pointer not obtained from `malloc`, or freeing twice, is undefined behaviour.
- **`realloc(p, n)`** resizes the block. It attempts to extend in place; if it cannot, it **allocates a new block, copies the bytes, frees the old one, and returns a different address**.
- **`calloc(count, size)`** allocates *and* zeroes. It is not merely `malloc` followed by `memset`, and the difference is one you have already met: for a large request, libc obtains fresh pages from the kernel — and those pages are **already zero** (§1.3, demand-zero paging). `calloc` can therefore **skip the `memset` entirely**. Zeroing a large buffer with `calloc` is free; zeroing it by hand is not.

> [!CAUTION]
> **Never call `realloc` on C++ objects.** `realloc` relocates bytes with a raw `memcpy`. It does not call move constructors, copy constructors, or destructors — it does not know they exist.
>
> The criterion is precise: an object may be relocated byte-wise only if it is **trivially copyable** (`std::is_trivially_copyable`). A custom destructor alone is enough to disqualify a type.
>
> `std::string` is the cautionary example, and it is not academic: most implementations use the **Small String Optimization**, storing short strings in a buffer *inside the object itself*, pointed to by an internal pointer. The object is **self-referential**. `realloc` it, and that pointer still addresses the old location. The string is silently corrupt.

**The consequence you should carry forward:** this is exactly why **`std::vector` cannot use `realloc`** — not as an optimization it declined, but as a thing it is forbidden to do. When a vector grows, it must allocate a new block, **move-construct each element individually**, destroy the originals, and free the old block.

That is why vector growth is expensive, why engines call `reserve()` in advance, and why fixed-size block allocators — the subject of Part II — are attractive in the first place.

### The C++ layer — `new` / `delete`

Operators of the language. They do **two** things, and this is the whole difference:

```
new T(args)   ≡   1. allocate sizeof(T) bytes   +   2. run T's CONSTRUCTOR
delete p      ≡   1. run the DESTRUCTOR          +   2. release the bytes
```

- `malloc` hands you mute bytes. `new` hands you a **live, fully constructed object**.
- They never mix. Memory from `new` is released with `delete`; memory from `malloc` with `free`. Crossing them is undefined behaviour.
- On failure they behave **oppositely**: `malloc` returns `nullptr`, and you must check it. `new` **throws `std::bad_alloc`** and never returns null. (`new (std::nothrow)` opts into the `malloc`-style behaviour.)

### The detail that makes this entire project possible

Inside `new` there are, in fact, two separable things — and C++ lets you touch each one independently.

- The **new-expression** (`new T(args)`) — the language syntax, which orchestrates *allocate, then construct*.
- The **allocation function**, `operator new(size_t)` — which performs *only* the first half, and which in the standard library typically calls `malloc`.

> [!IMPORTANT]
> **`operator new` is replaceable.** You may substitute your own — globally, or per class. It is the language's **official hook** for making `new Projectile()` draw its bytes from *your* allocator instead of `malloc`, **without changing a single line of game code**.
>
> Its counterpart is **placement new** — `new (address) T(args)` — which allocates nothing at all and simply constructs an object at an address you supply.
>
> Together, these two are the bridge between raw bytes and live C++ objects. Every allocator in Part II is built on them.

---

## 2.7 What Is Under `malloc`?

We can now answer the question that Chapter 1 raised and deferred: *the heap is managed automatically — but by whom?*

> [!IMPORTANT]
> **`malloc` is not a system call.** It is **libc** code — `glibc` on Linux, and its `ptmalloc` allocator — running in **user space**, inside your process.

libc maintains its own bookkeeping: free lists, size classes, arenas. It serves most requests **without ever entering the kernel**. It descends into the kernel only when it needs more raw memory, and it does so in one of two ways:

- **Small requests** extend the *program break* — the upper boundary of the heap from §1.1 — via the **`brk`/`sbrk`** system call. The heap grows contiguously upward.
- **Large requests**, above a threshold (128 KB by default on glibc), bypass the heap entirely and call **`mmap`**, which maps a separate anonymous region. When freed, these large blocks are genuinely returned to the kernel via `munmap`.

> [!NOTE]
> That threshold is not a constant. glibc **adapts it at runtime**: if your program repeatedly allocates and frees blocks just above the limit, glibc raises the threshold — up to 32 MB — so that those blocks are served from the heap and recycled, rather than being handed back to the kernel and requested again. Do not treat 128 KB as a fixed law; treat it as a starting point that the allocator will move under you.

### The surprise: `free` usually does *not* return memory to the OS

Call `free` on a small block, and libc **does not hand it back to the kernel**. It keeps it in a free list, ready for the next `malloc`. Entering the kernel every time would be slow; recycling in user space is not.

The practical consequence trips up everyone at least once: **your process's memory footprint, as seen from `top` or Task Manager, may not fall** even after you have released everything. libc is holding the memory in reserve. This is not a leak — but it looks exactly like one, and knowing the difference will save you an afternoon.

### The hidden costs: metadata and alignment

Two further costs, both of which are arguments for writing your own allocator.

1. **Metadata overhead.** libc must record the size of every block — otherwise `free(p)`, which receives only a pointer, could not know how many bytes to release. It therefore writes a **header** immediately before the bytes it returns. Request 8 bytes and you consume noticeably more. Across thousands of small objects, that waste compounds.

2. **Fixed, generous alignment.** `malloc` cannot know what you intend to store, so it must satisfy **any** type: 16 bytes on x86-64. If you needed four bytes aligned to four, you paid for the rounding regardless.

Your allocators, by contrast, **know**. A pool allocator for projectiles knows the exact size and alignment of a projectile: **no header** — the size is implicit — and **exact alignment**, no rounding. Two more pieces of the advantage you will measure in Chapter 9.

---

## 2.8 Why This Matters

Read §2.7 once more, because it reframes the entire project.

libc **already does what we are about to do.** It requests a few large blocks from the kernel and manages them internally, recycling memory through structures of its own design.

Our allocators are the same idea, **specialized**. `malloc` must serve a browser, a database, a compiler — it can assume nothing. We can assume a great deal: how many enemies exist, how long a frame lasts, how large a projectile is, when everything can be discarded at once.

**That surplus knowledge is the entire performance advantage.** Not cleverness — knowledge.

What remains is to learn how libc obtains raw memory from the kernel in the first place, since we will need to do the same. That is `mmap`, `VirtualAlloc`, and the virtual memory system beneath them: **Chapter 3**.

---

## Exercises

1. **Prove that code runs before `main`.** Write a global object whose constructor prints a line. Verify that it appears *before* anything printed inside `main`. That is §2.4, observable.

2. **Watch `realloc` move your data.** Allocate a block with `malloc`, record the address, then `realloc` it to a much larger size and print the new address. Note whether it changed. Now consider what would have happened had the block contained `std::string` objects.

3. **Watch libc talk to the kernel.** On Linux, run your program under `strace -e trace=brk,mmap ./program`. Allocate something small in a loop, then something very large — 1 MB, say.

   You will see a burst of `brk` and `mmap` calls at startup, before `main` even runs: that is libc and the dynamic linker setting themselves up. Ignore it. The interesting part is what your *own* allocations do. The small ones will mostly produce **nothing at all** — libc already holds that memory and serves them in user space. The large one produces a distinct **`mmap`**.

   That contrast is §2.7, observable.

4. **See the heap grow.** Have your program print `/proc/self/maps` — reading its own memory map requires no second terminal and no PID — then allocate 100 MB, then print it again. Compare the two. Either the `[heap]` region has extended, or a new anonymous mapping has appeared. Which one, and why?

---

## References

**Primary sources for this chapter**

- Bryant & O'Hallaron, *Computer Systems: A Programmer's Perspective* — the chapters on Linking and on Virtual Memory (which includes a full `malloc` implementation).
- `malloc(3)`, `brk(2)`, `mmap(2)` — Linux manual pages, for exact semantics.
- `cppreference.com` — `operator new`, placement new, storage duration.
- Scott Meyers, *Effective Modern C++* — for the object-lifetime consequences of raw memory operations.

---

<div align="center">

**[← Chapter 1](./01-memory-anatomy.md)** · **[Table of Contents](./README.md)** · **[Chapter 3 → Virtual Memory and the OS](./03-virtual-memory.md)**

</div>
