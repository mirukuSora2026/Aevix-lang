// ========================
//
// Created By : miruku_sora2026
//
// File name : Setup.cpp
//
// Description :
//     사용자 설치·제거와 환경 변수 갱신을 담당하는 64비트 Windows 설치기다.
//     Install and remove the 64-bit Windows compiler and update the user's PATH.
//
// ========================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef AEVIX_SETUP_VERSION
#define AEVIX_SETUP_VERSION L"0.2.0"
#endif

namespace {
namespace fs = std::filesystem;
constexpr wchar_t registryKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Aevix";
constexpr char managedMarker[] = "Aevix managed Windows installation v1\n";
bool silent = false;

struct LibraryFile {
    unsigned resource;
    const wchar_t* relative;
    const wchar_t* marker;
};
constexpr LibraryFile libraryFiles[] = {
    {106, L"share/aevix/library/Math/numeric.avx", L".aevix-library-numeric"},
    {107, L"share/aevix/library/Math/complex.avx", L".aevix-library-complex"},
    {108, L"share/aevix/licenses/GCC-GPL-3.0.txt", L".aevix-license-gcc-gpl"},
    {109, L"share/aevix/licenses/GCC-RUNTIME-EXCEPTION.txt", L".aevix-license-gcc-runtime"},
    {110, L"share/aevix/licenses/MinGW-w64-COPYING.txt", L".aevix-license-mingw"},
    {111, L"share/aevix/licenses/SOURCES.txt", L".aevix-license-sources"}
};

struct RegistryValue {
    std::wstring text;
    DWORD type = REG_EXPAND_SZ;
};

[[noreturn]] void fail(const char* message) { throw std::runtime_error(message); }

RegistryValue readPath() {
    RegistryValue value;
    HKEY key = nullptr;
    const auto opened = RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_QUERY_VALUE, &key);
    if (opened == ERROR_FILE_NOT_FOUND) return value;
    if (opened != ERROR_SUCCESS) fail("Cannot open the user's environment registry key.");
    DWORD bytes = 0;
    auto result = RegQueryValueExW(key, L"Path", nullptr, &value.type, nullptr, &bytes);
    if (result == ERROR_FILE_NOT_FOUND) { RegCloseKey(key); return {}; }
    if (result != ERROR_SUCCESS || (value.type != REG_SZ && value.type != REG_EXPAND_SZ)) {
        RegCloseKey(key); fail("The user PATH is unreadable or has an unsupported registry type.");
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 2, L'\0');
    result = RegQueryValueExW(key, L"Path", nullptr, &value.type,
                             reinterpret_cast<BYTE*>(buffer.data()), &bytes);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) fail("Cannot read the user's PATH.");
    value.text.assign(buffer.data());
    return value;
}

void setRegistryString(HKEY key, const wchar_t* name, const std::wstring& value, DWORD type = REG_SZ) {
    if (RegSetValueExW(key, name, 0, type, reinterpret_cast<const BYTE*>(value.c_str()),
                      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS)
        fail("Cannot update the registry.");
}

void writePath(const RegistryValue& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Environment", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        fail("Cannot write the user's environment registry key.");
    try { setRegistryString(key, L"Path", value.text, value.type); }
    catch (...) { RegCloseKey(key); throw; }
    RegCloseKey(key);
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 5000, &ignored);
}

std::wstring normalizedPath(std::wstring value) {
    const auto first = value.find_first_not_of(L" \t\"");
    if (first == std::wstring::npos) return {};
    value = value.substr(first, value.find_last_not_of(L" \t\"") - first + 1);
    const auto required = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (required) {
        std::vector<wchar_t> buffer(required);
        if (ExpandEnvironmentStringsW(value.c_str(), buffer.data(), required)) value = buffer.data();
    }
    std::replace(value.begin(), value.end(), L'/', L'\\');
    while (value.size() > 3 && value.back() == L'\\') value.pop_back();
    CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value;
}

std::vector<std::wstring> entries(const std::wstring& path) {
    std::vector<std::wstring> result;
    std::size_t begin = 0;
    do {
        const auto end = path.find(L';', begin);
        result.push_back(path.substr(begin, end == std::wstring::npos ? end : end - begin));
        if (end == std::wstring::npos) break;
        begin = end + 1;
    } while (true);
    return result;
}

