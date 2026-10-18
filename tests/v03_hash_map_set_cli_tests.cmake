# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_hash_map_set_cli_tests.cmake
#
# Description :
#     해시 충돌, 재해시, 소유권 보존, 대여 수명과 집합 대표값을 실제 라이브러리로 검사한다.
#     Check collisions, rehashing, ownership preservation, loan lifetimes, and set representatives through real libraries.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
function(collection_run name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 120)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(collection_accept name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    collection_run("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    collection_run("${name} AOT" 0 "" --run "${TEST_DIR}/${name}.avx")
    collection_run("${name} optimized AOT" 0 "" --run -O2 "${TEST_DIR}/${name}.avx")
    if(version MATCHES "ORC JIT enabled")
        collection_run("${name} JIT" 0 "" --jit "${TEST_DIR}/${name}.avx")
    endif()
endfunction()

collection_accept(hash_primitives [=[
import hash;
i32 main() {
    i8 low_0 = -128i8;
    i8 high_0 = 127i8;
    if (!hash.equal(&low_0, &low_0) || hash.equal(&low_0, &high_0) || hash.compare(&low_0, &high_0) != -1 || hash.value(&low_0) != hash.value(&low_0) || hash.bucket(hash.value(&high_0), 7) >= 7) return 1;
    i16 low_1 = -32768i16;
    i16 high_1 = 32767i16;
    if (!hash.equal(&low_1, &low_1) || hash.equal(&low_1, &high_1) || hash.compare(&low_1, &high_1) != -1 || hash.value(&low_1) != hash.value(&low_1) || hash.bucket(hash.value(&high_1), 7) >= 7) return 2;
    i32 low_2 = -2147483648i32;
    i32 high_2 = 2147483647i32;
    if (!hash.equal(&low_2, &low_2) || hash.equal(&low_2, &high_2) || hash.compare(&low_2, &high_2) != -1 || hash.value(&low_2) != hash.value(&low_2) || hash.bucket(hash.value(&high_2), 7) >= 7) return 3;
    i64 low_3 = -9223372036854775808i64;
    i64 high_3 = 9223372036854775807i64;
    if (!hash.equal(&low_3, &low_3) || hash.equal(&low_3, &high_3) || hash.compare(&low_3, &high_3) != -1 || hash.value(&low_3) != hash.value(&low_3) || hash.bucket(hash.value(&high_3), 7) >= 7) return 4;
    i128 low_4 = -170141183460469231731687303715884105728i128;
    i128 high_4 = 170141183460469231731687303715884105727i128;
    if (!hash.equal(&low_4, &low_4) || hash.equal(&low_4, &high_4) || hash.compare(&low_4, &high_4) != -1 || hash.value(&low_4) != hash.value(&low_4) || hash.bucket(hash.value(&high_4), 7) >= 7) return 5;
    isize low_5 = -9223372036854775808isize;
    isize high_5 = 9223372036854775807isize;
    if (!hash.equal(&low_5, &low_5) || hash.equal(&low_5, &high_5) || hash.compare(&low_5, &high_5) != -1 || hash.value(&low_5) != hash.value(&low_5) || hash.bucket(hash.value(&high_5), 7) >= 7) return 6;
    u8 low_6 = 0u8;
    u8 high_6 = 255u8;
    if (!hash.equal(&low_6, &low_6) || hash.equal(&low_6, &high_6) || hash.compare(&low_6, &high_6) != -1 || hash.value(&low_6) != hash.value(&low_6) || hash.bucket(hash.value(&high_6), 7) >= 7) return 7;
    u16 low_7 = 0u16;
    u16 high_7 = 65535u16;
    if (!hash.equal(&low_7, &low_7) || hash.equal(&low_7, &high_7) || hash.compare(&low_7, &high_7) != -1 || hash.value(&low_7) != hash.value(&low_7) || hash.bucket(hash.value(&high_7), 7) >= 7) return 8;
    u32 low_8 = 0u32;
    u32 high_8 = 4294967295u32;
    if (!hash.equal(&low_8, &low_8) || hash.equal(&low_8, &high_8) || hash.compare(&low_8, &high_8) != -1 || hash.value(&low_8) != hash.value(&low_8) || hash.bucket(hash.value(&high_8), 7) >= 7) return 9;
    u64 low_9 = 0u64;
    u64 high_9 = 18446744073709551615u64;
    if (!hash.equal(&low_9, &low_9) || hash.equal(&low_9, &high_9) || hash.compare(&low_9, &high_9) != -1 || hash.value(&low_9) != hash.value(&low_9) || hash.bucket(hash.value(&high_9), 7) >= 7) return 10;
    u128 low_10 = 0u128;
    u128 high_10 = 340282366920938463463374607431768211455u128;
    if (!hash.equal(&low_10, &low_10) || hash.equal(&low_10, &high_10) || hash.compare(&low_10, &high_10) != -1 || hash.value(&low_10) != hash.value(&low_10) || hash.bucket(hash.value(&high_10), 7) >= 7) return 11;
    usize low_11 = 0usize;
    usize high_11 = 18446744073709551615usize;
    if (!hash.equal(&low_11, &low_11) || hash.equal(&low_11, &high_11) || hash.compare(&low_11, &high_11) != -1 || hash.value(&low_11) != hash.value(&low_11) || hash.bucket(hash.value(&high_11), 7) >= 7) return 12;
    bool disabled = false;
    bool enabled = true;
    if (hash.value(&disabled) != 0 || hash.value(&enabled) != 1 || hash.equal(&disabled, &enabled) || hash.compare(&disabled, &enabled) != -1) return 20;
    byte first = Byte.from_u8(0u8);
    byte last = Byte.from_u8(255u8);
    if (!hash.equal(&first, &first) || hash.equal(&first, &last) || hash.compare(&first, &last) != -1) return 21;
    hash.value(&last);
    string_view hello = "hello";
    string_view same = "hello";
    string_view world = "world";
    if (!hash.equal(&hello, &same) || hash.value(&hello) != hash.value(&same) || hash.compare(&hello, &world) != -1) return 22;
    Result<string, TextError> text = string.from_view("hello");
    switch (move text) {
        case Result<string, TextError>.Ok { string owned = value }:
            if (hash.value(&owned) != hash.value(&hello) || !hash.equal(&owned, &owned) || hash.compare(&owned, &owned) != 0) return 23;
            break;
        case Result<string, TextError>.Err: return 24; break;
    }
    return 0;
}
]=])

collection_accept(collisions [=[
import map;
import hash;
struct Key implements hash.Hash, hash.Eq {
    public i32 number;
    public u64 hash(&Self self) { return 1; }
    public bool equal(&Self self, &Self other) { return self.number == other.number; }
};
Result<i32, CollectionError> exercise() {
    mut map.Map<Key, i32> table = map.Map<Key, i32>.new()?;
    mut i32 index = 0;
    while (index < 80) {
        Result<Optional<i32>, InputError<map.Entry<Key, i32>>> inserted = map.insert(&mut table, Key { number = index }, index * 3);
        if (Result.is_err(&inserted)) return Result.ok(1);
        index++;
    }
    if (table.len() != 80 || table.capacity() < 80) return Result.ok(2);
    index = 0;
    while (index < 80) {
        Key query = Key { number = index };
        Optional<&i32> found = map.get(&table, &query);
        switch (move found) {
            case Optional<&i32>.Some { &i32 value = value }: if (*value != index * 3) return Result.ok(3); break;
            case Optional<&i32>.None: return Result.ok(4); break;
        }
        index++;
    }
    index = 0;
    while (index < 80) {
        Key query = Key { number = index };
        Optional<i32> removed = map.remove(&mut table, &query);
        if (Optional.is_none(&removed)) return Result.ok(5);
        index += 2;
    }
    index = 1;
    while (index < 80) {
        Key query = Key { number = index };
        if (!map.contains_key(&table, &query)) return Result.ok(6);
        index += 2;
    }
    index = 80;
    while (index < 120) {
        Result<Optional<i32>, InputError<map.Entry<Key, i32>>> inserted = map.insert(&mut table, Key { number = index }, index);
        if (Result.is_err(&inserted)) return Result.ok(7);
        index++;
    }
    if (table.len() != 80) return Result.ok(8);
    table.clear();
    return Result.ok(table.is_empty() ? 0 : 9);
}
i32 main() {
    Result<i32, CollectionError> status = exercise();
    switch (move status) { case Result<i32, CollectionError>.Ok { i32 code = value }: return code; break; case Result<i32, CollectionError>.Err: return 99; break; }
}
]=])

collection_accept(text_and_borrows [=[
import map;
string make_text(string_view text) {
    Result<string, TextError> created = string.from_view(text);
    switch (move created) { case Result<string, TextError>.Ok { string owner = value }: return owner; break; case Result<string, TextError>.Err: Never.unreachable(); break; }
}
Result<i32, CollectionError> exercise() {
    mut map.Map<i32, string> table = map.Map<i32, string>.new()?;
    string first = make_text("first");
    string second = make_text("second");
    Result<Optional<string>, InputError<map.Entry<i32, string>>> inserted = map.insert(&mut table, 7, first);
    if (Result.is_err(&inserted)) return Result.ok(1);
    Result<Optional<string>, InputError<map.Entry<i32, string>>> replaced = map.insert(&mut table, 7, second);
    switch (move replaced) {
        case Result<Optional<string>, InputError<map.Entry<i32, string>>>.Ok { Optional<string> previous = value }:
            switch (move previous) {
                case Optional<string>.Some { string old = value }: if (!string.equals(&old, "first")) return Result.ok(2); break;
                case Optional<string>.None: return Result.ok(3); break;
            }
            break;
        case Result<Optional<string>, InputError<map.Entry<i32, string>>>.Err: return Result.ok(4); break;
    }
    i32 query = 7;
    Optional<&mut string> view = map.get_mut(&mut table, &query);
    switch (move view) {
        case Optional<&mut string>.Some { &mut string value = value }: string.clear(value); break;
        case Optional<&mut string>.None: return Result.ok(5); break;
    }
    Optional<string> removed = map.remove(&mut table, &query);
    switch (move removed) {
        case Optional<string>.Some { string value = value }: if (!string.is_empty(&value)) return Result.ok(6); break;
        case Optional<string>.None: return Result.ok(7); break;
    }
    return Result.ok(table.len() == 0 ? 0 : 8);
}
i32 main() {
    Result<i32, CollectionError> status = exercise();
    switch (move status) { case Result<i32, CollectionError>.Ok { i32 code = value }: return code; break; case Result<i32, CollectionError>.Err: return 99; break; }
}
]=])

collection_accept(traversal_and_snapshots [=[
import map;
Result<i32, CollectionError> exercise() {
    mut map.Map<i32, i32> table = map.Map<i32, i32>.with_seed(17)?;
    mut i32 index = 1;
    while (index <= 3) {
        Result<Optional<i32>, InputError<map.Entry<i32, i32>>> inserted = map.insert(&mut table, index, index * 10);
        if (Result.is_err(&inserted)) return Result.ok(1);
        index++;
    }
    mut i32 total = 0;
    table.for_each([&mut total](&i32 key, &i32 value) -> unit { total += *value; return Unit.value(); });
    if (total != 60) return Result.ok(2);
    table.for_each_mut([](&i32 key, &mut i32 value) -> unit { *value += *key; return Unit.value(); });
    table.retain([](&i32 key, &mut i32 value) -> bool { return *key % 2 == 1; });
    if (table.len() != 2) return Result.ok(3);
    Vector<i32> keys = table.keys_copy()?;
    Vector<i32> values = table.values_copy()?;
    Vector<map.Entry<i32, i32>> entries = table.entries_copy()?;
    if (Vector.len(&keys) != 2 || keys[0] + keys[1] != 4 || values[0] + values[1] != 44 || Vector.len(&entries) != 2) return Result.ok(4);
    Result<unit, CollectionError> excessive = table.reserve(18446744073709551615usize);
    if (Result.is_ok(&excessive) || table.len() != 2) return Result.ok(5);
    i32 query = 1;
    Optional<&i32> canonical = map.get_key(&table, &query);
    if (Optional.is_none(&canonical)) return Result.ok(6);
    Optional<map.Entry<i32, i32>> removed = map.remove_entry(&mut table, &query);
    switch (move removed) {
        case Optional<map.Entry<i32, i32>>.Some { map.Entry<i32, i32> entry = value }:
            switch (move entry) { case map.Entry<i32, i32>.Pair { i32 key = key, i32 value = value }: if (key != 1 || value != 11) return Result.ok(7); break; }
            break;
        case Optional<map.Entry<i32, i32>>.None: return Result.ok(8); break;
    }
    return Result.ok(table.len() == 1 ? 0 : 9);
}
i32 main() { Result<i32, CollectionError> status = exercise(); switch (move status) { case Result<i32, CollectionError>.Ok { i32 code = value }: return code; break; case Result<i32, CollectionError>.Err: return 99; break; } }
]=])

collection_accept(drop_owners [=[
import map;
mut i32 dropped = 0;
struct Resource implements Drop { public i32 tag; public void drop(&mut Self self) { dropped++; return; } };
Result<i32, CollectionError> exercise() {
    {
        mut map.Map<i32, Resource> table = map.Map<i32, Resource>.new()?;
        Result<Optional<Resource>, InputError<map.Entry<i32, Resource>>> first = map.insert(&mut table, 1, Resource { tag = 10 });
        if (Result.is_err(&first)) return Result.ok(1);
        Result<Optional<Resource>, InputError<map.Entry<i32, Resource>>> replaced = map.insert(&mut table, 1, Resource { tag = 20 });
        if (Result.is_err(&replaced)) return Result.ok(2);
        i32 query = 1;
        Optional<Resource> removed = map.remove(&mut table, &query);
        if (Optional.is_none(&removed)) return Result.ok(3);
        Result<Optional<Resource>, InputError<map.Entry<i32, Resource>>> second = map.insert(&mut table, 2, Resource { tag = 30 });
        if (Result.is_err(&second)) return Result.ok(4);
        table.clear();
        Result<Optional<Resource>, InputError<map.Entry<i32, Resource>>> last = map.insert(&mut table, 3, Resource { tag = 40 });
        if (Result.is_err(&last)) return Result.ok(5);
    }
    return Result.ok(dropped == 4 ? 0 : 6);
}
i32 main() { Result<i32, CollectionError> status = exercise(); switch (move status) { case Result<i32, CollectionError>.Ok { i32 code = value }: return code; break; case Result<i32, CollectionError>.Err: return 99; break; } }
]=])

collection_accept(set_members [=[
import set;
Result<i32, CollectionError> exercise() {
    mut set.Set<i32> members = set.Set<i32>.new()?;
    Result<bool, InputError<i32>> first = set.insert(&mut members, 3);
    Result<bool, InputError<i32>> second = set.insert(&mut members, 3);
    if (Result.is_err(&first) || Result.is_err(&second) || members.len() != 1) return Result.ok(1);
    i32 query = 3;
    if (!set.contains(&members, &query)) return Result.ok(2);
    Optional<&i32> canonical = set.get(&members, &query);
    switch (move canonical) { case Optional<&i32>.Some { &i32 value = value }: if (*value != 3) return Result.ok(3); break; case Optional<&i32>.None: return Result.ok(4); break; }
    Vector<i32> copied = members.values_copy()?;
    if (Vector.len(&copied) != 1 || copied[0] != 3) return Result.ok(5);
    Optional<i32> taken = set.take(&mut members, &query);
    if (Optional.is_none(&taken) || !members.is_empty()) return Result.ok(6);
    if (set.remove(&mut members, &query)) return Result.ok(7);
    members.reserve(20)?;
    members.clear();
    return Result.ok(0);
}
i32 main() {
    Result<i32, CollectionError> status = exercise();
    switch (move status) { case Result<i32, CollectionError>.Ok { i32 code = value }: return code; break; case Result<i32, CollectionError>.Err: return 99; break; }
}
]=])

file(WRITE "${TEST_DIR}/loan_conflict.avx" [=[
import map;
void invalid(&mut map.Map<i32, i32> table, &i32 query) {
    Optional<&i32> view = map.get(table, query);
    map.remove(table, query);
    switch (move view) { case Optional<&i32>.Some { &i32 value = value }: i32 retained = *value; break; case Optional<&i32>.None: break; }
    return;
}
i32 main() { return 0; }
]=])
collection_run(reject_live_view 1 "borrow|loan" --check "${TEST_DIR}/loan_conflict.avx")
message(STATUS "Hash map/set collision, ownership, and borrowing checks passed")
