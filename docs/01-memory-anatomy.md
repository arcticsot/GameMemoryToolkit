# Chapter 1 — Anatomy of a Process's Memory

> **In one sentence:** before you can manage memory, you need to know where the operating system puts it.

**What you will learn**

- How a process's virtual address space is divided into segments, and what each one is for.
- Why globals split across three different segments, and how a huge zeroed array can cost nothing.
- Why the stack is fast, and why that speed is the benchmark we will be chasing.
- Why the heap is slow — and why `malloc` is not what most programmers think it is.

---

## 1.1 The Process Address Space

When you launch an executable, the operating system does not scatter your data randomly across RAM. It builds a **virtual address space** for the process: an ordered range of addresses, starting at `0x0`, which the program sees as private and entirely its own.

Two facts to fix now; the rest belongs to Chapter 8, where we meet the virtual memory system properly.

1. The addresses you print from a pointer (`0x5f8a...`) are **virtual**, not physical. Your program never touches RAM directly.
2. The space is divided into **segments** with distinct permissions (read, write, execute). Violating a permission means the kernel terminates the process — the familiar *segmentation fault*.

Here is the map, from **low** addresses at the bottom to **high** addresses at the top. Everything in this chapter is a commentary on this figure.

```
   HIGH addresses  0xFFFF...
  ┌────────────────────────────┐
  │           STACK            │   grows DOWNWARD
  │             │              │   automatic storage
  │             ▼              │
  ├────────────────────────────┤
  │                            │
  │        (free space)        │
  │                            │
  ├────────────────────────────┤
  │             ▲              │
  │             │              │
  │            HEAP            │   grows UPWARD
  │                            │   dynamic storage
  ├────────────────────────────┤
  │   .bss    (zero-init)      │   read / write
  ├────────────────────────────┤
  │   .data   (initialized)    │   read / write
  ├────────────────────────────┤
  │   .rodata (constants)      │   read-only
  ├────────────────────────────┤
  │   .text   (machine code)   │   read-only + execute
  └────────────────────────────┘
   LOW addresses   0x0000...
```

### How big is the gap in the middle?

The stack descends, the heap ascends. The obvious question: **do they collide?**

- On **32-bit** systems, the entire virtual space was **4 GB** (of which roughly 3 GB belonged to the process). Collision was a genuine risk.
- On **x86-64**, usable virtual addresses are **48 bits wide**, giving roughly **128 TB** of user address space. The gap in the middle is an abyss tens of terabytes across. Your program will exhaust physical RAM, or fail due to fragmentation, long before the stack and the heap come anywhere near one another.

> [!IMPORTANT]
> The practical consequence is not a footnote — it is a design opportunity. On 64-bit systems, **virtual addresses are essentially free**. You can *reserve* absurd quantities of address space without consuming a single byte of physical RAM. Modern engine allocators are built on precisely this trick, and we will use it in Chapter 8.

> [!NOTE]
> **On ASLR.** The map above describes the *relative order* of the segments, not fixed addresses. Modern systems employ **Address Space Layout Randomization**: the base positions of the stack, the heap, and shared libraries are randomized on every run, as a defence against exploits. This is why printing the same pointer across two runs yields two different addresses.

---

## 1.2 The Text Segment

The text segment holds the **compiled machine code**: your game logic, translated into binary instructions.

- **Permissions: read-only and executable.** It is deliberately not writable. If a stray pointer attempts to write here, the OS terminates the process immediately. This is a defence: it prevents a program — or an attacker — from rewriting its own instructions while running.
- **Fixed size**, known at compile time. It never grows at runtime.
- **Shared.** Launch the same executable twice, and the text segment occupies physical RAM *once*, mapped into both processes. This is possible precisely because it is read-only.

---

## 1.3 Static Storage: `.data`, `.rodata`, `.bss`

Globals and `static` variables live neither on the stack nor on the heap. They occupy fixed locations decided at compile time. But they split across **three** segments according to *how* they are initialized — and the distinction is not pedantry. It is an engineering trick to avoid wasting disk space.

### `.data` — initialized to a non-zero value

```cpp
static int max_enemies = 128;   // lands in .data
```

The value must be stored somewhere in order to be loaded, so `.data` **occupies space in the executable file** on disk. Read-write.

### `.rodata` — read-only constants

Frequently omitted from introductory treatments, but it exists and it matters: string literals and constant globals.

```cpp
const char* name = "Player1";   // the characters "Player1" live in .rodata
                                // (the pointer variable `name` itself lives in .data)
```

