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

**[Download 0.2V preview](https://github.com/mirukuSora2026/Aevix-lang/releases/tag/v0.2.0)** · [Installation guide](packaging/INSTALL.txt) · [0.2 implementation profile](htmls/releases/0_2.html)

0.2V is a prerelease. Compiler installer filenames retain `0.2.0-dev`; these are the packages distributed by the `v0.2.0` preview. [0.1V](https://github.com/mirukuSora2026/Aevix-lang/releases/tag/v0.1.0) remains the latest stable release. The implementation profile describes supported compiler behavior; the broader [language reference](htmls/index.html) also includes design proposals and APIs that are not implemented.

## Install

Choose the package for your operating system and CPU on the [0.2V preview release page](https://github.com/mirukuSora2026/Aevix-lang/releases/tag/v0.2.0). Follow the [installation guide](packaging/INSTALL.txt) for commands, installation locations, PATH setup, upgrades and removal.

- macOS Apple Silicon: a system `.pkg` or a per-user `.run` installer.
- Linux ARM64/x86-64: a per-user `.run` installer with marked shell PATH entries.
- Windows x86-64: an AOT-only setup `.exe` or portable ZIP. Native Windows installation and execution remain unverified.

After installation, open a new terminal and run:

```sh
aevixc --version
aevixc --check-deps
```

The packages include the Aevix runtime and required compiler libraries. AOT execution still needs an external Clang driver, linker and matching C runtime/SDK; these are not bundled. Use `--clang /path/to/clang` if discovery fails. GNU GCC is not required on every platform.

`--check-deps` compiles and links a small runtime probe without executing it. Use `-c` for an object-generation-only probe. Full cross-target checks need a matching runtime through `--runtime FILE` and the target toolchain/sysroot.

The macOS preview requires **macOS 26.0 or newer**. The compiler and bundled libraries are ad hoc signed; the `.pkg` is unsigned and is not notarized. Each installer records and checks its minimum OS version.

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

This example prints nothing and returns zero on success. The planned `system.print`, `println`, `printf` and `input` APIs are not part of 0.2.

## Implemented in 0.2

The compiler supports structs, payload enums, interfaces, explicit `dyn`, generic functions, imports, `Optional`, `Result`, `?`, `persist`, fixed/dynamic arrays, Range loops and homogeneous variadic arguments. The preview adds:

- Typed numeric literals, checked conversions, `i128`/`u128`, and portable `f16`/`bf16` values.
- UTF-8 owners and views, byte/scalar operations, checked subslices, tuples and repeated fixed arrays.
- Type aliases, partial explicit generic arguments, generic defaults, constant generic sizes, imported `Complex<T>` values and explicit-capture closures with `Fn`/`FnMut`/`FnOnce` contracts.
- Imported CPU SIMD and scoped parallel iteration with named, validated workers.
- `@bench` function timing and sampled process RSS, improved JIT module management, dependency diagnostics and native installers.

The [implementation profile](htmls/releases/0_2.html) specifies the supported operations and restrictions. GPU/Tensor execution, the full standard-library catalog, user-defined resource destructors, a formatter and a debugger are outside this release.

## Platform support

| Target | Preview scope |
| --- | --- |
| macOS Apple Silicon | AOT and ORC JIT; macOS 26.0+; per-user `.run` and system `.pkg` |
| Linux ARM64 | AOT and ORC JIT; per-user `.run`; execution checked in an ARM64 VM |
| Linux x86-64 | AOT and ORC JIT; per-user `.run`; execution checked under QEMU |
| Windows x86-64 | Experimental AOT-only setup/ZIP; cross-built packages, with native Windows and MSVC execution unverified |

A successful cross-build does not prove native execution. The macOS per-user `.run` was exercised; the `.pkg` payload was inspected without a system installation. macOS Intel, Windows ARM64 and 32-bit targets are outside this preview.

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

Download the VSIX from the [0.2V preview release page](https://github.com/mirukuSora2026/Aevix-lang/releases/tag/v0.2.0), then use **Extensions → … → Install from VSIX…** in VS Code 1.96 or newer.

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

These are future milestones, not features of the 0.2 preview. See the [roadmap to 0.5](htmls/roadmap/index.html) for prerequisites and exit gates.

| Version | Planned work |
| --- | --- |
| 0.3 | Native resource cleanup and FFI boundaries; collections, text, IO, paths and process libraries; [console IO through `import system`](htmls/roadmap/System_IO_0_3.html), including `print`, `println`, `printf` and `input` |
| 0.4 | Port the compiler to Aevix while retaining C++ as stage0; add `do-while` and review the [`try`/error-scope design](htmls/roadmap/Syntax_0_4.html) |
| 0.5 | Bootstrap the Aevix-written compiler through stage1, stage2 and stage3, compare outputs, and run the regression suite to establish self-hosting |

The formatting-call contract and `try` error model still need review. `try` does not imply an approved `throw`/`catch` unwinding model. The external roadmap review is incomplete; performance-changing optimization remains on hold pending its final review.

## Project

`include/aevix/` and `src/` contain the compiler; `runtime/` contains native support; `library/` contains imported language modules; `tests/` holds compiler and execution checks. `vscode/` is the TypeScript extension and `htmls/` is the documentation site.

The [release/0.1 branch](https://github.com/mirukuSora2026/Aevix-lang/tree/release/0.1) preserves the 0.1 source line.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the development workflow. Aevix is licensed under [Apache 2.0](LICENSE).
