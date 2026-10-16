<!-- ========================

Created By : miruku_sora2026

File name : README.md

Description :
    구현 범위, 빌드·설치, 실행 방법과 문서의 경계를 설명한다.
    Describe implemented scope, build and installation, execution, and reference boundaries.

======================== -->

<img src="design/logo/aevix-logo.png" width="160" height="160" alt="Aevix blue circuit A logo">

# Aevix

Aevix is a statically typed language with ownership and borrow checking. It compiles `.avx` files to native code and can execute them through LLVM ORC JIT. Values use native storage and explicit ownership; there is no tracing garbage collector or reserved JVM-style heap.

This checkout is **0.2 development**. The published release is still [0.1V](https://github.com/mirukuSora2026/Aevix-lang/releases/tag/v0.1.0). The [0.2 implementation profile](htmls/releases/0_2.html) describes the working compiler. The broader [language reference](htmls/index.html) also contains proposed APIs.

## Build

Requirements: CMake 3.20+, a C compiler, a C++20 compiler, and a 64-bit host toolchain.

```sh
git clone https://github.com/mirukuSora2026/Aevix-lang.git
cd Aevix-lang
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAEVIX_ENABLE_JIT=OFF
cmake --build build --config Release --parallel 2
ctest --test-dir build --output-on-failure --build-config Release
```

The AOT-only compiler builds without LLVM development libraries. AOT execution and linking require an external Clang driver, a linker and the target's C runtime/SDK. Set `--clang /path/to/clang` when needed. GNU GCC is not required on every platform.

For JIT, configure with `-DAEVIX_ENABLE_JIT=ON` and LLVM 22+ development files. Set `LLVM_DIR` to the directory containing `LLVMConfig.cmake` if CMake cannot find them. Check the configuration output and `aevixc --version` to confirm JIT was enabled. JIT execution uses the running host's architecture.

On macOS/Linux the executable is `build/aevixc`. A Visual Studio multi-configuration build puts it in `build/Release/aevixc.exe`. The examples below assume `aevixc` is on PATH.

## Install

The [installation guide](packaging/INSTALL.txt) covers PATH setup, upgrades and removal. The 0.2 packaging scripts produce:

- macOS ARM64: a `.pkg` that installs under `/Library/Aevix` and registers `/etc/paths.d/aevix`, plus a per-user `.run` alternative.
- Linux ARM64/x86-64: a per-user `.run` installer with marked shell PATH entries.
- Windows x86-64: a per-user setup `.exe` with a user PATH entry and uninstaller.

Build local packages from a completed compiler build:

```sh
sh tools/packaging/build-installers.sh --build-dir build --version 0.2.0
```

Distribution builds should use `-DAEVIX_PORTABLE_PACKAGE=ON` and remove machine-specific paths from debug metadata. Windows packaging uses `tools/packaging/build-windows-installer.sh`. The packages include the Aevix runtime and required compiler libraries, but do not install Clang or a platform SDK. The current macOS development artifacts require macOS 26.0 or newer; each installer records and checks its actual minimum. Local macOS packages are not Developer ID signed or notarized.

CPack source packaging is disabled; use the repository's public GitHub source archives.

After installation, open a new terminal and run:

```sh
aevixc --version
aevixc --check-deps
```

`--check-deps` compiles and links a small runtime probe without executing it. Use `-c` for an object-generation-only probe. Full cross-target checks need a matching runtime through `--runtime FILE` and the target toolchain/sysroot.

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

The first command compiles and runs, then removes temporary output. `--check` checks types and ownership. `--jit` requires a JIT-enabled build. `--with-starter` keeps an executable beside the source and a `.sh` launcher on macOS/Linux; Windows uses an `.exe`. Existing output files are not overwritten. This example prints nothing and returns zero on success.

## 0.2 work

The compiler includes structs, payload enums, interfaces, explicit `dyn`, generic functions, imports, `Optional`, `Result`, `?`, `persist`, fixed/dynamic arrays, Range loops and homogeneous variadic arguments. The current 0.2 work extends that foundation with:

- Typed numeric literals, checked conversions, `i128`/`u128`, and portable `f16`/`bf16` values.
- UTF-8 owners and views, byte/scalar operations, checked subslices, tuples and repeated fixed arrays.
- Type aliases, generic defaults, constant generic sizes, imported `Complex<T>` values and explicit-capture closures with `Fn` contracts.
- Imported CPU SIMD and scoped parallel iteration with named, validated workers.
- `@bench` function timing and sampled process RSS, improved JIT module management, dependency diagnostics and native installers.

The profile pages specify supported operations and restrictions. GPU/Tensor execution and the full standard-library catalog are outside this release. User-defined resource destructors, a formatter and a debugger are not included. Self-hosting is planned for 0.5.

| Target | Qualification boundary |
| --- | --- |
| macOS Apple Silicon | 43/43 CTest gates; relocated installer, PATH, AOT/JIT and removal checks passed |
| Linux ARM64 | Fresh LLVM 22 JIT build in an ARM64 VM: 43/43 CTest gates; installed AOT/JIT, PATH and removal passed |
| Linux x86-64 | Fresh LLVM 22 JIT build under QEMU: 43/43 CTest gates; installed AOT/JIT, PATH and removal passed |
| Windows x86-64 | AOT-only portable setup/ZIP and nine COFF/static-link probes passed; native Windows and MSVC execution remain unverified |

A successful cross-build does not prove native execution. macOS installer execution was tested with the per-user `.run`; the `.pkg` payload was inspected without a system installation. Native Windows execution remains an outstanding release gate.

The current Linux artifacts were qualified on Ubuntu 24.04.5 LTS with glibc 2.39. They target glibc-based systems; broader distribution compatibility requires separate checks.

## VS Code

Build the extension with Node.js and npm:

```sh
cd vscode
npm ci
npm test
npm run package
code --install-extension dist/aevix-0.2.0.vsix
```

Requires VS Code 1.96+. You can also use **Extensions → … → Install from VSIX…**. The extension provides highlighting, snippets, completion and navigation. Compiler diagnostics, builds and execution require `aevixc` separately.

The extension searches workspace build locations and PATH. To choose a compiler explicitly:

```json
{
  "aevix.compilerPath": "${workspaceFolder}/build/aevixc"
}
```

Save the file and trust the workspace before using **Aevix: Check Current File**, **Build Current File**, or **Run Current File**. Remote workspaces need the compiler on the same host as the extension and source files.

## Project

`include/aevix/` and `src/` contain the compiler; `runtime/` contains native support; `library/` contains imported language modules; `tests/` holds compiler and execution checks. `vscode/` is the TypeScript extension and `htmls/` is the documentation site.

The [roadmap to 0.5](htmls/roadmap/index.html) lists the library and compiler prerequisites for self-hosting. Its external review is incomplete; performance-changing optimization is held pending the final Claude review.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the development workflow. Aevix is licensed under [Apache 2.0](LICENSE).