Read-**only**. This is precisely why modifying a string literal is undefined behaviour: you are writing into `.rodata`, and the OS stops you.

### `.bss` — zero-initialized, or uninitialized

```cpp
static int player_score;         // lands in .bss
static int scoreboard[10000];    // 40 KB, also in .bss
```

**Why separate it from `.data`?** To avoid wasting disk space. Ten thousand zeroed integers would occupy 40 KB of useless zeros inside the file. Instead, the executable does not store them at all: the file header simply instructs the OS, *"reserve 40 KB for me and fill it with zeros when you load me."* The `.bss` segment therefore has **zero size on disk**.

The name is historical: *Block Started by Symbol*.

| Segment | Holds | Occupies file space? | Permissions |
|---|---|---|---|
| `.data` | globals initialized to non-zero | **Yes** | read / write |
| `.rodata` | constants, string literals | Yes | read-only |
| `.bss` | globals initialized to zero, or uninitialized | **No** (size only) | read / write |

### And in RAM? It costs nothing either — at first

The saving is not confined to disk. Nobody runs a 40 KB `memset` at startup. The mechanism is called **demand-zero paging**:

1. The OS maps `.bss` as **anonymous memory**, promising it will read as zero — while allocating **no physical RAM**.
2. As long as the program only **reads**, the OS may point every one of those virtual pages at a **single shared physical page of zeros** (the *zero page*). A thousand virtual pages, one physical frame.
3. Only when the program **writes** does the CPU raise a page fault. The kernel then allocates a real physical page, zeroes it, maps it in, and **restarts the faulting instruction**, which now proceeds.

The result: declaring an enormous array in `.bss` is **instantaneous** and costs **zero bytes of physical RAM** until you actually touch it. You pay only for the pages you use.

> [!WARNING]
> **This is not Copy-on-Write** — a common and costly confusion. Copy-on-Write duplicates *pre-existing content* on first write; it is the mechanism behind `fork()`. Here there is nothing to copy: the content is zero by definition. The two are cousins, not synonyms.

---

## 1.4 The Stack (Automatic Storage)

The stack is the fast working memory of execution. It is strictly **LIFO**, and on x86-64 it **grows toward lower addresses**.

> [!NOTE]
> *Automatic storage* and *dynamic storage* are the terms the C++ standard itself uses — it never says "stack" or "heap", which are properties of the machine, not of the language. Chapter 2 makes the distinction precise. For now, treat them as labels.


### The stack frame

Each function call pushes a block called a **stack frame**, containing:

1. the arguments passed (those that do not fit in registers),
2. the **return address** — where to resume once the function completes,
3. the function's **local variables**.

> [!WARNING]
> **Who actually pushes the frame?** Not the operating system — this is a widespread misconception. It is **machine code emitted by the compiler**. Every function carries a *prologue* and an *epilogue*: a handful of instructions, inserted by the compiler, that move the stack pointer. There is no runtime supervisor watching over the stack. The code is inline, inside your own function. (Chapter 2 examines who does what.)

When the function returns, the epilogue restores the stack pointer and the frame simply ceases to exist. This is why you never call `free` on a local variable.

> [!NOTE]
> **A point of terminology worth getting right.** The ordinary return of a function is *not* called **stack unwinding**. Stack unwinding is the mechanism by which, when an **exception** is thrown, frames are dismantled one by one up the call chain, invoking destructors as they go. The two are distinct processes.

### Why it is fast

Allocating on the stack is **not a search**. It is a single subtraction applied to a CPU register — the **stack pointer** (`RSP` on x86-64). Move `RSP` by N bytes, and you have "allocated" N bytes. Cost: one instruction.

Remember this figure. **It is the speed our own allocators will be measured against.**

### Its limits — and why the heap must exist

- **Small and fixed.** Roughly **8 MB on Linux**, **1 MB on Windows**, by default. It is not built for bulk data.
- **Overflow is fatal.** A large local array, or unbounded recursion, exhausts the space and produces a **stack overflow**, killing the process. And note *how* it fails: there is no error code to check, no exception to catch. The process simply dies.
- **Sizes are fixed at compile time.** In standard C++, the size of a stack frame is decided when the function is compiled. You cannot decide at runtime that you want N objects and place them there.

<details>
<summary>A caveat on that last point — <code>alloca</code> and variable-length arrays</summary>

<br>

