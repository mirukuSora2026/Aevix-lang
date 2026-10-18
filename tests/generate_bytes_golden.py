#!/usr/bin/env python3
# ========================
#
# Created By : miruku_sora2026
#
# File name : generate_bytes_golden.py
#
# Description :
#     독립 바이트·비트·범위·UTF-8 기준값과 정수·실수 표현 검사를 생성한다.
#     Generate independent byte, bit, range, UTF-8 and integer/float representation checks.
#
# ========================

import argparse
import hashlib
import json
from pathlib import Path
import random


def byte_literal(value):
    return f"Byte.from_u8({value}u8)"


def scalar_goldens():
    """Generate exhaustive scalar tests, using independent Python per-row fingerprints.

    A row fingerprint records every output in order; it is a compact oracle, not
    a proof that distinct output sequences cannot collide. Unary outputs and
    binary strings are checked directly, without fingerprints.
    """
    prime = 4294967291

    def fingerprint(values):
        result = 0
        for value in values:
            result = (result * 257 + value) % prime
        return result

    def array(name, values):
        return f"    u64 {name}[{len(values)}] = [" + ", ".join(f"{v}u64" for v in values) + "];\n"

    source = [r'''/* Independent exhaustive Python bit-pattern goldens; row fingerprints are explicitly probabilistic checks. */
import bytes;
import ordering;
import error { AnyError };
T success<T, E>(Result<T, E> input) {
    switch (move input) {
        case Result<T, E>.Ok { T output = value }: return output; break;
        default: Never.unreachable(); break;
    }
}
T convert<T, U>(U input) { return success(Integer.convert<T>(input)); }
u64 accumulate(u64 current, byte value) { return (current * 257u64 + convert<u64>(Byte.to_u8(value))) % 4294967291u64; }
bool is_error<T>(Result<T, bytes.ByteError> input, string_view code) {
    switch (move input) {
        case Result<T, bytes.ByteError>.Err { bytes.ByteError reason = error }: return string.equals(reason.code(), code); break;
        default: return false; break;
    }
}
i32 scalar_unary() {
''']
    unary = []
    for value in range(256):
        digits = f"{value:08b}"
        fields = [value.bit_count(), 8-value.bit_count(), len(digits)-len(digits.lstrip("0")),
                  len(digits)-len(digits.lstrip("1")), len(digits)-len(digits.rstrip("0")),
                  len(digits)-len(digits.rstrip("1")), value.bit_length()]
        packed = sum(item << (4*index) for index, item in enumerate(fields))
        packed |= (value.bit_count() % 2) << 28
        packed |= int(digits[::-1], 2) << 29
        unary.append(packed)
    source.append(array("expected", unary))
    source.append(r'''    if (Byte.to_u8(bytes.zero()) != 0u8 || Byte.to_u8(bytes.ones()) != 255u8 || bytes.bit_width() != 8u32 || bytes.byte_width() != 1u32) return 1;
    mut u32 raw = 0;
    while (raw < 256u32) {
        byte value = Byte.from_u8(convert<u8>(raw));
        u64 actual = convert<u64>(bytes.count_ones(value))
            + (convert<u64>(bytes.count_zeros(value)) << 4)
            + (convert<u64>(bytes.leading_zeros(value)) << 8)
            + (convert<u64>(bytes.leading_ones(value)) << 12)
            + (convert<u64>(bytes.trailing_zeros(value)) << 16)
            + (convert<u64>(bytes.trailing_ones(value)) << 20)
            + (convert<u64>(bytes.bit_length(value)) << 24)
            + (bytes.parity_odd(value) ? 268435456u64 : 0u64)
            + (convert<u64>(Byte.to_u8(bytes.reverse_bits(value))) << 29);
        if (actual != expected[convert<usize>(raw)]) return 2;
        if (bytes.is_zero(value) != (raw == 0u32) || bytes.is_all_ones(value) != (raw == 255u32)) return 3;
        if (Byte.to_u8(bytes.high_nibble(value)) != convert<u8>(raw / 16u32) || Byte.to_u8(bytes.low_nibble(value)) != convert<u8>(raw % 16u32)) return 4;
        if (Byte.to_u8(bytes.bit_not(value)) != convert<u8>(255u32 - raw)) return 5;
        raw++;
    }
    return 0;
}
i32 scalar_pairs() {
''')
    pair_ops = {"and": lambda a,b:a&b, "or":lambda a,b:a|b, "xor":lambda a,b:a^b, "and_not":lambda a,b:a&(~b&255)}
    for name, operation in pair_ops.items():
        source.append(array("expected_"+name, [fingerprint(operation(a,b) for b in range(256)) for a in range(256)]))
    source.append(r'''    mut u32 first = 0;
    while (first < 256u32) {
        byte left = Byte.from_u8(convert<u8>(first));
        mut u64 hash_and = 0;
        mut u64 hash_or = 0;
        mut u64 hash_xor = 0;
        mut u64 hash_and_not = 0;
        mut u32 second = 0;
        while (second < 256u32) {
            byte right = Byte.from_u8(convert<u8>(second));
            hash_and = accumulate(hash_and, bytes.bit_and(left, right));
            hash_or = accumulate(hash_or, bytes.bit_or(left, right));
            hash_xor = accumulate(hash_xor, bytes.bit_xor(left, right));
            hash_and_not = accumulate(hash_and_not, bytes.bit_and_not(left, right));
            if (bytes.equal(left, right) != (first == second)) return 1;
            i32 expected_order = first < second ? -1 : (first > second ? 1 : 0);
            if (ordering.to_i32(bytes.compare_bits(left, right)) != expected_order) return 2;
            second++;
        }
        usize row = convert<usize>(first);
        if (hash_and != expected_and[row] || hash_or != expected_or[row] || hash_xor != expected_xor[row] || hash_and_not != expected_and_not[row]) return 3;
        first++;
    }
    return 0;
}
i32 scalar_positions() {
''')
    position_ops = {
        "left":lambda a,n:(a<<n)&255, "right":lambda a,n:a>>n,
        "rotate_left":lambda a,n:((a<<n)|(a>>(8-n)))&255,
        "rotate_right":lambda a,n:((a>>n)|(a<<(8-n)))&255,
        "set":lambda a,n:a|(1<<n), "clear":lambda a,n:a&~(1<<n), "toggle":lambda a,n:a^(1<<n)}
    for name, operation in position_ops.items():
        source.append(array("expected_"+name, [fingerprint(operation(a,n) for n in range(8)) for a in range(256)]))
    source.append(r'''    mut u32 raw = 0;
    while (raw < 256u32) {
        byte value = Byte.from_u8(convert<u8>(raw));
        mut u64 left = 0; mut u64 right = 0; mut u64 rotate_left = 0; mut u64 rotate_right = 0;
        mut u64 set = 0; mut u64 clear = 0; mut u64 toggle = 0;
        mut u32 position = 0;
        while (position < 8u32) {
            left = accumulate(left, success(bytes.shift_left(value, position)));
            right = accumulate(right, success(bytes.shift_right(value, position)));
            rotate_left = accumulate(rotate_left, bytes.rotate_left(value, position));
            rotate_right = accumulate(rotate_right, bytes.rotate_right(value, position));
            byte set_value = success(bytes.set_bit(value, position));
            byte clear_value = success(bytes.clear_bit(value, position));
            set = accumulate(set, set_value); clear = accumulate(clear, clear_value);
            toggle = accumulate(toggle, success(bytes.toggle_bit(value, position)));
            if (!bytes.equal(success(bytes.with_bit(value, position, true)), set_value) || !bytes.equal(success(bytes.with_bit(value, position, false)), clear_value)) return 1;
            if (success(bytes.test_bit(value, position)) != ((raw / (1u32 << position)) % 2u32 != 0u32)) return 2;
            position++;
        }
        usize row = convert<usize>(raw);
        if (left != expected_left[row] || right != expected_right[row] || rotate_left != expected_rotate_left[row] || rotate_right != expected_rotate_right[row]) return 3;
        if (set != expected_set[row] || clear != expected_clear[row] || toggle != expected_toggle[row]) return 4;
        if (!bytes.equal(bytes.rotate_left(value, 4294967295u32), bytes.rotate_left(value, 7u32)) || !bytes.equal(bytes.rotate_right(value, 4294967295u32), bytes.rotate_right(value, 7u32))) return 5;
        raw++;
    }
    return 0;
}
i32 scalar_fields() {
''')
    ranges = [(start,width) for start in range(9) for width in range(9-start)]
    extracted = [fingerprint((value>>start)&((1<<width)-1) for start,width in ranges) for value in range(256)]
    deposited = [fingerprint((value & (~(((1<<width)-1)<<start)&255)) | (field<<start)
                            for start,width in ranges for field in range(1<<width)) for value in range(256)]
    source.append(array("expected_extract", extracted))
    source.append(array("expected_deposit", deposited))
    source.append(r'''    mut u32 raw = 0;
    while (raw < 256u32) {
        byte value = Byte.from_u8(convert<u8>(raw));
        mut u64 extracted = 0; mut u64 deposited = 0;
        mut u32 start = 0;
        while (start <= 8u32) {
            mut u32 width = 0;
            while (width <= 8u32 - start) {
                extracted = accumulate(extracted, success(bytes.extract_bits(value, start, width)));
                mut u32 field = 0;
                while (field < (1u32 << width)) {
                    deposited = accumulate(deposited, success(bytes.deposit_bits(value, start, width, Byte.from_u8(convert<u8>(field)))));
                    field++;
                }
                if (width < 8u32 && !error_field(bytes.deposit_bits(value, start, width, Byte.from_u8(convert<u8>(1u32 << width))), Byte.from_u8(convert<u8>(1u32 << width)), width)) return 1;
                width++;
            }
            start++;
        }
        usize row = convert<usize>(raw);
        if (extracted != expected_extract[row] || deposited != expected_deposit[row]) return 2;
        raw++;
    }
    return 0;
}
i32 scalar_nibbles() {
    mut u32 high = 0;
    while (high < 256u32) {
        mut u32 low = 0;
        while (low < 256u32) {
            Result<byte, bytes.ByteError> joined = bytes.join_nibbles(Byte.from_u8(convert<u8>(high)), Byte.from_u8(convert<u8>(low)));
            if (high < 16u32 && low < 16u32) {
                if (Byte.to_u8(success(joined)) != convert<u8>(high * 16u32 + low)) return 1;
            } else {
                switch (move joined) {
                    case Result<byte, bytes.ByteError>.Err { bytes.ByteError reason = error }:
                        switch (reason) {
                            case bytes.ByteError.InvalidNibble { byte actual = value, u32 position = position }:
                                if (position != (high >= 16u32 ? 0u32 : 1u32) || Byte.to_u8(actual) != convert<u8>(high >= 16u32 ? high : low)) return 2;
                                break;
                            default: return 3; break;
                        }
                        break;
                    default: return 4; break;
                }
            }
            low++;
        }
        high++;
    }
    return 0;
}
i32 scalar_binary() {
''')
    source.append('    string_view expected = "'+"".join(f"{v:08b}" for v in range(256))+'";\n')
    source.append(r'''    mut u32 raw = 0;
    while (raw < 256u32) {
        byte value = Byte.from_u8(convert<u8>(raw));
        usize start = convert<usize>(raw) * 8;
        string_view golden = success(string.slice_bytes(expected, start, start + 8));
        string actual = success(bytes.format_binary(value));
        if (!string.equals(string.view(&actual), golden) || !bytes.equal(success(bytes.parse_binary(golden)), value)) return 1;
        mut byte output[9] = [Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8)];
        if (success(bytes.format_binary_into(value, Slice.from_array_mut(&mut output))) != 8 || Byte.to_u8(output[8]) != 170u8) return 2;
        Slice<byte> wanted = string.as_bytes(golden);
        mut usize index = 0;
        while (index < 8) { if (!bytes.equal(output[index], wanted[index])) return 3; index++; }
        raw++;
    }
    return 0;
}
i32 scalar_binary_ascii() {
    mut byte input[8] = [Byte.from_u8(48u8), Byte.from_u8(48u8), Byte.from_u8(48u8), Byte.from_u8(48u8), Byte.from_u8(48u8), Byte.from_u8(48u8), Byte.from_u8(48u8), Byte.from_u8(48u8)];
    mut u32 digit = 0;
    while (digit < 128u32) {
        mut usize position = 0;
        while (position < 8) {
            mut usize reset = 0;
            while (reset < 8) { input[reset] = Byte.from_u8(48u8); reset++; }
            input[position] = Byte.from_u8(convert<u8>(digit));
            string text = success(string.from_utf8(Slice.from_array(&input)));
            if (digit == 48u32 || digit == 49u32) {
                u32 expected = digit == 48u32 ? 0u32 : (1u32 << convert<u32>(7usize - position));
                if (Byte.to_u8(success(bytes.parse_binary(string.view(&text)))) != convert<u8>(expected)) return 1;
            } else {
                if (!error_binary_digit(bytes.parse_binary(string.view(&text)), position)) return 2;
            }
            position++;
        }
        digit++;
    }
    return 0;
}
i32 scalar_ordering() {
    i32 inputs[7] = [-2147483648i32, -7, -1, 0, 1, 9, 2147483647];
    mut usize index = 0;
    while (index < 7) {
        i32 input = inputs[index];
        ordering.Ordering value = ordering.from_i32(input);
        i32 canonical = input < 0 ? -1 : (input > 0 ? 1 : 0);
        if (ordering.to_i32(value) != canonical || ordering.to_i32(ordering.reverse(value)) != -canonical) return 1;
        if (ordering.is_eq(value) != (input == 0) || ordering.is_ne(value) != (input != 0)) return 2;
        if (ordering.is_lt(value) != (input < 0) || ordering.is_le(value) != (input <= 0) || ordering.is_gt(value) != (input > 0) || ordering.is_ge(value) != (input >= 0)) return 3;
        mut i32 next = -1;
        while (next <= 1) {
            if (ordering.to_i32(ordering.then(value, ordering.from_i32(next))) != (canonical == 0 ? next : canonical)) return 4;
            next++;
        }
        index++;
    }
    bytes.Ordering reexported = ordering.Ordering.Equal;
    if (ordering.to_i32(reexported) != 0) return 5;
    byte empty[0] = [];
    byte first[3] = [Byte.from_u8(0u8), Byte.from_u8(128u8), Byte.from_u8(255u8)];
    byte same[3] = [Byte.from_u8(0u8), Byte.from_u8(128u8), Byte.from_u8(255u8)];
    byte prefix[2] = [Byte.from_u8(0u8), Byte.from_u8(128u8)];
    byte greater[2] = [Byte.from_u8(0u8), Byte.from_u8(129u8)];
    if (!ordering.is_eq(bytes.compare_lexicographic(Slice.from_array(&empty), Slice.from_array(&empty)))) return 6;
    if (!ordering.is_lt(bytes.compare_lexicographic(Slice.from_array(&empty), Slice.from_array(&first)))) return 7;
    if (!ordering.is_eq(bytes.compare_lexicographic(Slice.from_array(&first), Slice.from_array(&same)))) return 8;
    if (!ordering.is_lt(bytes.compare_lexicographic(Slice.from_array(&prefix), Slice.from_array(&first)))) return 9;
    if (!ordering.is_gt(bytes.compare_lexicographic(Slice.from_array(&greater), Slice.from_array(&first)))) return 10;
    return 0;
}
''')
    for name, variant, parameters, arguments, fields in [
        ("field", "FieldOutOfRange", "byte expected_field, u32 expected_width", "byte field = field, u32 width = width", "bytes.equal(field, expected_field) && width == expected_width"),
        ("shift", "InvalidShift", "u32 expected", "u32 count = count", "count == expected"),
        ("index", "InvalidBitIndex", "u32 expected", "u32 index = index", "index == expected"),
        ("range", "InvalidBitRange", "u32 expected_start, u32 expected_width", "u32 start = start, u32 width = width", "start == expected_start && width == expected_width"),
        ("binary_length", "InvalidBinaryLength", "usize expected", "usize length = length", "length == expected"),
        ("binary_digit", "InvalidBinaryDigit", "usize expected", "usize offset = offset", "offset == expected"),
    ]:
        source.append(f'''bool error_{name}<T>(Result<T, bytes.ByteError> input, {parameters}) {{
    switch (move input) {{
        case Result<T, bytes.ByteError>.Err {{ bytes.ByteError reason = error }}:
            switch (reason) {{
                case bytes.ByteError.{variant} {{ {arguments} }}: return {fields}; break;
                default: return false; break;
            }}
            break;
        default: return false; break;
    }}
}}
''')
    source.append(r'''i32 scalar_rejected() {
    byte value = Byte.from_u8(170u8);
    u32 bad[3] = [8u32, 31u32, 4294967295u32];
    mut usize index = 0;
    while (index < 3) {
        u32 count = bad[index];
        if (!error_shift(bytes.shift_left(value, count), count) || !error_shift(bytes.shift_right(value, count), count)) return 1;
        if (!error_index(bytes.test_bit(value, count), count) || !error_index(bytes.set_bit(value, count), count) || !error_index(bytes.clear_bit(value, count), count)) return 2;
        if (!error_index(bytes.toggle_bit(value, count), count) || !error_index(bytes.with_bit(value, count, true), count) || !error_index(bytes.with_bit(value, count, false), count)) return 3;
        index++;
    }
    if (!error_range(bytes.extract_bits(value, 9u32, 0u32), 9u32, 0u32) || !error_range(bytes.deposit_bits(value, 9u32, 0u32, value), 9u32, 0u32)) return 4;
    if (!error_range(bytes.extract_bits(value, 4294967295u32, 1u32), 4294967295u32, 1u32) || !error_range(bytes.deposit_bits(value, 0u32, 4294967295u32, value), 0u32, 4294967295u32)) return 5;
    if (!error_range(bytes.extract_bits(value, 7u32, 2u32), 7u32, 2u32) || !error_range(bytes.deposit_bits(value, 8u32, 1u32, value), 8u32, 1u32)) return 6;
    if (!error_binary_length(bytes.parse_binary(""), 0) || !error_binary_length(bytes.parse_binary("0000000"), 7) || !error_binary_length(bytes.parse_binary("000000000"), 9)) return 7;
    if (!error_binary_digit(bytes.parse_binary("x0000000"), 0) || !error_binary_digit(bytes.parse_binary("0000000x"), 7) || !error_binary_digit(bytes.parse_binary("000000é"), 6)) return 8;
    mut byte output[7] = [Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8)];
    if (!is_error(bytes.format_binary_into(value, Slice.from_array_mut(&mut output)), "bytes.out_of_bounds")) return 9;
    mut usize position = 0;
    while (position < 7) { if (Byte.to_u8(output[position]) != 170u8) return 10; position++; }
    bytes.ByteError field_error = bytes.ByteError.FieldOutOfRange { field = value, width = 0u32 };
    AnyError erased = success(AnyError.pack(field_error));
    if (!string.equals(erased.code(), "bytes.field_out_of_range") || string.is_empty(erased.message())) return 11;
    return 0;
}
i32 main() {
    if (scalar_unary() != 0) return 1;
    if (scalar_pairs() != 0) return 2;
    if (scalar_positions() != 0) return 3;
    if (scalar_fields() != 0) return 4;
    if (scalar_nibbles() != 0) return 5;
    if (scalar_binary() != 0) return 6;
    if (scalar_binary_ascii() != 0) return 9;
    if (scalar_ordering() != 0) return 7;
    if (scalar_rejected() != 0) return 8;
    return 0;
}
''')
    return "".join(source), {"scalar_values":256,"ordered_byte_pairs":65536,
            "contained_ranges_per_value":len(ranges), "valid_deposits_per_value":sum(1<<width for _,width in ranges),
            "fingerprint":{"base":257,"prime":prime,"scope":"Per-left-value ordered output rows; compact probabilistic cross-check, not collision-free proof."},
            "direct_checks":"All unary outputs, 256 binary strings, equality/order pairs, all nibble pair success/error payloads, and range/error fields."}


