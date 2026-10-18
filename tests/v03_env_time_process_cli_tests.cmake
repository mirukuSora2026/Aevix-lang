# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_env_time_process_cli_tests.cmake
#
# Description :
#     실제 환경·시간·프로세스 라이브러리의 공개 API와 자원 정리를 AOT/JIT으로 확인한다.
#     Exercise public environment, time, and process APIs and cleanup through AOT and JIT.
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
set(cmake_path "${CMAKE_COMMAND}")
set(marker_path "${TEST_DIR}/child-finished.txt")
foreach(path cmake_path marker_path)
    string(REPLACE "\\" "\\\\" ${path} "${${path}}")
    string(REPLACE "\"" "\\\"" ${path} "${${path}}")
endforeach()

function(os_command label expected)
    execute_process(COMMAND "${AEVIXC}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${result}" STREQUAL "${expected}")
        message(FATAL_ERROR "${label}: expected ${expected}, got ${result}: ${output}${error}")
    endif()
endfunction()

function(os_program name source)
    string(CONFIGURE "${source}" configured @ONLY)
    file(MAKE_DIRECTORY "${TEST_DIR}/${name}")
    set(path "${TEST_DIR}/${name}/main.avx")
    file(WRITE "${path}" "${configured}\n")
    os_command("${name} check" 0 --check "${path}")
    foreach(mode IN LISTS modes)
        foreach(optimization -O0 -O2)
            file(REMOVE "${TEST_DIR}/child-finished.txt")
            os_command("${name} ${mode} ${optimization}" 0 "${mode}" "${optimization}" "${path}")
            if(name STREQUAL "process" AND NOT EXISTS "${TEST_DIR}/child-finished.txt")
                message(FATAL_ERROR "Child Drop did not wait for its output marker")
            endif()
        endforeach()
    endforeach()
endfunction()

os_program(environment [=[
import env;
Result<i32, env.EnvError> exercise() {
    env.remove("AEVIX_LIBRARY_TEST_03")?;
    if (env.contains("AEVIX_LIBRARY_TEST_03")?) return Result.ok(1);
    Optional<string> missing = env.get("AEVIX_LIBRARY_TEST_03")?;
    if (Optional.is_some(&missing)) return Result.ok(2);
    Result<string, env.EnvError> required = env.require("AEVIX_LIBRARY_TEST_03");
    switch (move required) {
        case Result<string, env.EnvError>.Ok: return Result.ok(3); break;
        case Result<string, env.EnvError>.Err { env.EnvError cause = error }:
            switch (cause) { case env.EnvError.NotFound: break; default: return Result.ok(4); break; }
            break;
    }
    env.set("AEVIX_LIBRARY_TEST_03", "he ll o 안녕")?;
    if (!(env.contains("AEVIX_LIBRARY_TEST_03")?)) return Result.ok(5);
    string text = env.require("AEVIX_LIBRARY_TEST_03")?;
    if (!string.equals(&text, "he ll o 안녕")) return Result.ok(6);
    env.set("AEVIX_LIBRARY_TEST_03", "")?;
    Optional<string> empty = env.get("AEVIX_LIBRARY_TEST_03")?;
    switch (move empty) {
        case Optional<string>.Some { string value = value }: if (!string.is_empty(&value)) return Result.ok(7); break;
        case Optional<string>.None: return Result.ok(8); break;
    }
    env.remove("AEVIX_LIBRARY_TEST_03")?;
    Result<unit, env.EnvError> invalid = env.set("", "value");
    switch (invalid) {
        case Result<unit, env.EnvError>.Ok: return Result.ok(9); break;
        case Result<unit, env.EnvError>.Err { env.EnvError cause = error }:
            switch (cause) { case env.EnvError.InvalidName: break; default: return Result.ok(10); break; }
            break;
    }
    return Result.ok(0);
}
i32 main() {
    Result<i32, env.EnvError> outcome = exercise();
    switch (outcome) {
        case Result<i32, env.EnvError>.Ok { i32 code = value }: return code; break;
        case Result<i32, env.EnvError>.Err: return 99; break;
    }
}
]=])

os_program(clocks [=[
import time;
Result<i32, time.TimeError> exercise() {
    time.Duration zero = time.Duration.from_nanos(0);
    time.Duration micros = time.Duration.from_micros(1250)?;
    time.Duration millis = time.Duration.from_millis(1250)?;
    time.Duration seconds = time.Duration.from_secs(2)?;
    if (!zero.is_zero() || micros.as_nanos() != 1250000 || millis.as_millis() != 1250) return Result.ok(1);
    if (millis.as_secs() != 1 || millis.subsec_nanos() != 250000000 || seconds.as_micros() != 2000000) return Result.ok(2);
    time.Duration sum = seconds.checked_add(millis)?;
    time.Duration difference = sum.checked_sub(millis)?;
    time.Duration multiplied = difference.checked_mul(3)?;
    time.Duration divided = multiplied.checked_div(2)?;
    if (divided.as_nanos() != 3000000000) return Result.ok(3);
    Result<time.Duration, time.TimeError> overflow = time.Duration.from_secs(18446744074u64);
    if (Result.is_ok(&overflow)) return Result.ok(4);
    Result<time.Duration, time.TimeError> underflow = zero.checked_sub(micros);
    if (Result.is_ok(&underflow)) return Result.ok(5);
    Result<time.Duration, time.TimeError> divide_zero = seconds.checked_div(0);
    if (Result.is_ok(&divide_zero)) return Result.ok(6);
    time.Instant before = time.Instant.now()?;
    u64 start = time.monotonic_ns()?;
    time.sleep_ns(0)?;
    time.sleep(time.Duration.from_millis(1)?)?;
    u64 finish = time.monotonic_ns()?;
    time.Instant after = time.Instant.now()?;
    time.Duration elapsed = before.elapsed()?;
    time.Duration interval = after.duration_since(&before)?;
    if (finish < start || interval.is_zero() || elapsed.is_zero()) return Result.ok(7);
    time.SystemTime wall = time.SystemTime.now()?;
    if (wall.unix_nanos() <= 0 || (time.unix_time_ns()?) <= 0) return Result.ok(8);
    time.SystemTime epoch = time.SystemTime.from_unix_nanos(-1);
    return Result.ok(epoch.unix_nanos() == -1 ? 0 : 9);
}
i32 main() {
    Result<i32, time.TimeError> outcome = exercise();
    switch (outcome) {
        case Result<i32, time.TimeError>.Ok { i32 code = value }: return code; break;
        case Result<i32, time.TimeError>.Err: return 99; break;
    }
}
]=])

