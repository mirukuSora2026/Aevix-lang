# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_io_cli_tests.cmake
#
# Description :
#     실제 File API의 바이트·오류·소유권·비공개 연결층을 AOT와 JIT에서 검증한다.
#     Verify real File API bytes, errors, ownership, and private bridges through AOT and JIT.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
set(modes --run)
if(version MATCHES "ORC JIT enabled")
    list(APPEND modes --jit)
endif()

set(helpers [=[
import io;
import fs;
bool fs_io_code(&fs.FsError problem, string_view expected) {
    switch (*problem) {
        case fs.FsError.File { fs.FileError file_error = error }:
            switch (file_error) {
                case fs.FileError.Io { fs.IoError cause = error }: return string.equals(cause.code(), expected); break;
                default: return false; break;
            }
            break;
        default: return false; break;
    }
}
io.File take_file(Result<io.File, io.FileError> outcome) {
    switch (move outcome) {
        case Result<io.File, io.FileError>.Ok { io.File owner = value }: return owner; break;
        case Result<io.File, io.FileError>.Err: Never.unreachable(); break;
    }
}
usize count(Result<usize, io.IoError> outcome) {
    switch (outcome) {
        case Result<usize, io.IoError>.Ok { usize bytes = value }: return bytes; break;
        case Result<usize, io.IoError>.Err: Never.unreachable(); break;
    }
}
void done(Result<unit, io.IoError> outcome) {
    switch (outcome) {
        case Result<unit, io.IoError>.Ok: return; break;
        case Result<unit, io.IoError>.Err: Never.unreachable(); break;
    }
}
bool closed(Result<usize, io.IoError> outcome) {
    switch (outcome) {
        case Result<usize, io.IoError>.Ok: return false; break;
        case Result<usize, io.IoError>.Err { io.IoError cause = error }:
            switch (cause) { case io.IoError.Closed: return true; break; default: return false; break; }
            break;
    }
}
bool read_failed(Result<unit, io.IoError> outcome, usize expected) {
    switch (outcome) {
        case Result<unit, io.IoError>.Ok: return false; break;
        case Result<unit, io.IoError>.Err { io.IoError cause = error }:
            switch (cause) {
                case io.IoError.ReadFailed { i32 native = os_code, usize progress = progress }:
                    return native == 0 && progress == expected;
                    break;
                default: return false; break;
            }
            break;
    }
}
]=])

