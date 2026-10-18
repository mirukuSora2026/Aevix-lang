# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_path_fs_cli_tests.cmake
#
# Description :
#     UTF-8 경로의 어휘 처리와 실제 파일시스템 조회·오류·심볼릭 링크를 검사한다.
#     Test UTF-8 lexical paths and real filesystem queries, errors, and symbolic links.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
set(source_dir "${TEST_DIR}/sources")
file(MAKE_DIRECTORY "${source_dir}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)

function(check_path_fs label expected)
    execute_process(COMMAND "${AEVIXC}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${result}" STREQUAL "${expected}")
        message(FATAL_ERROR "${label}: expected ${expected}, got ${result}: ${output}${error}")
    endif()
endfunction()

set(lexical "${source_dir}/lexical.avx")
file(WRITE "${lexical}" [=[
import path;

Result<i32, path.PathError> exercise() {
    if (!string.equals(path.basename("a/b/")?, "b")) return Result.ok(1);
    if (!string.equals(path.dirname("/a/b/")?, "/a")) return Result.ok(2);
    if (!string.equals(path.dirname("file")?, "")) return Result.ok(3);
    if (!string.equals(path.basename("/")?, "/")) return Result.ok(4);
    if (!string.equals(path.dirname("/")?, "/")) return Result.ok(5);
    if (!string.equals(path.basename("")?, "")) return Result.ok(6);
    if (!string.equals(path.extension("archive.tar.gz")?, "gz")) return Result.ok(7);
    if (!string.equals(path.extension(".gitignore")?, "")) return Result.ok(8);
    if (!string.equals(path.extension("file.")?, "")) return Result.ok(9);
    if (!string.equals(path.extension("..")?, "")) return Result.ok(10);
    if (!string.equals(path.basename("지구/파일.txt")?, "파일.txt")) return Result.ok(11);
    if (!string.equals(path.extension("지구/파일.txt")?, "txt")) return Result.ok(12);
    if (!string.equals(path.basename("folder\\name.txt")?, "folder\\name.txt")) return Result.ok(13);
    if (!string.equals(path.dirname("C:\\docs\\file.txt", true)?, "C:\\docs")) return Result.ok(14);
    if (!string.equals(path.basename("C:\\docs\\file.txt", true)?, "file.txt")) return Result.ok(15);
    if (!string.equals(path.dirname("C:\\", true)?, "C:\\")) return Result.ok(16);
    if (!string.equals(path.basename("\\\\server\\share\\file.txt", true)?, "file.txt")) return Result.ok(17);
    string joined = path.join("dir", "file")?;
    if (!string.equals(&joined, "dir/file")) return Result.ok(18);
    string rooted = path.join("dir", "/file")?;
    if (!string.equals(&rooted, "/file")) return Result.ok(19);
    string no_extra = path.join("dir/", "file")?;
    if (!string.equals(&no_extra, "dir/file")) return Result.ok(20);
    string unchanged = path.join("dir", "")?;
    if (!string.equals(&unchanged, "dir")) return Result.ok(21);
    string drive = path.join("C:", "file", true)?;
    if (!string.equals(&drive, "C:file")) return Result.ok(22);
    Vector<string> pieces = path.components("/a//b/../")?;
    if (Vector.len(&pieces) != 4) return Result.ok(23);
    if (!string.equals(&pieces[0], "/") || !string.equals(&pieces[3], "..")) return Result.ok(24);
    Vector<string> empty = path.components("")?;
    if (Vector.len(&empty) != 0) return Result.ok(25);
    Vector<string> windows = path.components("C:\\a\\b", true)?;
    if (Vector.len(&windows) != 3 || !string.equals(&windows[0], "C:\\")) return Result.ok(26);
    Result<unit, path.PathError> rejected = path.validate("a\0b");
    switch (rejected) {
        case Result<unit, path.PathError>.Ok: return Result.ok(27); break;
        case Result<unit, path.PathError>.Err { path.PathError problem = error }:
            switch (problem) {
                case path.PathError.EmbeddedNull: break;
                default: return Result.ok(28); break;
            }
            break;
    }
    return Result.ok(0);
}
i32 main() {
    Result<i32, path.PathError> result = exercise();
    switch (result) {
        case Result<i32, path.PathError>.Ok { i32 code = value }: return code; break;
        case Result<i32, path.PathError>.Err: return 90; break;
    }
}
]=])

set(normalize "${source_dir}/normalize.avx")
if(WIN32)
    set(normalized "b\\d")
else()
    set(normalized "b/d")
endif()
set(normalize_source [=[
import path;
Result<i32, path.PathError> exercise() {
    string result = path.normalize("a/../b/./c/../d")?;
    if (!string.equals(&result, "@normalized@")) return Result.ok(1);
    string empty = path.normalize("")?;
    if (!string.equals(&empty, ".")) return Result.ok(2);
    string relative = path.normalize("../../a")?;
    if (string.is_empty(&relative)) return Result.ok(3);
    return Result.ok(0);
}
i32 main() {
    Result<i32, path.PathError> result = exercise();
    switch (result) {
        case Result<i32, path.PathError>.Ok { i32 code = value }: return code; break;
        case Result<i32, path.PathError>.Err: return 90; break;
    }
}
]=])
string(CONFIGURE "${normalize_source}" normalize_source @ONLY)
file(WRITE "${normalize}" "${normalize_source}")

set(work "${TEST_DIR}/sandbox_path_fs")
file(TO_CMAKE_PATH "${work}" work)
set(test_symlinks "false")
if(UNIX)
    set(test_symlinks "true")
endif()
set(filesystem_source [=[
import fs;
Result<i32, fs.FsError> exercise() {
    fs.Metadata info = fs.metadata("@work@/file_파일.txt")?;
    if (!info.is_file() || info.is_dir() || info.is_other() || info.kind != 1 || info.byte_length != 4 || info.modified_nanoseconds <= 0) return Result.ok(1);
    if (@test_symlinks@ && info.permissions != 384) return Result.ok(19);
    fs.Metadata duplicate = info;
    if (duplicate.byte_length != 4) return Result.ok(2);
    if (!fs.exists("@work@/file_파일.txt")? || !fs.is_file("@work@/file_파일.txt")?) return Result.ok(3);
    if (!fs.is_dir("@work@")? || fs.is_file("@work@")?) return Result.ok(4);
    if (fs.exists("@work@/missing")? || fs.is_dir("@work@/missing")?) return Result.ok(5);
    fs.create_dir("@work@/created")?;
    if (!fs.is_dir("@work@/created")?) return Result.ok(6);
    Result<unit, fs.FsError> duplicate_dir = fs.create_dir("@work@/created");
    switch (duplicate_dir) {
        case Result<unit, fs.FsError>.Ok: return Result.ok(7); break;
        case Result<unit, fs.FsError>.Err { fs.FsError problem = error }:
            if (!string.equals(problem.code(), "file.already_exists")) return Result.ok(8);
            break;
    }
    fs.remove("@work@/created")?;
    if (fs.exists("@work@/created")?) return Result.ok(9);
    fs.remove("@work@/delete.bin")?;
    if (fs.exists("@work@/delete.bin")?) return Result.ok(10);
    Result<unit, fs.FsError> nonempty = fs.remove("@work@/nonempty");
    switch (nonempty) {
        case Result<unit, fs.FsError>.Ok: return Result.ok(11); break;
        case Result<unit, fs.FsError>.Err: break;
    }
    Result<bool, fs.FsError> invalid = fs.exists("a\0b");
    switch (invalid) {
        case Result<bool, fs.FsError>.Ok: return Result.ok(12); break;
        case Result<bool, fs.FsError>.Err { fs.FsError problem = error }:
            if (!string.equals(problem.code(), "path.embedded_null") || string.is_empty(problem.message())) return Result.ok(13);
            break;
    }
    Result<fs.Metadata, fs.FsError> absent = fs.metadata("@work@/missing");
    switch (absent) {
        case Result<fs.Metadata, fs.FsError>.Ok: return Result.ok(14); break;
        case Result<fs.Metadata, fs.FsError>.Err { fs.FsError problem = error }:
            if (!string.equals(problem.code(), "file.not_found")) return Result.ok(15);
            break;
    }
    if (@test_symlinks@) {
        fs.Metadata link = fs.symlink_metadata("@work@/link.bin")?;
        fs.Metadata target = fs.metadata("@work@/link.bin")?;
        if (!link.is_symlink() || !target.is_file() || !fs.is_symlink("@work@/link.bin")?) return Result.ok(16);
        if (fs.exists("@work@/dangling.bin")? || !fs.is_symlink("@work@/dangling.bin")?) return Result.ok(17);
        fs.remove("@work@/link.bin")?;
        if (!fs.exists("@work@/file_파일.txt")?) return Result.ok(18);
    }
    return Result.ok(0);
}
i32 main() {
    Result<i32, fs.FsError> result = exercise();
    switch (result) {
        case Result<i32, fs.FsError>.Ok { i32 code = value }: return code; break;
        case Result<i32, fs.FsError>.Err: return 90; break;
    }
}
]=])
string(CONFIGURE "${filesystem_source}" filesystem_source @ONLY)
set(filesystem "${source_dir}/filesystem.avx")
file(WRITE "${filesystem}" "${filesystem_source}")

foreach(source "${lexical}" "${normalize}" "${filesystem}")
    check_path_fs("check ${source}" 0 --check "${source}")
endforeach()

set(modes aot aot_o2)
if(version MATCHES "ORC JIT enabled")
    list(APPEND modes jit jit_o2)
endif()
foreach(mode IN LISTS modes)
    if(mode MATCHES "^jit")
        set(arguments --jit)
    else()
        set(arguments --run)
    endif()
    if(mode MATCHES "_o2$")
        list(APPEND arguments -O2)
    endif()
    file(REMOVE_RECURSE "${work}")
    file(MAKE_DIRECTORY "${work}/nonempty")
    file(WRITE "${work}/file_파일.txt" "abc\n")
    if(UNIX)
        file(CHMOD "${work}/file_파일.txt" PERMISSIONS OWNER_READ OWNER_WRITE)
    endif()
    file(WRITE "${work}/delete.bin" "delete")
    file(WRITE "${work}/nonempty/keep.bin" "keep")
    if(UNIX)
        file(CREATE_LINK "${work}/file_파일.txt" "${work}/link.bin" SYMBOLIC RESULT link_result)
        file(CREATE_LINK "${work}/missing" "${work}/dangling.bin" SYMBOLIC RESULT dangling_result)
        if(NOT "${link_result}" STREQUAL "0" OR NOT "${dangling_result}" STREQUAL "0")
            message(FATAL_ERROR "Cannot create symbolic-link test fixtures: ${link_result}; ${dangling_result}")
        endif()
    endif()
    check_path_fs("lexical ${mode}" 0 ${arguments} "${lexical}")
    check_path_fs("normalize ${mode}" 0 ${arguments} "${normalize}")
    check_path_fs("filesystem ${mode}" 0 ${arguments} "${filesystem}")
endforeach()

function(reject_path_fs name source pattern)
    set(path "${source_dir}/${name}.avx")
    file(WRITE "${path}" "${source}\n")
    execute_process(COMMAND "${AEVIXC}" --check "${path}" RESULT_VARIABLE result
        OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
    if(NOT "${result}" STREQUAL "1" OR NOT "${output}${error}" MATCHES "${pattern}")
        message(FATAL_ERROR "${name}: expected a source boundary rejection, got ${result}: ${output}${error}")
    endif()
endfunction()
reject_path_fs(private_metadata [=[
import fs;
i32 main() { fs.__path_metadata("missing", 1); return 0; }
]=] "Cannot access private declaration")
reject_path_fs(private_normalize [=[
import path;
i32 main() { path.__path_normalize("a/../b"); return 0; }
]=] "Cannot access private declaration")
reject_path_fs(borrowed_component_escape [=[
import path;
Result<string_view, path.PathError> escape() {
    string owned = path.join("a", "file")?;
    return path.basename(string.view(&owned));
}
i32 main() { return 0; }
]=] "Cannot return a reference to a local value")

message(STATUS "Path lexical/owned component and UTF-8 filesystem contracts passed")
