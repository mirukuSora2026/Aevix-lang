<!-- ========================

Created By : miruku_sora2026

File name : README.md

Description :
    구현 범위, 빌드·설치, 실행 방법과 문서의 경계를 설명한다.
    Describe implemented scope, build and installation, execution, and reference boundaries.

======================== -->

<img src="design/logo/aevix-logo.png" width="160" height="160" alt="Aevix blue circuit A logo">

# Aevix

Aevix is a statically typed language with ownership and borrow checking. It compiles `.avx` files to native code and supports execution through LLVM ORC JIT. Memory uses native storage and ownership-based cleanup, without a tracing garbage collector or a reserved JVM-style heap.

**[Download 0.3V](https://github.com/mirukuSora2026/Aevix-lang/releases/tag/v0.3.0)** · [Installation guide](packaging/INSTALL.txt) · [Source API inventory](htmls/releases/api/Library_Status.html)

Compiler packages and the VS Code extension use version **0.3.0**. The [0.2V preview](https://github.com/mirukuSora2026/Aevix-lang/releases/tag/v0.2.0) and [0.1V release](https://github.com/mirukuSora2026/Aevix-lang/releases/tag/v0.1.0) remain available. The broader [language reference](htmls/index.html) includes design proposals and APIs that are not implemented.

## New in 0.3

0.3 adds console IO, resource cleanup and imported CPU library modules to the existing native execution model.

- Add console IO through `import system`, typed parsing, structured logging, and the common Result-based error model.
- Extend resource cleanup with `Drop`, typed native handles and borrowed buffers. Add imported CPU modules for collections, text and Unicode, numeric operations, filesystem and process access, shared ownership, synchronization and threaded Tasks.
- Refine raw literals, closure consumption, generic inference, resource lifetimes and parallel worker checks. Include an experimental, opt-in range-proof pass; unproved arithmetic and accesses retain their runtime checks.
- Include reproducible native benchmark controls, exact-output oracles and differential/metamorphic test tools. Performance reports distinguish timed code from setup, actual generated instructions from source abstractions, and measured features from planned ones.

The [source API inventory](htmls/releases/api/Library_Status.html) describes the provided surface. **The full CPU catalog is not complete.** Async/coroutines and GPU execution remain deferred, while reusable worker pools and adaptive JIT tiering are not implemented. A generated-program ASan discrepancy for zero-sized `Box<unit>` at O0 remains under investigation; see the [recorded issue](benchmarks/Comparison/cpu4096/reports/proof_zero_size_issue.json). The Windows package is cross-built and remains experimental; native Windows execution is unverified.

## Install

Choose the package for your operating system and CPU on the [0.3V release page](https://github.com/mirukuSora2026/Aevix-lang/releases/tag/v0.3.0). Follow the [installation guide](packaging/INSTALL.txt) for commands, installation locations, PATH setup, upgrades and removal.

- macOS Apple Silicon: a system `.pkg` or a per-user `.run` installer.
- Linux ARM64/x86-64: a per-user `.run` installer with marked shell PATH entries.
- Windows x86-64: an AOT-only setup `.exe` or portable ZIP. Native Windows installation and execution remain unverified.

After installation, open a new terminal and run:

```sh
aevixc --version
aevixc --check-deps
```

The packages include the Aevix runtime and required compiler libraries. AOT execution still needs an external Clang driver, linker and matching C/C++ runtime and development libraries/SDK; these are not bundled. Use `--clang /path/to/clang` if discovery fails. GNU GCC is not required on every platform.

`--check-deps` compiles and links a small runtime probe without executing it. Use `-c` for an object-generation-only probe. Full cross-target checks need a matching runtime through `--runtime FILE` and the target toolchain/sysroot.

The macOS package requires **macOS 26.0 or newer**. The compiler and bundled libraries are ad hoc signed; the `.pkg` is unsigned and is not notarized. Each installer records and checks its minimum OS version.

## First program

Save as `main.avx`. Module filename stems and folders below the source root currently use ASCII identifiers: a letter or `_`, followed by letters, digits or `_`.

```aevix
i32 sum(i32 values[]) {
    mut i32 total = 0;
    for (i32 value : values) total += value;
    return total;
}

i32 main() {
    i32 answer = sum(10, 20, 12);
    return answer == 42 ? 0 : 1;
}
```

```sh
aevixc main.avx
aevixc --check main.avx
aevixc --jit -O2 main.avx
aevixc --with-starter main.avx
```

The first command compiles and runs, then removes temporary output. `--check` checks types and ownership. `--jit` requires a JIT-enabled compiler and runs on the host architecture. `--with-starter` keeps an executable beside the source and a `.sh` launcher on macOS/Linux; Windows uses an `.exe`. Existing output files are not overwritten.

This example prints nothing and returns zero on success. To write to the terminal, import `system`:

```aevix
import system;

i32 main() {
    system.println("Hello, Aevix!");
    return 0;
}
```

`system` also provides `print`, `printf`, typed `input` and structured logging. Input and output operations return error values; handle those results when the program needs to recover from IO failures.

## Language features

The compiler supports structs, payload enums, interfaces, explicit `dyn`, generic functions, imports, `Optional`, `Result`, `?`, `persist`, fixed/dynamic arrays, Range loops and homogeneous variadic arguments. Features retained from 0.2 include:

- Typed numeric literals, checked conversions, `i128`/`u128`, and portable `f16`/`bf16` values.
- UTF-8 owners and views, byte/scalar operations, checked subslices, tuples and repeated fixed arrays.
- Type aliases, partial explicit generic arguments, generic defaults, constant generic sizes, imported `Complex<T>` values and explicit-capture closures with `Fn`/`FnMut`/`FnOnce` contracts.
- Imported CPU SIMD and scoped parallel iteration with named, validated workers.
- `@bench` function timing and sampled process RSS, improved JIT module management, dependency diagnostics and native installers.

The [0.2 implementation profile](htmls/releases/0_2.html) records the previous baseline; the [source API inventory](htmls/releases/api/Library_Status.html) tracks the expanded library surface. 0.3 adds user-defined `Drop` cleanup. GPU execution, the full standard-library catalog, async/coroutines, a formatter and a debugger remain outside this release.

## Platform support

| Target | Package scope |
| --- | --- |
| macOS Apple Silicon | AOT and ORC JIT; macOS 26.0+; per-user `.run` and system `.pkg` |
| Linux ARM64 | AOT and ORC JIT; per-user `.run`; execution checked in an ARM64 VM |
| Linux x86-64 | AOT and ORC JIT; per-user `.run`; execution checked under QEMU |
| Windows x86-64 | Experimental AOT-only setup/ZIP; cross-built packages, with native Windows and MSVC execution unverified |

A successful cross-build does not prove native execution. The macOS per-user `.run` was exercised; the `.pkg` payload was inspected without a system installation. macOS Intel, Windows ARM64 and 32-bit targets are outside this release.

The current Linux artifacts were qualified on Ubuntu 24.04.5 LTS with glibc 2.39. They target glibc-based systems; broader distribution compatibility requires separate checks.

## Build from source

The `main` branch contains the current source. Requirements: CMake 3.20+, a C compiler, a C++20 compiler, and a 64-bit host toolchain.

```sh
git clone --branch main https://github.com/mirukuSora2026/Aevix-lang.git
cd Aevix-lang
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAEVIX_ENABLE_JIT=OFF
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

This builds an AOT-only compiler without LLVM development libraries. AOT execution and linking still need the external toolchain described above. On macOS/Linux the executable is `build/aevixc`. A Visual Studio multi-configuration build puts it in `build/Release/aevixc.exe`.

For JIT, install LLVM 22+ development files, then reconfigure and rebuild:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAEVIX_ENABLE_JIT=ON
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

Set `LLVM_DIR` to the directory containing `LLVMConfig.cmake` if CMake cannot find it. Requesting JIT does not guarantee it was enabled: check the configuration output and `aevixc --version`. The Windows preview is AOT-only; native Windows JIT is not qualified.

To build distributable installers, follow [packaging/INSTALL.txt](packaging/INSTALL.txt). Use `AEVIX_PORTABLE_PACKAGE=ON` and remove machine-specific paths from debug metadata. CPack source packaging is disabled; public source archives are available from GitHub.

## VS Code

Download the VSIX from the [0.3V release page](https://github.com/mirukuSora2026/Aevix-lang/releases/tag/v0.3.0), then use **Extensions → … → Install from VSIX…** in VS Code 1.96 or newer.

To build the extension from source, run these commands from the repository root:

```sh
cd vscode
npm ci
npm test
npm run package
```

Install the resulting VSIX from `vscode/dist/`. The extension provides highlighting, snippets, completion and navigation. Compiler diagnostics, builds and execution require `aevixc` separately.

The extension searches workspace build locations and PATH. To choose a compiler explicitly:

```json
{
  "aevix.compilerPath": "${workspaceFolder}/build/aevixc"
}
```

Save the file and trust the workspace before using **Aevix: Check Current File**, **Build Current File**, or **Run Current File**. Remote workspaces need the compiler on the same host as the extension and source files.

## Next versions

These milestones build on 0.3. See the [roadmap to 0.5](htmls/roadmap/index.html) for the broader language and self-hosting plan. Correctness checks apply to every optimization release.

| Version | Planned work |
| --- | --- |
| 0.3.1 | Extend range proofs and examine checked-arithmetic elimination and vectorization in matrix and struct loops |
| 0.3.2 | Measure and optimize ORC JIT phases, target alignment and cache invalidation |
| 0.3.3 | Add a reusable worker-pool path and compare it with the retained fresh-thread baseline |
| 0.3.4 | Expand differential fuzzing, sanitizer coverage, input ranges and ownership/concurrency regression cases |
| 0.4 | Port the compiler to Aevix while retaining C++ as stage0; add async/coroutine foundations and `do-while`, and review the [`try`/error-scope design](htmls/roadmap/Syntax_0_4.html) |
| 0.5 | Bootstrap the Aevix-written compiler through stage1, stage2 and stage3, compare outputs, and run the regression suite to establish self-hosting |

The `try` error model still needs review and does not imply an approved `throw`/`catch` unwinding model. Existing correctness failures are investigated when found; the 0.3.4 milestone does not postpone safety checks for earlier releases.

## Project

`include/aevix/` and `src/` contain the compiler; `runtime/` contains native support; `library/` contains imported language modules; `tests/` holds compiler and execution checks. `vscode/` is the TypeScript extension and `htmls/` is the documentation site.

The [release/0.1 branch](https://github.com/mirukuSora2026/Aevix-lang/tree/release/0.1) preserves the 0.1 source line.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the development workflow. Aevix is licensed under [Apache 2.0](LICENSE).
