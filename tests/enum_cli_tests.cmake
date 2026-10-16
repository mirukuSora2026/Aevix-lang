# ========================
#
# Created By : miruku_sora2026
#
# File name : enum_cli_tests.cmake
#
# Description :
#     enum 페이로드, 완전 분기, 대여 모드와 이동 이후 접근을 검사한다.
#     Check enum payloads, exhaustive branches, borrowing modes, and access after moves.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
set(prefix "enum Event { Empty; Number { public i32 value; }; }; ")
function(check_enum name expected pattern source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    execute_process(COMMAND "${AEVIXC}" --check "${TEST_DIR}/${name}.avx"
        RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
    if(NOT "${code}" STREQUAL "${expected}" OR NOT "${output}${error}" MATCHES "${pattern}")
        message(FATAL_ERROR "${name}: code=${code}, expected=${expected}\n${output}\n${error}")
    endif()
endfunction()
check_enum(payload_type 1 "Enum payload" "${prefix} i32 main() { Event event = Event.Number { value = true }; return 0; }")
check_enum(missing_payload 1 "Missing enum payload" "${prefix} i32 main() { Event event = Event.Number {}; return 0; }")
check_enum(bare_data_variant 1 "requires a named payload" "${prefix} i32 main() { Event event = Event.Number; return 0; }")
check_enum(missing_case 1 "cover every variant" "${prefix} i32 main() { Event event = Event.Empty; switch (event) { case Event.Empty: return 0; break; } }")
check_enum(duplicate_case 1 "Duplicate enum case" "${prefix} i32 main() { Event event = Event.Empty; switch (event) { case Event.Empty: break; case Event.Empty: break; default: break; } return 0; }")
check_enum(wrong_enum 1 "variant of the selected enum" "${prefix} enum Other { Item; }; i32 main() { Event event = Event.Empty; switch (event) { case Other.Item: break; default: break; } return 0; }")
check_enum(payload_default_readonly 1 "requires switch" "${prefix} i32 main() { mut Event event = Event.Number { value = 1 }; switch (event) { case Event.Number { &mut i32 loan = value }: *loan = 2; break; default: break; } return 0; }")
check_enum(payload_mutable 0 "ownership checked" "${prefix} i32 main() { mut Event event = Event.Number { value = 1 }; switch (&mut event) { case Event.Number { &mut i32 loan = value }: *loan = 2; break; default: break; } return 0; }")
check_enum(noncopy_read 1 "non-Copy payload" "struct Item { public i32 value; }; enum Wrapped { Some { public Item item; }; None; }; i32 main() { Wrapped wrapped = Wrapped.Some { item = Item { value = 1 } }; switch (wrapped) { case Wrapped.Some { Item item = item }: break; default: break; } return 0; }")
check_enum(noncopy_borrow 0 "ownership checked" "struct Item { public i32 value; }; enum Wrapped { Some { public Item item; }; None; }; i32 main() { Wrapped wrapped = Wrapped.Some { item = Item { value = 1 } }; switch (wrapped) { case Wrapped.Some { &Item view = item }: i32 value = view.value; break; default: break; } return 0; }")
check_enum(used_after_consume 1 "[Mm]ov" "${prefix} i32 main() { Event event = Event.Number { value = 1 }; switch (move event) { case Event.Number { i32 value = value }: break; default: break; } switch (event) { case Event.Empty: break; default: break; } return 0; }")
check_enum(copy_payload_required 1 "Copy" "struct Item { public i32 value; }; enum Wrapped implements Copy { Some { public Item item; }; None; }; i32 main() { return 0; }")
check_enum(copy_value_keeps_copy_semantics 0 "ownership checked" "enum Flag implements Copy { On; Off; }; i32 main() { Flag flag = Flag.On; switch (move flag) { case Flag.On: break; case Flag.Off: break; } switch (flag) { case Flag.On: return 1; break; case Flag.Off: return 0; break; } }")
message(STATUS "Enum CLI checks=13 passed")