function(io_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR
        (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()

function(io_accept name body expected_hex)
    set(data_path "${TEST_DIR}/${name}.data")
    set(missing_path "${TEST_DIR}/${name}.missing")
    string(REPLACE "\\" "\\\\" file_path "${data_path}")
    string(REPLACE "\"" "\\\"" file_path "${file_path}")
    string(REPLACE "\\" "\\\\" absent_path "${missing_path}")
    string(REPLACE "\"" "\\\"" absent_path "${absent_path}")
    string(CONFIGURE "${body}" configured @ONLY)
    set(source "${TEST_DIR}/${name}.avx")
    file(WRITE "${source}" "${helpers}\n${configured}\n")
    io_run("${name} check" 0 "" --check "${source}")
    foreach(mode IN LISTS modes)
        foreach(optimization -O0 -O2)
            # 파일 생성 정책이 실행 모드에 영향을 받지 않도록 매번 독립 상태에서 시작한다.
            # Start with independent filesystem state so creation policy is identical in every mode.
            file(REMOVE "${data_path}" "${missing_path}")
            io_run("${name} ${mode} ${optimization}" 0 "" "${mode}" "${optimization}" "${source}")
            if(NOT "${expected_hex}" STREQUAL "skip")
                if(NOT EXISTS "${data_path}")
                    message(FATAL_ERROR "${name}: expected output file is missing")
                endif()
                file(READ "${data_path}" contents HEX)
                if(NOT "${contents}" STREQUAL "${expected_hex}")
                    message(FATAL_ERROR "${name}: expected bytes ${expected_hex}, got ${contents}")
                endif()
            endif()
        endforeach()
    endforeach()
endfunction()

io_accept(filesystem_contents [=[
i32 main() {
    Result<unit, fs.FsError> written = fs.write_text("@file_path@", "he ll o · 한글\n");
    if (!Result.is_ok(&written)) return 1;
    Result<unit, fs.FsError> appended = fs.append_text("@file_path@", "second");
    if (!Result.is_ok(&appended)) return 2;
    Result<string, fs.FsError> text = fs.read_text("@file_path@");
    switch (move text) {
        case Result<string, fs.FsError>.Ok { string output = value }:
            if (!string.equals(&output, "he ll o · 한글\nsecond")) return 3;
            break;
        case Result<string, fs.FsError>.Err: return 4; break;
    }
    Result<Vector<byte>, fs.FsError> bytes = fs.read_bytes("@file_path@");
    switch (move bytes) {
        case Result<Vector<byte>, fs.FsError>.Ok { Vector<byte> output = value }:
            if (Vector.len(&output) != 24 || Byte.to_u8(output[0]) != 104u8) return 5;
            break;
        case Result<Vector<byte>, fs.FsError>.Err: return 6; break;
    }
    Result<string, fs.FsError> limited = fs.read_text("@file_path@", 0);
    switch (move limited) {
        case Result<string, fs.FsError>.Ok: return 7; break;
        case Result<string, fs.FsError>.Err { fs.FsError cause = error }:
            if (!fs_io_code(&cause, "io.input_too_long")) return 8;
            break;
    }
    Result<string, fs.FsError> missing = fs.read_text("@absent_path@");
    if (!Result.is_err(&missing)) return 9;
    Result<string, fs.FsError> nul = fs.read_text("bad\0path");
    switch (move nul) {
        case Result<string, fs.FsError>.Ok: return 10; break;
        case Result<string, fs.FsError>.Err { fs.FsError cause = error }:
            if (!string.equals(cause.code(), "path.embedded_null")) return 11;
            break;
    }
    byte raw[2] = [Byte.from_u8(0u8), Byte.from_u8(255u8)];
    Result<unit, fs.FsError> replaced = fs.write_bytes("@file_path@", Slice.from_array(&raw));
    if (!Result.is_ok(&replaced)) return 12;
    Result<string, fs.FsError> invalid = fs.read_text("@file_path@");
    switch (move invalid) {
        case Result<string, fs.FsError>.Ok: return 13; break;
        case Result<string, fs.FsError>.Err { fs.FsError cause = error }:
            if (!fs_io_code(&cause, "io.invalid_utf8")) return 14;
            break;
    }
    return 0;
}
]=] "00ff")

io_accept(read_remaining [=[
i32 main() {
    {
        mut io.File writer = take_file(io.File.create("@file_path@"));
        byte data[8193] = [Byte.from_u8(65u8); 8193];
        done(writer.write_all(Slice.from_array(&data)));
        done(writer.close());
    }
    mut io.File reader = take_file(io.File.open_read_write("@file_path@"));
    Result<Vector<byte>, io.IoError> bytes = reader.read_to_end(8193);
    switch (move bytes) {
        case Result<Vector<byte>, io.IoError>.Ok { Vector<byte> output = value }:
            if (Vector.len(&output) != 8193 || Byte.to_u8(output[0]) != 65u8 || Byte.to_u8(output[8192]) != 65u8) return 1;
            break;
        case Result<Vector<byte>, io.IoError>.Err: return 2; break;
    }
    if (count(reader.position()) != 8193) return 3;
    Result<Vector<byte>, io.IoError> empty = reader.read_to_end(0);
    switch (move empty) {
        case Result<Vector<byte>, io.IoError>.Ok { Vector<byte> output = value }: if (Vector.len(&output) != 0) return 4; break;
        case Result<Vector<byte>, io.IoError>.Err: return 5; break;
    }
    count(reader.seek(0));
    Result<string, io.IoError> text = reader.read_to_string(8193);
    switch (move text) {
        case Result<string, io.IoError>.Ok { string output = value }: if (string.byte_len(&output) != 8193) return 6; break;
        case Result<string, io.IoError>.Err: return 7; break;
    }
    count(reader.seek(0));
    Result<Vector<byte>, io.IoError> excessive = reader.read_to_end(8192);
    switch (move excessive) {
        case Result<Vector<byte>, io.IoError>.Ok: return 8; break;
        case Result<Vector<byte>, io.IoError>.Err { io.IoError cause = error }:
            switch (cause) { case io.IoError.InputTooLong: break; default: return 9; break; }
            break;
    }
    if (count(reader.position()) != 8193) return 10;
    count(reader.seek(0));
    Result<Vector<byte>, io.IoError> zero_limit = reader.read_to_end(0);
    if (!Result.is_err(&zero_limit) || count(reader.position()) != 1) return 11;
    done(reader.set_len(0));
    count(reader.seek(0));
    done(reader.write_text("he ll o · 한글\n"));
    count(reader.seek(0));
    Result<string, io.IoError> unicode = reader.read_to_string();
    switch (move unicode) {
        case Result<string, io.IoError>.Ok { string output = value }: if (!string.equals(&output, "he ll o · 한글\n")) return 12; break;
        case Result<string, io.IoError>.Err: return 13; break;
    }
    done(reader.set_len(0));
    count(reader.seek(0));
    byte invalid[1] = [Byte.from_u8(255u8)];
    done(reader.write_all(Slice.from_array(&invalid)));
    count(reader.seek(0));
    Result<string, io.IoError> invalid_text = reader.read_to_string();
    switch (move invalid_text) {
        case Result<string, io.IoError>.Ok: return 14; break;
        case Result<string, io.IoError>.Err { io.IoError cause = error }:
            switch (cause) { case io.IoError.InvalidUtf8: break; default: return 15; break; }
            break;
    }
    done(reader.close());
    Result<Vector<byte>, io.IoError> after_close = reader.read_to_end();
    if (!Result.is_err(&after_close)) return 16;
    return 0;
}
]=] "ff")

io_accept(bytes [=[
i32 main() {
    mut io.File writer = take_file(io.File.create("@file_path@"));
    if (!writer.is_open()) return 1;
    byte input[4] = [Byte.from_u8(65u8), Byte.from_u8(0u8), Byte.from_u8(255u8), Byte.from_u8(66u8)];
    byte empty[0] = [];
    if (count(writer.write(Slice.from_array(&input))) != 4) return 2;
    done(writer.write_all(Slice.from_array(&empty)));
    done(writer.write_text("\n"));
    done(writer.flush());
    done(writer.sync());
    if (count(writer.length()) != 5 || count(writer.position()) != 5) return 3;
    done(writer.close());
    done(writer.close());
    if (writer.is_open() || !closed(writer.write(Slice.from_array(&input)))) return 4;

    mut io.File file = take_file(io.File.open_read_write("@file_path@"));
    mut byte output[4] = [Byte.from_u8(0u8); 4];
    done(file.read_exact(Slice.from_array_mut(&mut output)));
    for (mut usize index = 0; index < 4; ++index) if (output[index] != input[index]) return 5;
    if (count(file.seek(-1, 2)) != 4) return 6;
    mut byte tail[2] = [Byte.from_u8(99u8); 2];
    if (count(file.read(Slice.from_array_mut(&mut tail))) != 1 || Byte.to_u8(tail[0]) != 10u8 || Byte.to_u8(tail[1]) != 99u8) return 7;
    if (count(file.read(Slice.from_array_mut(&mut tail))) != 0) return 8;
    done(file.set_len(2));
    if (count(file.length()) != 2 || count(file.position()) != 5) return 9;
    done(file.set_len(7));
    if (count(file.seek(2)) != 2) return 10;
    mut byte zeros[5] = [Byte.from_u8(99u8); 5];
    done(file.read_exact(Slice.from_array_mut(&mut zeros)));
    for (byte value : zeros) if (Byte.to_u8(value) != 0u8) return 11;
    if (!read_failed(file.read_exact(Slice.from_array_mut(&mut tail)), 0)) return 12;
    if (count(file.seek(6)) != 6) return 13;
    mut byte short_read[3] = [Byte.from_u8(99u8); 3];
    if (!read_failed(file.read_exact(Slice.from_array_mut(&mut short_read)), 1)) return 14;
    if (Byte.to_u8(short_read[0]) != 0u8 || Byte.to_u8(short_read[1]) != 99u8) return 15;
    Result<usize, io.IoError> invalid_seek = file.seek(-1);
    switch (invalid_seek) { case Result<usize, io.IoError>.Ok: return 16; break; default: break; }
    if (count(file.position()) != 7) return 17;
    done(file.close());
    if (!closed(file.read(Slice.from_array_mut(&mut output))) || !closed(file.length())) return 18;
    mut byte no_output[0] = [];
    done(file.read_exact(Slice.from_array_mut(&mut no_output)));
    done(file.write_all(Slice.from_array(&empty)));
    return 0;
}
]=] "41000000000000")

io_accept(factories [=[
i32 main() {
    Result<io.File, io.FileError> missing = io.File.open_read("@absent_path@");
    switch (move missing) {
        case Result<io.File, io.FileError>.Ok: return 1; break;
        case Result<io.File, io.FileError>.Err { io.FileError cause = error }:
            switch (cause) { case io.FileError.NotFound { i32 native = os_code }: if (native == 0) return 2; break; default: return 3; break; }
            break;
    }
    mut io.File fresh = take_file(io.File.create_new("@file_path@"));
    done(fresh.write_text("A"));
    done(fresh.close());
    Result<io.File, io.FileError> existing = io.File.create_new("@file_path@");
    switch (move existing) {
        case Result<io.File, io.FileError>.Ok: return 4; break;
        case Result<io.File, io.FileError>.Err { io.FileError cause = error }:
            switch (cause) { case io.FileError.AlreadyExists { i32 native = os_code }: if (native == 0) return 5; break; default: return 6; break; }
            break;
    }
    mut io.File append = take_file(io.File.append("@file_path@"));
    if (count(append.seek(0)) != 0) return 7;
    done(append.write_text("B"));
    done(append.close());
    mut io.File reader = take_file(io.File.open_read("@file_path@"));
    mut byte contents[2] = [Byte.from_u8(0u8); 2];
    done(reader.read_exact(Slice.from_array_mut(&mut contents)));
    if (Byte.to_u8(contents[0]) != 65u8 || Byte.to_u8(contents[1]) != 66u8) return 8;
    byte input[1] = [Byte.from_u8(90u8)];
    Result<unit, io.IoError> denied = reader.write_all(Slice.from_array(&input));
    switch (denied) {
        case Result<unit, io.IoError>.Ok: return 9; break;
        case Result<unit, io.IoError>.Err { io.IoError cause = error }:
            switch (cause) { case io.IoError.WriteFailed { usize progress = progress }: if (progress != 0) return 10; break; default: return 11; break; }
            break;
    }
    if (!reader.is_open() || count(reader.length()) != 2) return 12;
    done(reader.close());
    mut io.File generic = take_file(io.File.open("@file_path@", 3));
    if (count(generic.seek(1)) != 1 || count(generic.write(Slice.from_array(&input))) != 1) return 13;
    done(generic.close());
    mut io.File created = take_file(io.File.create("@file_path@"));
    if (count(created.length()) != 0) return 14;
    done(created.write_text("Done"));
    mut io.File append_missing = take_file(io.File.append("@absent_path@"));
    done(append_missing.write_text("created by append"));
    return 0;
}
]=] "446f6e65")

io_accept(scope_and_moves [=[
struct Holder implements Drop {
    public io.File file;
    public &mut i32 drops;
    public void drop(&mut Holder self) { *self.drops += 1; return; }
};
void consume(io.File owner) {
    if (!owner.is_open()) Never.unreachable();
    return;
}
i32 main() {
    mut i32 drops = 0;
    {
        io.File original = take_file(io.File.create("@file_path@"));
        mut Holder holder = Holder { file = original, drops = &mut drops };
        done(holder.file.write_text("scope-drop"));
    }
    if (drops != 1) return 1;
    io.File reader = take_file(io.File.open_read("@file_path@"));
    io.File moved = reader;
    if (!moved.is_open() || count(moved.length()) != 10) return 2;
    consume(moved);
    return 0;
}
]=] "73636f70652d64726f70")

io_accept(ordinary_helpers [=[
i32 __file_open(i32 value) { return value + 1; }
i32 __file_size(i32 value) { return value + 2; }
unit __file_cleanup(i32 value) { return Unit.value(); }
i32 main() {
    __file_cleanup(1);
    return __file_open(6) == 7 && __file_size(8) == 10 ? 0 : 1;
}
]=] "skip")

function(io_reject name source pattern)
    set(path "${TEST_DIR}/reject_${name}.avx")
    file(WRITE "${path}" "${source}\n")
    io_run("reject ${name}" 1 "${pattern}" --check "${path}")
endfunction()
io_reject(no_import "i32 main() { io.File.open_read(\"x\"); return 0; }" "Unknown|import")
io_reject(private_token "import io; u64 expose(&io.File file) { return file.token; } i32 main() { return 0; }" "private|Private|accessible")
io_reject(forged_file "import io; i32 main() { io.File forged = io.File { token = 1u64 }; return 0; }" "private|Private|accessible")
io_reject(private_bridge "import io; i32 main() { io.__file_open(\"x\", 1); return 0; }" "private|Private|accessible|Unknown")
io_reject(private_cleanup "import io; i32 main() { io.__file_cleanup(1u64); return 0; }" "private|Private|accessible|Unknown")
io_reject(extern_spoof "extern \"C\" void aevix_os_file_cleanup(u64 token); i32 main() { aevix_os_file_cleanup(1u64); return 0; }" "reserved|compiler.managed|native bridge")
io_reject(copy_file "import io; struct Bad implements Copy { public io.File file; }; i32 main() { return 0; }" "Copy|Drop|copy")
io_reject(manual_drop "import io; void bad(&mut io.File file) { file.drop(); return; } i32 main() { return 0; }" "drop|Drop|cleanup")
io_reject(generic_drop "import io; void bad<T>(&mut T value) where T implements Drop { value.drop(); return; } void invoke(&mut io.File value) { bad(value); return; } i32 main() { return 0; }" "drop|Drop|contract")
io_reject(immutable_write "import io; void bad(&io.File file, Slice<byte> bytes) { file.write_all(bytes); return; } i32 main() { return 0; }" "mut|exclusive|write|borrow")
io_reject(moved_use [=[
import io;
void consume(io.File file) { return; }
bool bad(io.File file) { consume(file); return file.is_open(); }
i32 main() { return 0; }
]=] "moved|unavailable|ownership")
io_reject(live_alias [=[
import io;
bool bad(mut io.File file) { &io.File view = &file; file.close(); return view.is_open(); }
i32 main() { return 0; }
]=] "borrow|loan|exclusive")
message(STATUS "Public File factories, byte I/O, errors, ownership, and private bridge restrictions passed")