def copy_goldens():
    """Cross-check overlap and range errors against Python list slice assignment."""
    prime = 4294967291

    def accumulate(current, values):
        for value in values:
            current = (current * 257 + value) % prime
        return current

    source = [r'''/* Python list-slice memmove oracle, including every small open/closed source and destination bound. */
import bytes;
T success<T, E>(Result<T, E> input) {
    switch (move input) { case Result<T,E>.Ok { T output = value }: return output; break; default: Never.unreachable(); break; }
}
T convert<T,U>(U input) { return success(Integer.convert<T>(input)); }
u64 hash_value(u64 current, usize value) { return (current * 257u64 + convert<u64>(value)) % 4294967291u64; }
u64 hash_outcome(u64 current, Result<unit,bytes.ByteError> outcome) {
    switch (move outcome) {
        case Result<unit,bytes.ByteError>.Ok: return hash_value(current, 0); break;
        case Result<unit,bytes.ByteError>.Err { bytes.ByteError reason = error }:
            switch (reason) {
                case bytes.ByteError.OutOfBounds { usize offset = offset, usize width = width, usize length = length }:
                    if (!string.equals(reason.code(), "bytes.out_of_bounds")) Never.unreachable();
                    return hash_value(hash_value(hash_value(hash_value(current, 1), offset), width), length);
                    break;
                case bytes.ByteError.ReversedRange { usize start = start, usize end = end }:
                    if (!string.equals(reason.code(), "bytes.reversed_range")) Never.unreachable();
                    return hash_value(hash_value(hash_value(current, 2), start), end);
                    break;
                default: Never.unreachable(); break;
            }
            break;
    }
}
''']
    records = []
    for length in range(9):
        wanted = 0
        cases = 0
        for start in range(length+2):
            for end in range(length+2):
                for destination in range(length+2):
                    for inclusive in (False,True):
                        original = [i*31+7 for i in range(length)]
                        if start > end:
                            observations = [2,start,end]
                        elif start > length:
                            observations = [1,start,0,length]
                        elif inclusive and end >= length:
                            observations = [1,end,1,length]
                        elif not inclusive and end > length:
                            observations = [1,start,end-start,length]
                        else:
                            stop = end+1 if inclusive else end
                            width = stop-start
                            if destination > length or width > length-destination:
                                observations = [1,destination,width,length]
                            else:
                                observations = [0]
                                original[destination:destination+width] = original[start:stop]
                        wanted = accumulate(wanted, observations+original)
                        cases += 1
        records.append({"buffer_length":length,"cases":cases,"fingerprint":wanted})
        initial = ", ".join(byte_literal(0) for _ in range(length))
        source.append(f'''i32 copy_length_{length}() {{
    mut byte output[{length}] = [{initial}];
    mut u64 fingerprint = 0;
    mut usize start = 0;
    while (start < {length+2}usize) {{
        mut usize end = 0;
        while (end < {length+2}usize) {{
            mut usize destination = 0;
            while (destination < {length+2}usize) {{
                mut usize inclusive = 0;
                while (inclusive < 2usize) {{
                    mut usize reset = 0;
                    while (reset < {length}usize) {{ output[reset] = Byte.from_u8(convert<u8>(reset * 31 + 7)); reset++; }}
                    Range<usize> extent = inclusive == 0 ? Range.new(start, end) : Range.closed(start, end);
                    Result<unit,bytes.ByteError> outcome = bytes.copy_within(Slice.from_array_mut(&mut output), extent, destination);
                    fingerprint = hash_outcome(fingerprint, outcome);
                    mut usize index = 0;
                    while (index < {length}usize) {{ fingerprint = hash_value(fingerprint, convert<usize>(Byte.to_u8(output[index]))); index++; }}
                    inclusive++;
                }}
                destination++;
            }}
            end++;
        }}
        start++;
    }}
    return fingerprint == {wanted}u64 ? 0 : 1;
}}
''')
    source.append(r'''bool bounds(Result<unit,bytes.ByteError> outcome, usize expected_offset, usize expected_width, usize expected_length) {
    switch (move outcome) {
        case Result<unit,bytes.ByteError>.Err { bytes.ByteError reason = error }:
            switch (reason) {
                case bytes.ByteError.OutOfBounds { usize offset = offset, usize width = width, usize length = length }: return offset == expected_offset && width == expected_width && length == expected_length; break;
                default: return false; break;
            }
            break;
        default: return false; break;
    }
}
bool noncontiguous(Result<unit,bytes.ByteError> outcome) {
    switch (move outcome) {
        case Result<unit,bytes.ByteError>.Err { bytes.ByteError reason = error }:
            switch (reason) {
                case bytes.ByteError.NonContiguousRange: return string.equals(reason.code(), "bytes.non_contiguous_range") && !string.is_empty(reason.message()); break;
                default: return false; break;
            }
            break;
        default: return false; break;
    }
}
i32 copy_extremes() {
    mut byte output[3] = [Byte.from_u8(7u8), Byte.from_u8(38u8), Byte.from_u8(69u8)];
    usize limit = 18446744073709551615usize;
    if (!bounds(bytes.copy_within(Slice.from_array_mut(&mut output), Range.new(limit, limit), 0), limit, 0, 3)) return 1;
    if (!bounds(bytes.copy_within(Slice.from_array_mut(&mut output), Range.closed(0usize, limit), 0), limit, 1, 3)) return 2;
    if (!bounds(bytes.copy_within(Slice.from_array_mut(&mut output), Range.new(0usize, limit), 0), 0, limit, 3)) return 3;
    if (!bounds(bytes.copy_within(Slice.from_array_mut(&mut output), Range.new(0usize, 1usize), limit), limit, 1, 3)) return 4;
    if (!bounds(bytes.copy_within(Slice.from_array_mut(&mut output), Range.new(3usize, 3usize), limit), limit, 0, 3)) return 5;
    if (!noncontiguous(bytes.copy_within(Slice.from_array_mut(&mut output), Range.descending(2usize, 0usize), 0))) return 6;
    if (!noncontiguous(bytes.copy_within(Slice.from_array_mut(&mut output), Range.descending(0usize, 0usize), 0))) return 7;
    if (!noncontiguous(bytes.copy_within(Slice.from_array_mut(&mut output), success(Range.with_step(Range.new(0usize, 3usize), 2usize)), 0))) return 8;
    if (!noncontiguous(bytes.copy_within(Slice.from_array_mut(&mut output), success(Range.with_step(Range.new(3usize, 3usize), 2usize)), 0))) return 9;
    if (Byte.to_u8(output[0]) != 7u8 || Byte.to_u8(output[1]) != 38u8 || Byte.to_u8(output[2]) != 69u8) return 10;
    success(bytes.copy_within(Slice.from_array_mut(&mut output), Range.new(3usize, 3usize), 3));
    if (Byte.to_u8(output[0]) != 7u8 || Byte.to_u8(output[1]) != 38u8 || Byte.to_u8(output[2]) != 69u8) return 11;
    return 0;
}
i32 main() {
''')
    for length in range(9):
        source.append(f"    if (copy_length_{length}() != 0) return {length+1};\n")
    source.append("    if (copy_extremes() != 0) return 10;\n    return 0;\n}\n")
    return "".join(source), {"oracle":"Python list slice assignment with prevalidated bounds and explicit open/closed extent policy",
            "cases":sum(record["cases"] for record in records),"rows":records,
            "limits":"Per-length fingerprints are compact probabilistic checks, not collision-free proofs; MAX bounds and descriptor rejection are direct checks."}


