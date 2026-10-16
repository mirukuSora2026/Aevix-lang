<!-- ========================

Created By : miruku_sora2026

File name : README.md

Description :
    Describe the current Aevix implementation, extension installation, and compiler usage.

======================== -->

# Aevix

Aevix is a statically typed language with ownership and borrow checking. It compiles `.avx` source files to native code and supports AOT execution and LLVM ORC JIT execution.

**0.1V is the first public release.** It includes the compiler core and a VS Code extension. The language reference also describes planned features; not every documented API is implemented in this version.

[Download 0.1V](https://github.com/mirukuSora2026/Aevix-lang/releases/tag/v0.1.0) · [Installation guide](https://github.com/mirukuSora2026/Aevix-lang/releases/download/v0.1.0/INSTALL.md) · [Language reference](htmls/)

## Install the VS Code extension

Requires **VS Code 1.96 or later**.

1. Download `aevix-0.1.0.vsix` from the [release page](https://github.com/mirukuSora2026/Aevix-lang/releases/tag/v0.1.0).
2. Open the Extensions view in VS Code and select `…` → `Install from VSIX…`.
3. Select the downloaded file, then open an `.avx` file.

If the `code` command is available, run this from your download directory instead:

```sh
code --install-extension aevix-0.1.0.vsix
```

The extension provides syntax highlighting, snippets, completion, and source navigation. It does not bundle the compiler. Diagnostics, builds, and execution require a separate `aevixc` build.

## Build the compiler

You need **CMake 3.20 or later**, a C compiler, a C++20 compiler, and a 64-bit toolchain. Building and running Aevix programs also requires an external Clang driver, linker, and platform SDK. The Clang/LLVM version used for execution checks is **22.1.8**.

This builds the AOT compiler without JIT support. LLVM development libraries are not required for this configuration.

```sh
git clone https://github.com/mirukuSora2026/Aevix-lang.git
cd Aevix-lang
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAEVIX_ENABLE_JIT=OFF -DBUILD_TESTING=OFF
cmake --build build --config Release --parallel 2
```

On macOS and Linux, check the result with `./build/aevixc --version`. With a Visual Studio multi-configuration build on Windows, the executable is `build/Release/aevixc.exe`.

For JIT support, install the **LLVM 22 or later development package** and configure with `AEVIX_ENABLE_JIT=ON`. If CMake cannot find LLVM, set `LLVM_DIR` to the directory containing `LLVMConfig.cmake`. The matching LLVM libraries must also be available at runtime.

## Run a program

Save this as `main.avx`:

```aevix
i32 sum(i32 values[]) {
    mut i32 total = 0;
    for (mut usize index = 0; index < Slice.len(values); index++) {
        total += values[index];
    }
    return total;
}

i32 main() {
    i32 answer = sum(10, 20, 12);
    return answer == 42 ? 0 : 1;
}
```

It produces no output and returns exit code `0` on success.

```sh
./build/aevixc main.avx
./build/aevixc --check main.avx
./build/aevixc --with-starter main.avx
```

The first command compiles and runs the program, then removes temporary build output. `--check` checks the source without running it. `--with-starter` saves a native executable beside the source and adds a `.sh` launcher on macOS/Linux. On Windows it creates an `.exe`. Existing output files are not overwritten.

Use `--clang` followed by the Clang executable path if Clang is not found or you need a different installation.

## Connect VS Code to the compiler

When you open the repository in VS Code, the extension searches locations such as `build/aevixc`. To set the path yourself, add this to your settings JSON:

```json
{
  "aevix.compilerPath": "${workspaceFolder}/build/aevixc"
}
```

For a Windows multi-configuration build, use `"${workspaceFolder}/build/Release/aevixc.exe"`. Save the source file at least once and trust the workspace before using **Aevix: Check Current File**, **Aevix: Build Current File**, or **Aevix: Run Current File** from the Command Palette. Build and Run require saved files.

With Remote SSH or another remote setup, keep the extension, compiler, and source files on the same host.

## Current scope

- Type checking, modules and imports, ownership moves, and shared/exclusive borrow checking.
- Structs, enums, interfaces, generic functions and methods, and explicit `dyn` dispatch.
- Fixed and dynamic arrays, integer Range loops, variadic arguments, and minimal Slice/Box support.
- `Optional`, `Result`, `?`, `persist`, and resource cleanup on normal scope exits.
- AOT compilation, ORC JIT execution on supported systems, and VS Code editing tools with compiler diagnostics.

| Target | Current status |
| --- | --- |
| macOS Apple Silicon | AOT and JIT execution checked |
| Linux ARM64 | AOT and JIT execution checked in a VM |
| Linux x86-64 | AOT and JIT execution checked under QEMU CPU emulation |
| Windows x86-64 | AOT cross-build and linking checked; native execution and MSVC remain unverified |

GPU/Tensor execution, application-level parallelism, and the general standard library are not included yet. The extension uses a source index for navigation; the compiler supplies type and ownership diagnostics. A formatter and debugger are not included.

## Source and documentation

- [`include/aevix/`](include/aevix/), [`src/`](src/): compiler declarations and implementation.
- [`runtime/`](runtime/): native runtime.
- [`tests/`](tests/): compiler and execution checks.
- [`vscode/`](vscode/): TypeScript extension sources.
- [`htmls/`](htmls/): language design and type reference. Download the repository and open `htmls/index.html` in a browser to read it.
