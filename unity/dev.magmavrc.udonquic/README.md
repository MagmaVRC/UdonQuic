# UdonQuic

A native Udon bytecode optimizer for VRChat worlds. UdonQuic runs after the compiler: whenever UdonSharp, UdonLuau or any other tool stores a compiled Udon program, UdonQuic optimizes it and stores the result in its place, so the optimized program is the compiled output you test in Play Mode and upload.

It inlines small methods, turns calls into tail calls where it can, folds constants, removes redundant copies and dead stores, and simplifies branches. Exported variables, synced variables and entry points are left as they are, so `GetProgramVariable`, `SendCustomEvent`, networking and the inspector keep working.

Requires Windows, Unity 2022.3 or later, and the VRChat Worlds SDK 3.9.0 or later.

## Install

Add `https://vpm.magmamc.dev/index.json` to the VRChat Creator Companion under Settings > Packages > Add Repository, then add UdonQuic to your world project from Manage Project. Without VCC, extract a release zip and use Window > Package Manager > + > Add package from disk.

There is nothing to set up: programs are optimized from the next compile on. Tools > UdonQuic > Enabled turns it off.

## How it works

- Programs are optimized shortly after they compile, before they are saved, when a world build is requested, and when entering Play Mode.
- An optimized program carries a `__const_quic_optimized` heap slot, so it is never optimized twice.
- Programs compiled together are optimized in parallel. Results are cached in `Library/UdonQuic/cache`, keyed by the program's code, heap, externs, entry points, the pass settings and the optimizer build, so a program that compiles to the same thing again is not re-optimized. Entries unused for two weeks are deleted.
- A program the optimizer cannot handle, for example one that calls an extern the installed SDK does not know, is left as compiled and a warning says why. The warning is logged once per program content, not on every compile.
- Every result is recorded in `Library/UdonQuic/results.json`, with the listings before and after in `Library/UdonQuic/listings/<guid>.before.txt` and `.after.txt`, keyed by the serialized program asset's GUID.

## Runtime error lines

UdonSharp maps a runtime error's program counter to a source line through debug information it keeps in `Library/UdonSharpCache/DebugInfo`. After UdonQuic stores an optimized UdonSharp program, it rebuilds that information so every new instruction maps to the line of the instruction it came from; instructions the optimizer created take the line of the instruction before them. Inlined code reports the line inside the inlined method. The Manager shows whether a program's lines were remapped.

Remapping needs `UdonQuic.dll` 0.2.0 or later, which reports where each instruction came from. With an older library, error lines can point at the wrong line in optimized programs; exclude the program or turn UdonQuic off while tracking down an exception.

## Manager

Tools > UdonQuic > Manager lists every program UdonQuic has a result for, plus the open scenes' programs it has not seen yet, with the compiler, status, instructions and externs before and after, and what the passes did. The toolbar turns UdonQuic on or off, recompiles everything, optimizes every plain program now, and analyzes plain programs without storing them; the strip below totals the results. The list filters by status, compiler and text, and sorts by any column.

Selecting a program shows its source with Ping and Open, the optimizer's message, every counter, the optimizer version and when it ran, and its listing before, after, and as a diff that ignores addresses. Re-optimize recompiles the program; Exclude stores it as compiled from then on. UdonSharp compiles all of its programs together, so both ask before recompiling every program.

## Menus

Tools > UdonQuic:

- **Enabled** turns optimization on or off and recompiles every UdonSharp and UdonLuau program, so the plain or optimized output comes back.
- **Recompile All** recompiles every UdonSharp and UdonLuau program.
- **Manager** opens the Manager.
- **Settings** opens UdonQuic's page in Project Settings.
- **Reload Native Optimizer** loads `UdonQuic.dll` again after it was replaced.

## Settings

Settings live in Project Settings > UdonQuic and are stored in `ProjectSettings/UdonQuicSettings.asset`, so they can be committed with the project:

- **Enabled** (default on).
- **Inline Budget**, the largest method body in instructions that is inlined (default 24).
- **Passes**, each optimizer pass with its description; turning one off skips it.
- **Excluded Programs**, programs that are stored as compiled. Exclude them from the Manager.

Values from the editor preferences `UdonQuic_Enabled`, `UdonQuic_InlineBudget` and `UdonQuic_DisabledPasses` used by 0.1 are copied over once.

## License

MIT, see [LICENSE.md](LICENSE.md).