def utf8_goldens():
    """Use Python strict UTF-8 validity, with explicit byte-position contracts."""
    def classify(data):
        try:
            data.decode("utf-8", "strict")
            return 0
        except UnicodeDecodeError:
            # In two bytes: an ASCII prefix or a complete two-byte lead places
            # the actual invalid continuation at1. Longer truncated leads report0.
            if len(data) == 2 and (data[0] < 128 or 0xC2 <= data[0] <= 0xDF):
                return 2
            return 1

    first = [classify(bytes([value])) for value in range(256)]
    rows = []
    for lead in range(256):
        current = 0
        for continuation in range(256):
            current = (current*257+classify(bytes([lead, continuation]))) % 4294967291
        rows.append(current)
    source = [r'''/* Python strict UTF-8 validity; all one/two-byte inputs plus explicit malformed offsets. */
import bytes;
T success<T,E>(Result<T,E> input) { switch(move input){case Result<T,E>.Ok{T value=value}:return value;break;default:Never.unreachable();break;} }
T convert<T,U>(U input) { return success(Integer.convert<T>(input)); }
u64 classification(Result<unit,bytes.ByteError> input) {
    switch(move input) {
        case Result<unit,bytes.ByteError>.Ok:return 0u64;break;
        case Result<unit,bytes.ByteError>.Err {bytes.ByteError reason=error}:
            switch(reason) {
                case bytes.ByteError.InvalidUtf8 {usize offset=offset}:
                    if(!string.equals(reason.code(),"bytes.invalid_utf8") || string.is_empty(reason.message()))Never.unreachable();
                    return convert<u64>(offset)+1u64;break;
                default:Never.unreachable();break;
            }
            break;
    }
}
bool invalid<T>(Result<T,bytes.ByteError> input,usize expected) {
    switch(move input) {
        case Result<T,bytes.ByteError>.Err {bytes.ByteError reason=error}:
            switch(reason) {case bytes.ByteError.InvalidUtf8 {usize offset=offset}:return offset==expected;break;default:return false;break;}
            break;
        default:return false;break;
    }
}
i32 all_small() {
''']
    source.append('    u64 first[256]=['+', '.join(f'{v}u64' for v in first)+'];\n')
    source.append('    u64 rows[256]=['+', '.join(f'{v}u64' for v in rows)+'];\n')
    source.append(r'''    mut byte one[1]=[Byte.from_u8(0u8)];
    mut byte two[2]=[Byte.from_u8(0u8),Byte.from_u8(0u8)];
    mut u32 lead=0;
    while(lead<256u32) {
        one[0]=Byte.from_u8(convert<u8>(lead));
        if(classification(bytes.utf8_validate(Slice.from_array(&one)))!=first[convert<usize>(lead)])return 1;
        mut u64 actual=0;
        mut u32 next=0;
        while(next<256u32) {
            two[0]=Byte.from_u8(convert<u8>(lead));two[1]=Byte.from_u8(convert<u8>(next));
            actual=(actual*257u64+classification(bytes.utf8_validate(Slice.from_array(&two))))%4294967291u64;
            if(Byte.to_u8(two[0])!=convert<u8>(lead) || Byte.to_u8(two[1])!=convert<u8>(next))return 2;
            next++;
        }
        if(actual!=rows[convert<usize>(lead)])return 3;
        lead++;
    }
    return 0;
}
''')
    malformed = [([0x80],0),([0xC0,0x80],0),([0xC2],0),([0xC2,0x20],1),([0xE0,0x80,0x80],1),
                 ([0xED,0xA0,0x80],1),([0xF0,0x80,0x80,0x80],1),([0xF4,0x90,0x80,0x80],1),
                 ([0xF5,0x80,0x80,0x80],0),([0xE1,0x80],0),([0xF0,0x9F,0x98],0),
                 ([0x41,0xE2,0x82,0x20],3),([0x41,0xE2,0x20,0x80],2),([0x41,0xE2,0x82],1),
                 ([0xF0,0x9F,0x98,0x80,0xFF],4),([0x41,0x42,0x00,0xED,0xA0,0x80],4)]
    for index, (data,offset) in enumerate(malformed):
        try:
            bytes(data).decode("utf-8", "strict")
            raise AssertionError(data)
        except UnicodeDecodeError:
            pass
        source.append(f'i32 malformed_{index}() {{\n    byte input[{len(data)}]=['+', '.join(map(byte_literal,data))+'];\n')
        source.append(f'    if(!invalid(bytes.utf8_validate(Slice.from_array(&input)),{offset}) || !invalid(bytes.decode_utf8(Slice.from_array(&input)),{offset}))return 1;\n')
        source.append('    if('+' || '.join(f'Byte.to_u8(input[{i}])!={value}u8' for i,value in enumerate(data))+')return 2;\n    return 0;\n}\n')
    source.append('i32 main(){if(all_small()!=0)return 1;\n')
    for index in range(len(malformed)):
        source.append(f'    if(malformed_{index}()!=0)return {index+2};\n')
    source.append('    return 0;\n}\n')
    return ''.join(source), {'oracle':'Python bytes.decode("utf-8","strict") for validity; explicit invalid-byte/truncated-start offset policy',
            'one_byte_inputs':256,'two_byte_inputs':65536,'malformed_cases':[{'bytes':data,'offset':offset} for data,offset in malformed],
            'fingerprint':'Two-byte rows use base257/prime4294967291 compact probabilistic fingerprints; one-byte and malformed offset checks are direct.'}