Strictly speaking, runtime-sized stack allocation *is* possible. C99 introduced **variable-length arrays**, and the non-standard `alloca()` has existed for decades; both move the stack pointer by an amount computed at runtime.

Neither is a way out, and neither belongs in an engine:

- **VLAs are not part of standard C++.** GCC and Clang accept them as an extension; MSVC does not.
- **`alloca()` cannot fail gracefully.** If the requested size exceeds the remaining stack, there is no `nullptr` to check — the process simply dies. A size derived from user input, a network packet, or a level file therefore becomes a crash, or worse, an exploitable one.

The statement above holds where it matters: **for portable, safe C++, stack sizes are fixed at compile time.**

</details>

These three limits are exactly why the heap exists — and, as you will see, why custom allocators exist.

---

## 1.5 The Heap (Dynamic Storage)

The heap is the large expanse of memory used for **runtime** allocations whose size is not known at compile time. In C++ you reach it through `new`/`delete` or `malloc`/`free`. Unlike the stack, **management is manual**: what you allocate, you must release.

### The truth about `malloc`: it is not an OS function

Fix this now, because it is the thesis of the entire project.

> [!IMPORTANT]
> **`malloc` is not a kernel system call.** It is a **general-purpose memory allocator**, written by the authors of the C standard library (**libc** — typically `glibc` on Linux), and it runs **entirely in user space**, inside your process.
>
> It is, in other words, the same kind of machine you are about to build. Only general, where yours will be specialized.

When you call `malloc`, libc **searches** memory it has already obtained. Only when it runs dry does it issue a **system call** (`brk` or `mmap` on Linux) to request another large block from the kernel. Chapter 2 covers the details.

Why this reframes everything: what you are about to build — request one large block, then manage it yourself — **is not heresy. It is exactly what libc already does.** The difference is that `malloc` must serve *everyone*: a browser, a database, a compiler. Being general-purpose, it can assume nothing.

You, by contrast, know things. You know how many enemies exist, how long a frame lasts, how large a projectile is. **That surplus knowledge is what will let you outperform `malloc` by orders of magnitude.** You are not reinventing the wheel; you are specializing it.

With that established, here are the two problems inherent to any general-purpose allocator — the problems our allocators will eliminate. Both descend from a single word: **search**.

### Problem 1 — Slow and unpredictable

`malloc` is not a single addition. It must traverse data structures, occasionally descend into the kernel for more pages, and — in a multi-threaded program such as a game, with audio, rendering, and physics running concurrently — **acquire a lock** so that two threads do not corrupt the allocator simultaneously.

The cost is **not constant**. Sometimes it returns instantly; sometimes it does not. At 60 frames per second you have **16.6 ms per frame**. A `malloc` that occasionally takes longer produces a visible **stutter**.

For a game, unpredictability is worse than slowness. A consistently slow operation can be budgeted for. An occasionally slow one cannot.

### Problem 2 — Fragmentation

Continually allocating and freeing objects of varying sizes — explosion particles, projectiles, temporary buffers — leaves the heap resembling Swiss cheese: full of free holes that are **not contiguous**. You may have 50 MB free in total and still **fail** to allocate a 10 MB asset, because no single contiguous hole is large enough.

This is *external fragmentation*; Chapter 3 treats it formally.

### The hidden costs: metadata and alignment

Two further costs, both of which are arguments in favour of writing your own allocators.

1. **Metadata overhead.** libc must remember how large each block is — otherwise `free(p)`, which receives only a pointer, could not know how many bytes to release. It therefore writes a **header** immediately before the bytes it hands you. Ask for 8 bytes and you consume considerably more. Across thousands of small objects, the waste compounds.
2. **Fixed, generous alignment.** `malloc` cannot know what you intend to store, so it must guarantee an alignment suitable for **any** type — 16 bytes on x86-64. If you needed 4 bytes aligned to 4, you paid for the rounding anyway.

Both waste memory, but not in the same way — and the distinction matters, because each is eliminated by a different means.

- **Alignment** produces **internal fragmentation**: padding bytes left *empty* inside your block, so that the next one begins on a valid boundary. You eliminate it by knowing the alignment your objects actually require.
- **Metadata** is **allocator overhead**: bytes that are not empty at all, but occupied by the allocator's own bookkeeping. You eliminate it by not needing a header — which is possible only when the size is known in advance.

Both are *internal* in the sense that the waste lies within the allocated block, as opposed to the Swiss cheese above, which is *external* fragmentation between blocks. Chapter 3 treats all three formally.