os_program(process [=[
import process;
Result<i32, process.ProcessError> exercise() {
    string_view okay[3] = ["@cmake_path@", "-E", "true"];
    mut process.Child child = process.Child.spawn(Slice.from_array(&okay))?;
    if (child.is_joined()) return Result.ok(1);
    process.ExitStatus exited = child.wait()?;
    if (!child.is_joined() || !exited.success() || exited.signaled() || exited.code != 0) return Result.ok(2);
    Result<process.ExitStatus, process.ProcessError> repeated = child.wait();
    switch (repeated) {
        case Result<process.ExitStatus, process.ProcessError>.Ok: return Result.ok(3); break;
        case Result<process.ExitStatus, process.ProcessError>.Err { process.ProcessError cause = error }:
            switch (cause) { case process.ProcessError.AlreadyReaped: break; default: return Result.ok(4); break; }
            break;
    }
    string_view failure[3] = ["@cmake_path@", "-E", "false"];
    process.ExitStatus failed = process.run(Slice.from_array(&failure))?;
    if (failed.success() || failed.signaled() || failed.code != 1) return Result.ok(5);
    string_view empty[0] = [];
    Result<process.Child, process.ProcessError> invalid = process.Child.spawn(Slice.from_array(&empty));
    switch (move invalid) {
        case Result<process.Child, process.ProcessError>.Ok: return Result.ok(6); break;
        case Result<process.Child, process.ProcessError>.Err { process.ProcessError cause = error }:
            switch (cause) { case process.ProcessError.InvalidArgument: break; default: return Result.ok(7); break; }
            break;
    }
    {
        string_view marker[4] = ["@cmake_path@", "-E", "touch", "@marker_path@"];
        process.Child cleanup = process.Child.spawn(Slice.from_array(&marker))?;
    }
    return Result.ok(0);
}
i32 main() {
    Result<i32, process.ProcessError> outcome = exercise();
    switch (outcome) {
        case Result<i32, process.ProcessError>.Ok { i32 code = value }: return code; break;
        case Result<i32, process.ProcessError>.Err: return 99; break;
    }
}
]=])
os_program(process_polling [=[
import process;
import time;
T success<T, E>(Result<T, E> value) {
    switch (move value) {
        case Result<T, E>.Ok { T item = value }: return item; break;
        case Result<T, E>.Err: Never.unreachable(); break;
    }
}
i32 main() {
    string_view waiting[4] = ["@cmake_path@", "-E", "sleep", "5"];
    mut process.Child running = success(process.Child.spawn(Slice.from_array(&waiting)));
    Optional<process.ExitStatus> polled = success(running.try_wait());
    if (Optional.is_some(&polled) || running.is_joined()) return 1;
    Optional<process.ExitStatus> immediate = success(running.wait_timeout(time.Duration.from_nanos(0)));
    if (Optional.is_some(&immediate) || running.is_joined()) return 2;
    Optional<process.ExitStatus> timed = success(running.wait_timeout(time.Duration.from_nanos(10000000)));
    if (Optional.is_some(&timed) || running.is_joined()) return 3;
    success(running.kill());
    process.ExitStatus killed = success(running.wait());
    if (killed.success() || !running.is_joined()) return 4;
    Result<Optional<process.ExitStatus>, process.ProcessError> repeated = running.try_wait();
    if (!Result.is_err(&repeated)) return 5;
    Result<unit, process.ProcessError> kill_after_wait = running.kill();
    if (!Result.is_err(&kill_after_wait)) return 6;
    Result<Optional<process.ExitStatus>, process.WaitError> timeout_after_wait = running.wait_timeout(time.Duration.from_nanos(0));
    switch (timeout_after_wait) {
        case Result<Optional<process.ExitStatus>, process.WaitError>.Ok: return 7; break;
        case Result<Optional<process.ExitStatus>, process.WaitError>.Err { &process.WaitError cause = error }:
            if (!string.equals(cause.code(), "process.already_reaped")) return 8;
            break;
    }
    string_view okay[3] = ["@cmake_path@", "-E", "true"];
    mut process.Child completed = success(process.Child.spawn(Slice.from_array(&okay)));
    Optional<process.ExitStatus> finished = success(completed.wait_timeout(time.Duration.from_nanos(5000000000)));
    switch (finished) {
        case Optional<process.ExitStatus>.Some { process.ExitStatus status = value }:
            if (!status.success() || !completed.is_joined()) return 9;
            break;
        case Optional<process.ExitStatus>.None: return 10; break;
    }
    return 0;
}
]=])
message(STATUS "Environment, clocks, durations and child-process APIs passed AOT/JIT execution")
