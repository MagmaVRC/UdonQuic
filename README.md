# UdonQuic

An optimizer for VRChat Udon bytecode, written in C++20.

UdonQuic works on finished Udon programs, not on source. It runs after the compiler, whether that is UdonSharp, UdonLuau or the graph, and rewrites the program into one that does the same thing with fewer instructions and fewer externs. On the stock VM every instruction and every extern costs a dispatch, so compilers never need to optimize: UdonQuic does it once, for all of them.

## Installing in Unity

Requirements: Windows, Unity 2022.3 or Unity 6, and the VRChat Worlds SDK.

**VRChat Creator Companion (recommended):** [add the repository to VCC](vcc://vpm/addRepo?url=https://vpm.magmamc.dev/index.json) (or Settings > Packages > Add Repository with `https://vpm.magmamc.dev/index.json`), then add UdonQuic to your world project from Manage Project. VCC handles updates.

**Manual:** download the latest release from [Releases](https://github.com/MagmaVRC/UdonQuic/releases) and use one of:

- **`dev.magmavrc.udonquic-<version>.zip`:** in Unity, open Window > Package Manager, choose + > Add package from tarball/disk... and pick the extracted `package.json`. Alternatively, extract it into your project's `Packages/` folder.
- **`UdonQuic-<version>.unitypackage`:** Assets > Import Package > Custom Package. It installs into `Packages/dev.magmavrc.udonquic`.

Installing straight from the git URL is not supported: the native optimizer DLL is a build output that only the release files contain.

**Updating:** install the new release over the old one; the editor can stay open. Unity loads a private copy of `UdonQuic.dll` under `Library/UdonQuic/`, so the file is never locked. Use Tools > UdonQuic > Reload Native Optimizer to pick up the new one.

There is nothing to set up. Compiled programs are optimized in place from then on; Tools > UdonQuic > Enabled turns it off and recompiles everything plain. Tools > UdonQuic > Manager lists every program with what each pass changed, instruction and extern counts before and after, and a diff of the listings. Settings, including the passes to run and programs to exclude, live in Project Settings > UdonQuic.

## Layout

| path | contents |
|---|---|
| `include/UdonQuic/` | public C++ API (`Program.hpp`, `Optimizer.hpp`) and the C API (`UdonQuic.h`) |
| `src/` | the optimizer |
| `tests/` | differential tests that run programs before and after optimization on a reference interpreter |
| `projects/` | Visual Studio projects; open `UdonQuic.slnx` |
| `unity/dev.magmavrc.udonquic` | the Unity editor package |

Projects:

- **UdonQuic.Core**: static library with the optimizer.
- **UdonQuic.Native**: `UdonQuic.dll`, exposing the C API for P/Invoke.
- **UdonQuic.Tests**: console test runner.

## Building

Requires Windows and Visual Studio 2022 or later with the C++ workload.

```
git clone https://github.com/MagmaVRC/UdonQuic
cd UdonQuic
pwsh tools/build.ps1
```

`tools/build.ps1` builds the static library, `UdonQuic.dll` and the tests into `build/Release`, then runs the tests. `tools/package.ps1` builds and produces the release files in `dist/`: the package `.zip`, the `.unitypackage`, a native SDK zip and `SHA256SUMS.txt`.

Releases are published by pushing a `v<version>` tag that matches the package version.

The test runner also takes a folder of programs dumped from a project:

- `UdonQuic.Tests --corpus <dir>` optimizes every program, checks that the output decodes and reports totals and timings.
- `UdonQuic.Tests --fuzz <dir|file> [passes]` runs every entry point of the original and the optimized program on the reference interpreter, with stub externs that return values derived from their operands, reports any program whose extern calls or visible variables differ, and totals the instructions and externs executed before and after.
- `UdonQuic.Tests --bench <dir> [threads]` optimizes every program serially and then on several threads, reports both times and checks the outputs match.
- `UdonQuic.Tests --disassemble <file> [passes]` prints a listing.

## What it does

Every program is first decoded with a full stack proof: every stack shape at every instruction, every call and return, every jump table and every code address held in the heap must be proven, or the program is left as compiled. Each pass then rewrites the program, the result is decoded again, and a pass whose output does not verify is dropped. Passes repeat until nothing changes.

| pass | effect |
|---|---|
| dead code | removes unreachable instructions, `NOP` and `ANNOTATION` |
| jumps | threads jump chains, removes jumps to the next instruction, copies return trampolines to the jumps that reach them, removes `PUSH`/`POP` pairs |
| tail calls | a call followed directly by a return becomes a jump, saving the return trampoline |
| inline | copies small method bodies (24 instructions by default), and methods with a single caller whatever their size, into the call site |
| switches | an if/else chain comparing one integer against six or more constants becomes a bounds check and a jump table |
| string concat | chains of two-string concatenations merge into the three- and four-argument `String.Concat` overloads |
| constant folding | evaluates operators on constants with C# semantics and simplifies identities such as `x + 0` and `b == true` |
| copies | copy propagation and reuse of results computed earlier in the same block |
| redundant checks | reuses a pure result or a state read (`Utilities.IsValid`, null compares, player and transform properties, `Time.time`, array elements) already computed on every path, across branches |
| loop hoisting | moves pure work that cannot change inside a loop, such as an array's `Length`, out of the loop |
| loop rotation | tests a loop's condition at the bottom with the inverted compare, saving a jump per iteration |
| branches | `!(a < b)` becomes `a >= b`; a branch on a negation branches on the operand |
| dead stores | removes writes nothing reads, and retargets an extern's result straight into the variable it was copied to |
| extern fusion | replaces several externs with the one Unity method that computes the same thing: component-wise vector arithmetic becomes one vector operator, and hand-written dot products, `sqrMagnitude`, `magnitude`, `Distance` and `Lerp` become the built-in call |
| stable reads | `Networking.LocalPlayer` and the local player's `playerId`, `displayName`, `isLocal` and `IsUserInVR` are fetched once per session behind a flag and reused across events |
| loop unrolling | counted loops with a constant trip count of eight or fewer are fully unrolled, removing the compare and branch per iteration and exposing the counter as a constant |
| heap compaction | runs last: removes private heap slots nothing references, merges duplicate constants, and lets private temporaries of the same type that are never live at the same time (counting values carried between events) share one slot; the heap is renumbered and `uq_result_slot_source` tells the host which input slot each result slot came from |
| empty per-frame events | an `Update`, `LateUpdate`, `FixedUpdate` or `PostLateUpdate` that does nothing is no longer exported, so the behaviour leaves UdonManager's per-frame loop |

Constant and copy propagation also runs across branches: a value known on every path into a block is used directly, and folding follows. Call sites that pass constant arguments get a larger inline budget, so the folded callee pays for itself.

Fused vector methods compute the same IEEE operations in the same order. Where the platform compiler fuses multiply-adds inside Unity's own methods, `Dot`, `magnitude`, `Distance` and `Lerp` can differ from the separate operations in the last bit.

Externs fall into five classes. Pure ones depend only on their operands. Reads (`IsValid`, player and transform getters, `Time.time`, array `Get`) change nothing and only see changes made by a write or a barrier. Writes (transform, rigidbody, renderer, animator, audio and text setters, array `Set`, `Debug.Log`) change engine state but never run Udon code. Everything else is a barrier, which may run any event of the behaviour.

What it never changes:

- exported and synced variables, entry points and their order, so `GetProgramVariable`, `SetProgramVariable`, `SendCustomEvent`, networking and the inspector behave the same;
- anything a barrier extern might observe;
- externs that can throw, so a behaviour that faults still faults at the same point and with the same visible state.

Only compiler temporaries, locals and constants (`__intnl_`, `__gintnl_`, `__lcl_`, `__const_`) are treated as private to the program. Values that survive from one event to the next are tracked, so a temporary read before it is written in an event is kept.

Every instruction of the output records the input instruction it came from (`uq_result_origin`), and the Unity package rebuilds UdonSharp's debug information from it, so runtime errors still point at the right source line.

## C API

`UdonQuic.h` is the whole interface. Build a program with `uq_program_create` (code in Udon's big-endian byte order), add each heap slot in address order with `uq_program_add_slot`, each exported entry point with `uq_program_add_entry` and each extern the code calls with `uq_program_add_extern` (its operand count, whether it has a result, and which operands it may write). `uq_optimize` returns a result with the new code, heap and entry points; slots past the input's count are new, and slots marked rewritten hold relocated code addresses.