> [!NOTE]
> Conventions differ across the literature. CS:APP defines internal fragmentation as the gap between the size of the allocated *block* and the size of the *payload* — a definition under which the header counts as internal fragmentation. Operating-systems texts more often reserve the term for padding alone and classify metadata separately as overhead. The distinction drawn above is the one that is useful to us, because it maps onto two different fixes; do not be surprised to meet the other.

Your allocators, by contrast, **know**. A pool allocator for projectiles knows the exact size: **no header** (the size is implicit) and **exact alignment**.

### And beneath it all: cache misses

There is a third effect, silent and lethal. `malloc` returns blocks **scattered** across RAM. The CPU does not read memory byte by byte; it reads in blocks called **cache lines**. Scattered data means continuous **cache misses** — the CPU stalling, waiting for RAM.

This, beneath everything else, is the real reason AAA studios forbid `new` inside the game loop. Chapter 3 treats cache locality properly.[^drepper]

> [!TIP]
> **Portability (x86-64 / ARM64).** A cache line is **64 bytes** on x86-64 and on most ARM64 chips — but **128 bytes on Apple Silicon**. Do not hard-code `64`. C++17 provides `std::hardware_destructive_interference_size`. A full table of per-platform constants appears in the Appendix.

---

## 1.6 Why This Matters: The Golden Rule

You now hold every piece required to understand the rule that governs this entire book.

> **During the game loop — the frames rendered sixty times a second — you do not ask the operating system for memory.** The cost of `malloc` and `new` destroys frame-time consistency.

And therefore the strategy, which the remaining chapters implement:

1. **At startup**, or during a loading screen, ask the heap **once** for one enormous block — say, 1 GB.
2. From that point on, **cut ties with the OS.** You manage that gigabyte yourself, with your own allocators, handing memory to game entities at speeds comparable to the stack (§1.4), free of fragmentation, and with data laid out **contiguously** in RAM.

The stack showed us **how fast allocation can be**: one subtraction on a register. The heap showed us **what to avoid**: searching, locking, fragmenting, scattering.

The work ahead is to build structures with **the speed of the stack** and **the freedom of the heap**.

---

## Exercises

1. **Redraw the map.** Reproduce the segment diagram of §1.1 from memory, in your own words.

2. **See the segments.** Compile a small program and inspect it with `size ./program`, which reports the sizes of `text`, `data`, and `bss`. Now add an uninitialized `static int arr[100000];`, recompile, and inspect again.

   **Observe:** `bss` grows by 400 KB, while the size of the file on disk barely changes. That is §1.3, in front of you.

3. **Watch the zero page work.** Extend the previous program to print its own **resident set size** — the physical RAM it actually occupies — before and after writing to `arr`:

   ```bash
   grep VmRSS /proc/self/status
   ```

   Declaring a 400 KB array should barely move `VmRSS`. Writing across it should raise it by roughly 400 KB. **That gap is demand-zero paging**, measured.

4. **See the real map.** On Linux, have your program print its own memory map — no second terminal, no process ID required:

   ```cpp
   #include <fstream>
   #include <iostream>

   int main() {
       std::ifstream maps("/proc/self/maps");
       std::cout << maps.rdbuf();
   }
   ```

   The kernel exposes every mapping of the running process, together with its permissions. Your executable will appear as **several separate mappings — one per permission set**: an `r-xp` region (that is `.text`), an `r--p` region (`.rodata`), and an `rw-p` region (`.data` and `.bss`). Alongside them you will find `[heap]`, `[stack]`, and the shared libraries.

   Do not expect an exact count of lines: modern toolchains split the segments more finely than the textbook diagram suggests. **Read the permissions, not the line numbers** — they are what identify each region.

   **This is the diagram of §1.1, printed by the kernel.** Compare them side by side.

---

## References

[^drepper]: Ulrich Drepper, *What Every Programmer Should Know About Memory* (2007). Freely available; the definitive treatment of cache behaviour.

**Primary sources for this chapter**

- Randal E. Bryant & David R. O'Hallaron, *Computer Systems: A Programmer's Perspective* — the chapter on Linking, for segment structure; the chapter on Virtual Memory, for the address-space model.
- Jason Gregory, *Game Engine Architecture* — Chapter 5, Memory Management, for the engine perspective.

---

<div align="center">

**[← Table of Contents](./README.md)** · **[Chapter 2 → How C++ Reaches Memory](./02-cpp-and-memory.md)**

</div>