def float_goldens():
    output, calls, records = [], [], []
    for kind in ("f16", "bf16"):
        name = "float_exhaustive_" + kind
        calls.append(f"    if ({name}() != 0) return 120;\n")
        output.append(f"""i32 {name}() {{
    mut byte output[4] = [Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8)];
    mut u32 pattern = 0;
    while (pattern < 65536u32) {{
        u16 bits = convert<u16>(pattern);
        {kind} value = Float.from_bits<{kind}>(bits);
        u8 low = convert<u8>(pattern % 256u32);
        u8 high = convert<u8>(pattern / 256u32);
        if (Float.to_bits(value) != bits) return 1;
        if (success(bytes.write_{kind}_le(Slice.from_array_mut(&mut output), 1, value)) != 2) return 2;
        if (Byte.to_u8(output[1]) != low || Byte.to_u8(output[2]) != high) return 3;
        if (Float.to_bits(success(bytes.read_{kind}_le(Slice.from_array(&output), 1))) != bits) return 4;
        if (success(bytes.write_{kind}_be(Slice.from_array_mut(&mut output), 1, value)) != 2) return 5;
        if (Byte.to_u8(output[1]) != high || Byte.to_u8(output[2]) != low) return 6;
        if (Float.to_bits(success(bytes.read_{kind}_be(Slice.from_array(&output), 1))) != bits) return 7;
        if (Byte.to_u8(output[0]) != 170u8 || Byte.to_u8(output[3]) != 170u8) return 8;
        pattern++;
    }}
    return 0;
}}
""")
    rng = random.Random(0xAE71_F10A)
    for kind, bits, exponent, fraction in [("f32",32,8,23),("f64",64,11,52)]:
        width = bits // 8
        sign = 1 << (bits - 1)
        infinity = ((1 << exponent) - 1) << fraction
        patterns = {0,sign,1,sign|1,(1<<fraction)-1,sign|((1<<fraction)-1),1<<fraction,
                    sign|(1<<fraction),infinity-1,sign|(infinity-1),infinity,sign|infinity,
                    infinity|1,sign|infinity|1,infinity|(1<<(fraction-1)),
                    sign|infinity|(1<<(fraction-1)),infinity|((1<<fraction)-1),
                    sign|infinity|((1<<fraction)-1),infinity|0x12345,sign|infinity|0x12345}
        patterns.update(rng.getrandbits(bits) for _ in range(24))
        for index, pattern in enumerate(sorted(patterns)):
            name = f"float_{kind}_{index}"
            calls.append(f"    if ({name}() != 0) return {121+index};\n")
            little = list(pattern.to_bytes(width,"little")); big = list(reversed(little))
            records.append({"type":kind,"bits":pattern,"little":little,"big":big})
            output.append(f"i32 {name}() {{\n    u{bits} bits = {pattern}u{bits};\n    {kind} value = Float.from_bits<{kind}>(bits);\n")
            output.append(f"    mut byte output[{width+2}] = ["+", ".join([byte_literal(170)]*(width+2))+"];\n")
            for endian, wanted in [("le",little),("be",big)]:
                output.append(f"    byte expected_{endian}[{width}] = ["+", ".join(map(byte_literal,wanted))+"];\n")
                output.append(f"    if (Float.to_bits(success(bytes.read_{kind}_{endian}(Slice.from_array(&expected_{endian}), 0))) != bits) return 1;\n")
                output.append(f"    if (success(bytes.write_{kind}_{endian}(Slice.from_array_mut(&mut output), 1, value)) != {width}) return 2;\n")
                conditions=" || ".join(f"Byte.to_u8(output[{i+1}]) != {item}u8" for i,item in enumerate(wanted))
                output.append(f"    if ({conditions}) return 3;\n")
                output.append(f"    if (Float.to_bits(success(bytes.read_{kind}_{endian}(Slice.from_array(&output), 1))) != bits) return 4;\n")
            output.append(f"    if (Byte.to_u8(output[0]) != 170u8 || Byte.to_u8(output[{width+1}]) != 170u8) return 5;\n    return 0;\n}}\n")
    output.append(r"""
bool float_bounds<T>(Result<T, bytes.ByteError> input, usize offset, usize width, usize length) {
    switch (move input) {
        case Result<T, bytes.ByteError>.Err { bytes.ByteError reason = error }:
            switch (reason) {
                case bytes.ByteError.OutOfBounds { usize actual_offset = offset, usize actual_width = width, usize actual_length = length }:
                    return actual_offset == offset && actual_width == width && actual_length == length;
                    break;
                default: return false; break;
            }
            break;
        default: return false; break;
    }
}
i32 float_rejected_ranges() {
    mut byte output[1] = [Byte.from_u8(170u8)];
""")
    for kind,bits in [("f16",16),("bf16",16),("f32",32),("f64",64)]:
        width = bits//8
        for endian in ("le","be"):
            for offset in (0,18446744073709551615):
                output.append(f"    if (!float_bounds(bytes.read_{kind}_{endian}(Slice.from_array(&output), {offset}usize), {offset}usize, {width}, 1)) return 1;\n")
                output.append(f"    if (!float_bounds(bytes.write_{kind}_{endian}(Slice.from_array_mut(&mut output), {offset}usize, Float.from_bits<{kind}>({(1<<bits)-1}u{bits})), {offset}usize, {width}, 1)) return 2;\n")
                output.append("    if (Byte.to_u8(output[0]) != 170u8) return 3;\n")
    output.append("    return 0;\n}\n")
    calls.append("    if (float_rejected_ranges() != 0) return 190;\n")
    return "".join(output),calls,records


