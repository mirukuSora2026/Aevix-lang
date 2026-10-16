<!-- ========================

Created By : miruku_sora2026

File name : README.md

Description :
    Aevix의 현재 구현 범위와 확장 설치, 컴파일러 사용법을 안내한다.
    Describe the current Aevix implementation, extension installation, and compiler usage.

======================== -->

# Aevix

Aevix는 소유권과 대여 검사를 사용하는 정적 타입 언어입니다. `.avx` 소스를 네이티브 코드로 컴파일하며, AOT 실행과 LLVM ORC 기반 JIT 실행을 지원합니다.

**0.1V는 첫 공개 버전입니다.** 기본 컴파일러와 VS Code 확장을 사용할 수 있습니다. 언어 문서에는 앞으로 구현할 기능도 함께 적혀 있어, 문서에 있는 모든 API가 이 버전에서 동작하는 것은 아닙니다.

[0.1V 다운로드](https://github.com/mirukuSora2026/Aevix-lang/releases/tag/v0.1.0) · [설치 안내](https://github.com/mirukuSora2026/Aevix-lang/releases/download/v0.1.0/INSTALL.md) · [언어 문서](htmls/)

## VS Code에 설치하기

VS Code **1.96 이상**에서 사용할 수 있습니다.

1. [릴리스 페이지](https://github.com/mirukuSora2026/Aevix-lang/releases/tag/v0.1.0)에서 `aevix-0.1.0.vsix`를 받습니다.
2. VS Code의 확장 화면에서 `…` → **Install from VSIX…**를 선택합니다.
3. 내려받은 파일을 선택하고 `.avx` 파일을 엽니다.

`code` 명령을 사용할 수 있다면 다운로드한 폴더에서 다음 명령으로 설치해도 됩니다.

```sh
code --install-extension aevix-0.1.0.vsix
```

문법 강조, 스니펫, 자동완성, 정의 이동 등을 제공합니다. 확장에는 컴파일러가 포함되어 있지 않습니다. 오류 검사와 빌드·실행을 사용하려면 `aevixc`를 별도로 빌드해야 합니다.

## 컴파일러 빌드하기

CMake **3.20 이상**, C 컴파일러와 C++20 컴파일러, 64비트 빌드 환경이 필요합니다. Aevix 프로그램을 네이티브 실행 파일로 만들 때는 별도의 Clang과 링커·운영체제 SDK가 필요합니다. 현재 실행 검증에 사용한 Clang/LLVM 버전은 **22.1.8**입니다.

다음은 JIT를 제외한 AOT 빌드입니다. 이 설정은 LLVM 개발 라이브러리를 요구하지 않습니다.

```sh
git clone https://github.com/mirukuSora2026/Aevix-lang.git
cd Aevix-lang
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAEVIX_ENABLE_JIT=OFF -DBUILD_TESTING=OFF
cmake --build build --config Release --parallel 2
```

macOS와 Linux에서는 `./build/aevixc --version`으로 확인합니다. Windows에서 Visual Studio의 다중 구성 빌드를 사용했다면 실행 파일은 `build/Release/aevixc.exe`에 생성됩니다.

JIT까지 빌드하려면 LLVM **22 이상 개발 패키지**를 설치하고 `AEVIX_ENABLE_JIT=ON`으로 구성하세요. CMake가 찾지 못하면 `LLVMConfig.cmake`가 있는 디렉터리를 `LLVM_DIR`로 지정해야 합니다. JIT 실행 환경에도 해당 LLVM 라이브러리가 필요합니다.

## 코드 실행하기

아래 코드를 `main.avx`로 저장합니다.

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

정상이면 별도 출력 없이 종료 코드 `0`으로 끝납니다.

```sh
./build/aevixc main.avx
./build/aevixc --check main.avx
./build/aevixc --with-starter main.avx
```

첫 번째 명령은 컴파일 후 실행하고 임시 결과물을 정리합니다. `--check`는 실행하지 않고 검사합니다. `--with-starter`는 소스 옆에 실행 파일을 남기고 macOS/Linux에서는 `.sh` 실행 스크립트도 만듭니다. Windows에서는 `.exe`를 생성합니다. 같은 이름의 결과물이 있으면 덮어쓰지 않습니다.

Clang을 찾지 못하거나 다른 버전을 사용하려면 `--clang`으로 실행 파일 경로를 지정할 수 있습니다.

## VS Code와 컴파일러 연결하기

저장소를 VS Code에서 열면 확장이 `build/aevixc` 등의 위치를 자동으로 찾습니다. 직접 지정하려면 설정 JSON에 추가합니다.

```json
{
  "aevix.compilerPath": "${workspaceFolder}/build/aevixc"
}
```

Windows의 다중 구성 빌드에서는 `"${workspaceFolder}/build/Release/aevixc.exe"`를 사용하세요. 소스를 한 번 저장하고 작업 공간을 신뢰한 뒤 명령 팔레트에서 **Aevix: Check Current File**, **Build Current File**, **Run Current File**을 실행할 수 있습니다. 빌드와 실행 전에는 파일을 저장해야 합니다.

Remote SSH나 원격 환경을 사용한다면 확장·컴파일러·소스를 같은 호스트에 두어야 합니다.

## 현재 구현한 범위

- 타입 검사, 모듈과 import, 소유권 이동과 읽기·수정 대여 검사.
- struct, enum, interface, 제네릭 함수·메서드, 명시적 `dyn` 호출.
- 정적·동적 배열, 정수 Range 반복, 가변 인수, 최소한의 Slice·Box 지원.
- `Optional`, `Result`, `?`, `persist`, 일반 스코프 종료 시 자원 정리.
- AOT 컴파일, 지원 환경의 ORC JIT 실행, VS Code 편집 도구와 컴파일러 진단.

| 대상 | 현재 상태 |
| --- | --- |
| macOS Apple Silicon | AOT·JIT 실행 확인 |
| Linux ARM64 | 가상 환경에서 AOT·JIT 실행 확인 |
| Linux x86-64 | QEMU CPU 에뮬레이션에서 AOT·JIT 실행 확인 |
| Windows x86-64 | AOT 교차 빌드·링크 확인. 네이티브 실행·MSVC는 미검증 |

GPU·Tensor 실행, 애플리케이션 병렬 처리, 일반 표준 라이브러리는 아직 포함하지 않습니다. VS Code 확장의 이름 탐색은 소스 인덱스 기반이며, 타입·소유권 오류의 최종 판단은 컴파일러가 합니다. 포매터와 디버거는 제공하지 않습니다.

## 소스와 문서

- [`include/aevix/`](include/aevix/), [`src/`](src/): 컴파일러 선언과 구현.
- [`runtime/`](runtime/): 네이티브 런타임.
- [`tests/`](tests/): 컴파일러와 실행 경로 검사.
- [`vscode/`](vscode/): TypeScript 확장 소스.
- [`htmls/`](htmls/): 언어 설계와 자료형 문서. 내려받은 저장소의 `htmls/Aevix.html`을 브라우저에서 열어 읽을 수 있습니다.
