<div align="center">
  
# Memory Management for Game Engines
A ground-up study of how memory works, and how to take control of it in C++  
*Companion notes to the GameMemoryToolkit library*

</div>

> **🚧 Work In Progress:** This repository is in active development. The foundational theory is currently being documented, and the C++ implementation will follow.

## About This Manual
Modern game engines do not use `new` and `delete` in the game loop. They allocate memory in enormous blocks at startup and then manage every byte themselves, with hand-written allocators tuned to the shape of the problem.

These notes are the study behind that practice — written while building **GameMemoryToolkit**, a small library of production-style allocators.

The aim is not encyclopaedic coverage. It is a direct line from how memory actually works to working, measurable code. Every chapter earns its place by moving that line forward.

[!NOTE]
> **How to read this.** Chapters 1–4 are foundations: read them in order, since each depends on the last. Chapters 5–8 implement the allocators and can be read selectively. Chapters 9–10 are what turn the code into an engineering artefact — measurement and portability.

## Table of Contents

### Part I — Foundations
| # | Chapter | What it answers | Status |
|---|---------|-----------------|--------|
| 1 | [Anatomy of a Process's Memory](docs/01-memory-anatomy.md) | Where does my data actually live? | ✅ Ready |
| 2 | [How C++ Reaches Memory](docs/02-cpp-and-memory.md) | Who puts it there — and what are malloc and new, really? | ✅ Ready |
| 3 | *Virtual Memory and the OS* | Where does the big block come from? mmap, VirtualAlloc, paging. | *Drafting* |
| 4 | *Alignment and Pointer Arithmetic* | The arithmetic every allocator is built on. | *Upcoming* |

### Part II — The Allocators (Code implementation)
*The `src/` directory and `CMake` build system will be published alongside Chapter 5.*

| # | Chapter | What you build | Status |
|---|---------|----------------|--------|
| 5 | *The Linear Allocator* | The simplest and fastest. Allocation in a single addition. | *Upcoming* |
| 6 | *The Stack Allocator* | Adds LIFO release: markers and rollback. | *Upcoming* |
| 7 | *The Pool Allocator* | Fixed-size slots and free lists. Bullets, particles, entities. | *Upcoming* |
| 8 | *The Frame Allocator* | Per-frame scratch memory, reclaimed for free. | *Upcoming* |

### Part III — Engineering
| # | Chapter | Why it matters | Status |
|---|---------|----------------|--------|
| 9 | *Benchmarking* | A performance claim without a measurement is an opinion. | *Upcoming* |
| 10| *Portability* | x86-64 and ARM64: what actually differs, and what does not. | *Upcoming* |
| - | *Appendix — Platform Constants* | Page sizes, cache lines, alignment, per platform. | *Upcoming* |

## Primary Sources
This study draws on a small number of texts, each selected for what the others do not cover.

| Source | Used for |
|--------|----------|
| **Bryant & O'Hallaron**, *Computer Systems: A Programmer's Perspective* | Segments, alignment, virtual memory, allocator internals |
| **Jason Gregory**, *Game Engine Architecture* | The engine perspective; allocator taxonomy |
| **Ulrich Drepper**, *What Every Programmer Should Know About Memory* | Cache behaviour and its cost |
| **Robert Nystrom**, *Game Programming Patterns* | Object pooling; data locality |
| **cppreference.com**, `malloc(3)`, `mmap(2)` | Exact semantics, when writing code |

<div align="center">
  
[Begin with Chapter 1 →](docs/01-memory-anatomy.md)

</div>