bool addPath(const fs::path& directory) {
    auto value = readPath();
    const auto wanted = normalizedPath(directory.wstring());
    for (const auto& entry : entries(value.text)) if (normalizedPath(entry) == wanted) return false;
    if (!value.text.empty()) value.text += L';';
    value.text += directory.wstring();
    writePath(value);
    return true;
}

void removePath(const fs::path& directory) {
    auto value = readPath();
    std::wstring rebuilt;
    bool first = true;
    bool removed = false;
    const auto wanted = normalizedPath(directory.wstring());
    for (const auto& entry : entries(value.text)) {
        if (!removed && normalizedPath(entry) == wanted) { removed = true; continue; }
        if (!first) rebuilt += L';';
        rebuilt += entry;
        first = false;
    }
    if (removed) { value.text = std::move(rebuilt); writePath(value); }
}

fs::path installDirectory() {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &path)))
        fail("Cannot locate Local AppData.");
    const fs::path result = fs::path(path) / L"Programs" / L"Aevix";
    CoTaskMemFree(path);
    return result;
}

fs::path currentExecutable() {
    std::vector<wchar_t> path(32768);
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) fail("Cannot locate this installer.");
    return fs::path(std::wstring(path.data(), length));
}

bool isManaged(const fs::path& root) {
    std::ifstream input(root / L".aevix-install", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return text == managedMarker;
}

void writeText(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output || !output.write(text.data(), static_cast<std::streamsize>(text.size())))
        fail("Cannot write an installation file.");
}

void writeResource(unsigned identifier, const fs::path& path) {
    const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(identifier), RT_RCDATA);
    const auto loaded = resource ? LoadResource(nullptr, resource) : nullptr;
    const auto bytes = resource ? SizeofResource(nullptr, resource) : 0;
    const auto data = loaded ? LockResource(loaded) : nullptr;
    if (!data || !bytes) fail("This setup executable is missing a compiler or runtime resource.");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output || !output.write(static_cast<const char*>(data), bytes)) fail("Cannot extract the compiler payload.");
}

