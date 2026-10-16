<!-- ========================

Created By : miruku_sora2026

File name : CONTRIBUTING.md

Description :
    Aevix 빌드·테스트 방법과 변경 기여 절차를 설명한다.
    Describe how to build, test, and contribute changes to Aevix.

======================== -->

# Contributing to Aevix

Aevix is still under development. Small changes with a clear purpose are easier to review than a large collection of unrelated fixes.

For a bug report, include the compiler version, operating system and CPU architecture, command used, a small `.avx` program, and the actual and expected results. Remove personal paths and confidential source code from logs before sharing them.

Discuss changes to ownership rules, module boundaries, public syntax, or the supported platforms in an issue before implementing them. Existing language decisions and executable tests take precedence over examples marked as design drafts.

## Build

Use CMake 3.20 or later and a C++20 compiler. A 64-bit toolchain is required. Native execution also needs Clang, a suitable linker, and the target platform's development files.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DAEVIX_ENABLE_JIT=OFF
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

To test JIT execution, configure a separate build with `-DAEVIX_ENABLE_JIT=ON` and LLVM 22 or later. Set `LLVM_DIR` to the directory containing `LLVMConfig.cmake` if needed. Check the CMake output to confirm JIT is enabled; requesting it does not guarantee LLVM was found.

With Visual Studio, add `--config Debug` to the build command and `-C Debug` to CTest. The current platform scope is macOS Apple Silicon, Linux ARM64 and x86-64, and Windows x86-64. State which platforms you actually tested. Cross-compilation alone does not establish native execution support.

## Source changes

- Keep declarations in `include/aevix/` and implementations in `src/`. Runtime code belongs in `runtime/`.
- Keep the existing file banner, with the actual file name and a Korean description followed by English. Write explanatory code comments in Korean followed by English. Files under `htmls/` and `.avx` examples are exempt from this banner rule.
- Preserve type-first declarations, explicit semicolons, and the established ownership and borrowing rules in language examples.
- Add focused positive and negative tests for behavior changes. Changes to lowering or runtime behavior should exercise generated code, including AOT and JIT where available.
- Check failure paths as well as success: overflow, allocation failure, invalid input, moves, and borrowed lifetimes matter.
- Avoid machine-specific paths in source or distributable binaries. Keep credentials, build logs, private measurements, and local package evidence out of commits.

When changing language behavior, update the relevant HTML reference and the VS Code grammar, snippets, or language index as needed. Mark planned features clearly until implementation and execution tests exist.

## VS Code extension

```sh
cd vscode
npm ci
npm test
npm run package
```

The extension's compiler checks require an `aevixc` build. Tests that start VS Code may require a graphical session and a downloaded VS Code test runtime; describe any checks you could not run.

## Installers

Installer sources live under `packaging/`, with build scripts under `tools/packaging/`. Test PATH changes and uninstall behavior with an isolated home directory or a disposable machine. Do not use an installer regression test to modify your everyday shell configuration.

## Pull requests

Explain the problem, the resulting behavior, and any relevant compatibility changes. Include a small before/after example when it helps. Keep unrelated refactoring and generated artifacts separate. Use descriptive English commit messages and PR descriptions.

Before submitting, inspect the staged diff for accidental files and personal information. Include the checks you ran and their results in the PR description. Do not claim coverage for platforms or execution modes you did not test.

## License

Aevix is licensed under [Apache License 2.0](LICENSE). Unless explicitly stated otherwise, contributions submitted for inclusion in Aevix are offered under the same license. Preserve the notices and licenses of bundled third-party components.