def generate():
    output = [r'''/* Independent Python int.to_bytes/int.from_bytes goldens; no host byte-order assumption. */
import bytes;
T success<T, E>(Result<T, E> input) {
    switch (move input) {
        case Result<T, E>.Ok { T value = value }: return value; break;
        case Result<T, E>.Err: Never.unreachable(); break;
    }
}
T convert<T, U>(U input) { return success(Integer.convert<T>(input)); }
i32 exhaustive_small() {
    mut byte scratch[18] = [Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8), Byte.from_u8(170u8)];
    mut u32 raw = 0;
    while (raw < 256u32) {
        u8 unsigned_value = convert<u8>(raw);
        i8 signed_value = convert<i8>(raw < 128u32 ? convert<i32>(raw) : convert<i32>(raw) - 256);
        if (success(bytes.write_u8(Slice.from_array_mut(&mut scratch), 1, unsigned_value)) != 1) return 1;
        if (Byte.to_u8(scratch[1]) != unsigned_value || success(bytes.read_u8(Slice.from_array(&scratch), 1)) != unsigned_value) return 2;
        if (success(bytes.read_i8(Slice.from_array(&scratch), 1)) != signed_value) return 3;
        success(bytes.write_i8(Slice.from_array_mut(&mut scratch), 1, signed_value));
        if (Byte.to_u8(scratch[1]) != unsigned_value) return 4;
        raw++;
    }
    raw = 0;
    while (raw < 65536u32) {
        u16 unsigned_unit = convert<u16>(raw);
        i16 signed_unit = convert<i16>(raw < 32768u32 ? convert<i32>(raw) : convert<i32>(raw) - 65536);
        u8 low = convert<u8>(raw % 256u32);
        u8 high = convert<u8>(raw / 256u32);
        success(bytes.write_u16_le(Slice.from_array_mut(&mut scratch), 1, unsigned_unit));
        if (Byte.to_u8(scratch[1]) != low || Byte.to_u8(scratch[2]) != high) return 5;
        if (success(bytes.read_u16_le(Slice.from_array(&scratch), 1)) != unsigned_unit || success(bytes.read_i16_le(Slice.from_array(&scratch), 1)) != signed_unit) return 6;
        success(bytes.write_u16_be(Slice.from_array_mut(&mut scratch), 1, unsigned_unit));
        if (Byte.to_u8(scratch[1]) != high || Byte.to_u8(scratch[2]) != low) return 7;
        if (success(bytes.read_u16_be(Slice.from_array(&scratch), 1)) != unsigned_unit || success(bytes.read_i16_be(Slice.from_array(&scratch), 1)) != signed_unit) return 8;
        success(bytes.write_i16_le(Slice.from_array_mut(&mut scratch), 1, signed_unit));
        if (Byte.to_u8(scratch[1]) != low || Byte.to_u8(scratch[2]) != high) return 9;
        success(bytes.write_i16_be(Slice.from_array_mut(&mut scratch), 1, signed_unit));
        if (Byte.to_u8(scratch[1]) != high || Byte.to_u8(scratch[2]) != low) return 10;
        raw++;
    }
    if (Byte.to_u8(scratch[0]) != 170u8 || Byte.to_u8(scratch[17]) != 170u8) return 11;
''']
    rng = random.Random(0xAE71_0300)
    output.append("    return 0;\n}\n")
    records = []
    entry_calls = []
    for bits in (32, 64, 128):
        width = bits // 8
        values = sorted({0, 1, 127, 128, 255, 256, (1 << (bits - 1)) - 1,
                         1 << (bits - 1), (1 << (bits - 1)) + 1, (1 << bits) - 2,
                         (1 << bits) - 1, *[rng.getrandbits(bits) for _ in range(20)]})
        for case, value in enumerate(values):
            raw_bytes = value.to_bytes(width, "little", signed=False)
            signed = int.from_bytes(raw_bytes, "little", signed=True)
            record = {"bits": bits, "unsigned": value, "signed": signed,
                      "little": list(raw_bytes), "big": list(reversed(raw_bytes)), "exit_code": 20 + len(records)}
            records.append(record)
            name = f"wide_{bits}_{case}"
            entry_calls.append(f"    if ({name}() != 0) return {20 + len(records) - 1};\n")
            output.append(f"i32 {name}() {{\n")
            output.append("    mut byte scratch[18] = [" + ", ".join(["Byte.from_u8(170u8)"] * 18) + "];\n")
            output.append(f"        u{bits} expected_unsigned = {value}u{bits};\n")
            output.append(f"        i{bits} expected_signed = {signed}i{bits};\n")
            for endian in ("le", "be"):
                wanted = record["little" if endian == "le" else "big"]
                checks = " || ".join(f"Byte.to_u8(scratch[{index+1}]) != {item}u8" for index, item in enumerate(wanted))
                code = record["exit_code"]
                output.append(f"        success(bytes.write_u{bits}_{endian}(Slice.from_array_mut(&mut scratch), 1, expected_unsigned));\n")
                output.append(f"        if ({checks}) return {code};\n")
                output.append(f"        if (success(bytes.read_u{bits}_{endian}(Slice.from_array(&scratch), 1)) != expected_unsigned || success(bytes.read_i{bits}_{endian}(Slice.from_array(&scratch), 1)) != expected_signed) return {code};\n")
                output.append(f"        success(bytes.write_i{bits}_{endian}(Slice.from_array_mut(&mut scratch), 1, expected_signed));\n")
                output.append(f"        if ({checks}) return {code};\n")
            output.append("    return 0;\n}\n")
    float_source,float_calls,float_records = float_goldens()
    output.append(float_source)
    output.append('i32 main() { if (exhaustive_small() != 0) return 1;\n' + ''.join(entry_calls+float_calls) + '    return 0;\n}\n')
    manifest = {"schema_version": 1, "oracle": "Python int.to_bytes / int.from_bytes, independent of host byte order",
                "seed": "0xAE710300", "exhaustive_bit_patterns": {"8": 256, "16": 65536},
                "wide_cases": records, "source_function_contracts": 128, "float_exhaustive_bit_patterns": {"f16":65536,"bf16":65536}, "float_cases":float_records,
                "separate_contract_fixtures": ["tests/v03_library_bytes_buffers.avx", "tests/v03_library_bytes_utf8.avx"],
                "limits": "No native-endian codecs or numerical float conversion/canonicalization is claimed; float codecs preserve representation bits."}
    return "".join(output), manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--scalar-output", type=Path)
    parser.add_argument("--copy-output", type=Path)
    parser.add_argument("--utf8-output", type=Path)
    args = parser.parse_args()
    source, metadata = generate()
    if args.scalar_output:
        scalar_source, metadata["scalar_oracle"] = scalar_goldens()
        args.scalar_output.write_text(scalar_source)
        metadata["scalar_source_sha256"] = hashlib.sha256(scalar_source.encode()).hexdigest()
    if args.copy_output:
        copy_source, metadata["copy_oracle"] = copy_goldens()
        args.copy_output.write_text(copy_source)
        metadata["copy_source_sha256"] = hashlib.sha256(copy_source.encode()).hexdigest()
    if args.utf8_output:
        utf8_source, metadata["utf8_oracle"] = utf8_goldens()
        args.utf8_output.write_text(utf8_source)
        metadata["utf8_source_sha256"] = hashlib.sha256(utf8_source.encode()).hexdigest()
    metadata["source_sha256"] = hashlib.sha256(source.encode()).hexdigest()
    args.output.write_text(source)
    args.metadata.write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"Generated byte goldens: 256 eight-bit and65536 sixteen-bit patterns, {len(metadata['wide_cases'])} wide Python cases; f16+bf16 each65536 patterns, {len(metadata['float_cases'])} f32/f64 raw-bit cases.")


if __name__ == "__main__":
    main()