bool isReparsePoint(const fs::path& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

std::string readText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string libraryFingerprint(const fs::path& path) {
    if (!fs::is_regular_file(path) || isReparsePoint(path)) return {};
    std::ifstream input(path, std::ios::binary);
    if (!input) fail("Cannot read an existing library file.");
    // 이 표식은 설치 소유권과 사용자 변경을 구분하며 보안 서명으로 사용하지 않는다.
    // This marker detects ownership and user edits; it is not a security signature.
    std::uint64_t hash = 14695981039346656037ULL;
    std::uint64_t size = 0;
    char buffer[4096];
    while (input.read(buffer, sizeof(buffer)) || input.gcount()) {
        const auto count = input.gcount();
        for (std::streamsize index = 0; index < count; ++index) {
            hash ^= static_cast<unsigned char>(buffer[index]); hash *= 1099511628211ULL;
        }
        size += static_cast<std::uint64_t>(count);
    }
    if (!input.eof()) fail("Cannot read an existing library file.");
    return "Aevix owned library v1 " + std::to_string(size) + " " + std::to_string(hash) + "\n";
}

bool ownsLibrary(const fs::path& root, const LibraryFile& file) {
    if (isReparsePoint(root / file.marker)) return false;
    const auto fingerprint = libraryFingerprint(root / file.relative);
    return !fingerprint.empty() && readText(root / file.marker) == fingerprint;
}

void preflightLibraries(const fs::path& root) {
    for (const auto* directory : {L"share", L"share/aevix", L"share/aevix/library", L"share/aevix/library/Math", L"share/aevix/licenses"})
        if (isReparsePoint(root / directory)) fail("Refusing to install through a library directory link.");
    for (const auto& file : libraryFiles) {
        if (isReparsePoint(root / file.marker) || isReparsePoint(root / file.relative))
            fail("Refusing to replace a library link.");
        if (fs::exists(root / file.relative) && !ownsLibrary(root, file))
            fail("A bundled library destination contains an unmanaged or modified file. Nothing was replaced.");
    }
}

void installLibraries(const fs::path& root) {
    fs::create_directories(root / L"share" / L"aevix" / L"library" / L"Math");
    fs::create_directories(root / L"share" / L"aevix" / L"licenses");
    for (const auto& file : libraryFiles) {
        writeResource(file.resource, root / file.relative);
        writeText(root / file.marker, libraryFingerprint(root / file.relative));
    }
}

void removeLibraries(const fs::path& root) {
    // 명시적으로 소유하며 바뀌지 않은 파일만 제거한다. 추가·수정된 파일은 보존한다.
    // Remove only owned, unchanged files; preserve additions and user modifications.
    for (const auto& file : libraryFiles) {
        if (ownsLibrary(root, file)) fs::remove(root / file.relative);
        if (!isReparsePoint(root / file.marker)) fs::remove(root / file.marker);
    }
}

void registerUninstaller(const fs::path& root) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, registryKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        fail("Cannot register the uninstaller.");
    try {
        setRegistryString(key, L"DisplayName", L"Aevix " AEVIX_SETUP_VERSION);
        setRegistryString(key, L"DisplayVersion", AEVIX_SETUP_VERSION);
        setRegistryString(key, L"Publisher", L"Aevix");
        setRegistryString(key, L"InstallLocation", root.wstring());
        const auto command = L"\"" + (root / L"Uninstall.exe").wstring() + L"\" --uninstall";
        setRegistryString(key, L"UninstallString", command);
        setRegistryString(key, L"QuietUninstallString", command + L" --silent");
        const DWORD one = 1;
        RegSetValueExW(key, L"NoModify", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
        RegSetValueExW(key, L"NoRepair", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    } catch (...) { RegCloseKey(key); throw; }
    RegCloseKey(key);
}

void install() {
    const auto root = installDirectory();
    if (fs::exists(root) && !isManaged(root)) fail("The Aevix installation directory contains unmanaged files. Nothing was replaced.");
    if (isReparsePoint(root)) fail("Refusing to install through a directory link.");
    preflightLibraries(root);
    fs::create_directories(root / L"bin");
    fs::create_directories(root / L"lib" / L"aevix");
    fs::create_directories(root / L"share" / L"aevix");
    writeText(root / L".aevix-install", managedMarker);
    writeResource(101, root / L"bin" / L"aevixc.exe.new");
    writeResource(102, root / L"lib" / L"aevix" / L"libaevix_runtime.a.new");
    writeResource(103, root / L"share" / L"aevix" / L"LICENSE.txt");
    writeResource(104, root / L"share" / L"aevix" / L"CONTRIBUTING.txt");
    writeResource(105, root / L"share" / L"aevix" / L"logo.png");
    installLibraries(root);
    if (!MoveFileExW((root / L"bin" / L"aevixc.exe.new").c_str(), (root / L"bin" / L"aevixc.exe").c_str(), MOVEFILE_REPLACE_EXISTING))
        fail("Cannot replace aevixc.exe. Close running Aevix processes, then run setup again.");
    if (!MoveFileExW((root / L"lib" / L"aevix" / L"libaevix_runtime.a.new").c_str(), (root / L"lib" / L"aevix" / L"libaevix_runtime.a").c_str(), MOVEFILE_REPLACE_EXISTING))
        fail("Cannot replace the runtime archive.");
    const auto self = currentExecutable();
    if (normalizedPath(self.wstring()) != normalizedPath((root / L"Uninstall.exe").wstring()) &&
        !CopyFileW(self.c_str(), (root / L"Uninstall.exe").c_str(), FALSE)) fail("Cannot create the uninstaller.");
    const bool previouslyAdded = fs::exists(root / L".aevix-path-added");
    if (addPath(root / L"bin") || previouslyAdded) writeText(root / L".aevix-path-added", "1\n");
    registerUninstaller(root);
    if (!silent) MessageBoxW(nullptr,
        L"Aevix was installed and added to your user PATH.\n\nOpen a new terminal and run: aevixc --check-deps\n\nClang and a Windows C/C++ SDK are separate dependencies.\nRemove Aevix from Settings > Apps when needed.",
        L"Aevix Setup", MB_OK | MB_ICONINFORMATION);
}

void uninstallWorker(DWORD parent) {
    if (parent) {
        const auto process = OpenProcess(SYNCHRONIZE, FALSE, parent);
        if (process) { WaitForSingleObject(process, 30000); CloseHandle(process); }
    }
    const auto root = installDirectory();
    if (!isManaged(root) || isReparsePoint(root)) fail("No managed Aevix installation was found.");
    for (const auto* directory : {L"share", L"share/aevix", L"share/aevix/library", L"share/aevix/library/Math", L"share/aevix/licenses"})
        if (isReparsePoint(root / directory)) fail("Refusing to uninstall through a library directory link.");
    removeLibraries(root);
    if (fs::exists(root / L".aevix-path-added")) removePath(root / L"bin");
    // 소유한 파일 이름만 제거하고 사용자가 추가한 파일은 남겨 둔다.
    // Remove only installer-owned filenames and preserve additional user files.
    for (const auto* name : {L"bin/aevixc.exe", L"bin/aevixc.exe.new", L"lib/aevix/libaevix_runtime.a",
                             L"lib/aevix/libaevix_runtime.a.new", L"share/aevix/LICENSE.txt", L"share/aevix/CONTRIBUTING.txt",
                             L"share/aevix/logo.png", L"Uninstall.exe", L".aevix-path-added", L".aevix-install"}) {
        std::error_code error;
        fs::remove(root / name, error);
        if (error) fail("Some Aevix files are still in use. Close Aevix processes and try uninstalling again.");
    }
    for (const auto* directory : {L"bin", L"lib/aevix", L"lib", L"share/aevix/library/Math",
                                   L"share/aevix/library", L"share/aevix/licenses", L"share/aevix", L"share", L""}) {
        std::error_code ignored;
        fs::remove(root / directory, ignored);
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, registryKey);
    if (!silent) MessageBoxW(nullptr, L"Aevix was uninstalled. Open a new terminal to refresh PATH.", L"Aevix Setup", MB_OK | MB_ICONINFORMATION);
}

void startUninstallWorker() {
    wchar_t temporary[MAX_PATH + 1]{};
    wchar_t filename[MAX_PATH + 1]{};
    if (!GetTempPathW(MAX_PATH, temporary) || !GetTempFileNameW(temporary, L"avx", 0, filename))
        fail("Cannot create the temporary uninstaller.");
    if (!CopyFileW(currentExecutable().c_str(), filename, FALSE)) { DeleteFileW(filename); fail("Cannot prepare the uninstaller."); }
    std::wstring command = L"\"" + std::wstring(filename) + L"\" --uninstall-worker " + std::to_wstring(GetCurrentProcessId());
    if (silent) command += L" --silent";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(filename, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        DeleteFileW(filename); fail("Cannot launch the temporary uninstaller.");
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        int count = 0;
        auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        bool uninstall = false;
        bool worker = false;
        DWORD parent = 0;
        for (int index = 1; index < count; ++index) {
            const std::wstring argument(arguments[index]);
            if (argument == L"--silent") silent = true;
            else if (argument == L"--uninstall") uninstall = true;
            else if (argument == L"--uninstall-worker" && index + 1 < count) {
                worker = true;
                parent = static_cast<DWORD>(std::stoul(arguments[++index]));
            } else fail("Unknown setup argument.");
        }
        LocalFree(arguments);
        if (worker) { uninstallWorker(parent); return 0; }
        const auto mutex = CreateMutexW(nullptr, TRUE, L"Local\\AevixSetupPerUser");
        if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) fail("Another Aevix setup is already running.");
        if (!silent) {
            const auto prompt = uninstall ? L"Remove Aevix and its installer-added user PATH entry?" :
                L"Install Aevix for this user and add it to PATH?\n\nNo administrator access is required.";
            if (MessageBoxW(nullptr, prompt, L"Aevix Setup", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) { CloseHandle(mutex); return 1; }
        }
        if (uninstall) startUninstallWorker(); else install();
        CloseHandle(mutex);
        return 0;
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "Aevix Setup", MB_OK | MB_ICONERROR);
        return 2;
    }
}
