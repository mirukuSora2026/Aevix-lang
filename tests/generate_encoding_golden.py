# ========================
#
# Created By : miruku_sora2026
#
# File name : generate_encoding_golden.py
#
# Description :
#     Python 표준 코덱으로 전체 유니코드 스칼라와 짧은 UTF-8 입력의 독립 기준값을 생성한다.
#     Generate independent all-scalar and short UTF-8 golden values with Python standard codecs.
#
# ========================
import argparse
import json
from pathlib import Path


MODULUS = 4294967291


def digest(values):
    result = 0
    for value in values:
        result = (result * 257 + value + 17) % MODULUS
    return result


def reference():
    scalars = [value for value in range(0x110000) if not 0xD800 <= value <= 0xDFFF]
    text = "".join(map(chr, scalars))
    utf8 = text.encode("utf-8", errors="strict")
    utf16 = text.encode("utf-16-le", errors="strict")
    units = [int.from_bytes(utf16[index:index + 2], "little") for index in range(0, len(utf16), 2)]
    classifications = []
    valid = 0
    for width in (1, 2):
        for value in range(1 << (width * 8)):
            data = value.to_bytes(width, "big")
            try:
                decoded = data.decode("utf-8", errors="strict")
                classification = len(decoded) + 1
                valid += 1
            except UnicodeDecodeError:
                classification = 0
            classifications.append(classification)
    return {
        "scalar_count": len(scalars), "scalar_hash": digest(scalars),
        "utf8_length": len(utf8), "utf8_hash": digest(utf8),
        "utf16_length": len(units), "utf16_hash": digest(units),
        "short_input_count": len(classifications), "short_valid_count": valid,
        "short_hash": digest(classifications),
    }


SOURCE = r'''import encoding;
/* Independent Python strict-codec golden values cover every Unicode scalar and
 * every one/two-byte input. U+0000, noncharacters and unassigned scalars remain valid.
 * UTF-16 values are code units; the fixture does not imply native byte order.
 */
T success<T>(Result<T, encoding.EncodingError> input) {
    switch (move input) {
        case Result<T, encoding.EncodingError>.Ok { T value = value }: return value; break;
        case Result<T, encoding.EncodingError>.Err: Never.unreachable(); break;
    }
}
T collection<T>(Result<T, CollectionError> input) {
    switch (move input) {
        case Result<T, CollectionError>.Ok { T value = value }: return value; break;
        case Result<T, CollectionError>.Err: Never.unreachable(); break;
    }
}
T integer<T, U>(U input) {
    Result<T, IntegerError> converted = Integer.convert<T>(input);
    switch (move converted) {
        case Result<T, IntegerError>.Ok { T value = value }: return value; break;
        case Result<T, IntegerError>.Err: Never.unreachable(); break;
    }
}
u64 mix(u64 state, u64 value) { return (state * 257u64 + value + 17u64) % 4294967291u64; }
usize classification(Slice<byte> input) {
    Result<usize, encoding.EncodingError> result = encoding.validate_utf8(input);
    switch (move result) {
        case Result<usize, encoding.EncodingError>.Ok { usize count = value }: return count + 1; break;
        case Result<usize, encoding.EncodingError>.Err: return 0; break;
    }
}
i32 short_inputs() {
    mut byte one[1] = [Byte.from_u8(0u8)];
    mut byte two[2] = [Byte.from_u8(0u8), Byte.from_u8(0u8)];
    mut u32 first = 0;
    mut usize valid = 0;
    mut u64 observed = 0;
    while (first < 256u32) {
        one[0] = Byte.from_u8(integer<u8, u32>(first));
        usize kind = classification(Slice.from_array(&one));
        if (kind != (first < 128u32 ? 2usize : 0usize)) return 1;
        if (kind != 0) valid++;
        observed = mix(observed, integer<u64, usize>(kind));
        first++;
    }
    first = 0;
    while (first < 256u32) {
        two[0] = Byte.from_u8(integer<u8, u32>(first));
        mut u32 second = 0;
        while (second < 256u32) {
            two[1] = Byte.from_u8(integer<u8, u32>(second));
            usize expected = first < 128u32 && second < 128u32 ? 3usize :
                first >= 194u32 && first <= 223u32 && second >= 128u32 && second <= 191u32 ? 2usize : 0usize;
            usize kind = classification(Slice.from_array(&two));
            if (kind != expected) return 2;
            if (kind != 0) valid++;
            observed = mix(observed, integer<u64, usize>(kind));
            second++;
        }
        first++;
    }
    if (observed != @SHORT_HASH@u64 || valid != @SHORT_VALID_COUNT@usize) return 3;
    return 0;
}
i32 all_scalars() {
    Result<Vector<u32>, CollectionError> empty = [];
    mut Vector<u32> scalars = collection(empty);
    collection(Vector.reserve(&mut scalars, @SCALAR_COUNT@usize));
    mut u32 scalar = 0;
    while (scalar < 1114112u32) {
        if (scalar < 55296u32 || scalar > 57343u32) {
            Result<unit, InputError<u32>> appended = Vector.push(&mut scalars, scalar);
            switch (move appended) { case Result<unit, InputError<u32>>.Ok: break; default: Never.unreachable(); break; }
        }
        scalar++;
    }
    Vector<byte> bytes = success(encoding.encode_utf8(Slice.from_vector(&scalars)));
    Vector<u16> units = success(encoding.encode_utf16(Slice.from_vector(&scalars)));
    if (Vector.len(&scalars) != @SCALAR_COUNT@usize || Vector.len(&bytes) != @UTF8_LENGTH@usize || Vector.len(&units) != @UTF16_LENGTH@usize) return 1;
    if (success(encoding.validate_utf8(Slice.from_vector(&bytes))) != @SCALAR_COUNT@usize ||
        success(encoding.validate_utf16(Slice.from_vector(&units))) != @SCALAR_COUNT@usize) return 2;
    if (success(encoding.utf8_encoded_len(Slice.from_vector(&scalars))) != @UTF8_LENGTH@usize ||
        success(encoding.utf16_encoded_len(Slice.from_vector(&scalars))) != @UTF16_LENGTH@usize) return 3;
    mut u64 bytes_hash = 0;
    mut usize index = 0;
    while (index < Vector.len(&bytes)) { bytes_hash = mix(bytes_hash, integer<u64, u8>(Byte.to_u8(bytes[index]))); index++; }
    mut u64 units_hash = 0;
    index = 0;
    while (index < Vector.len(&units)) { units_hash = mix(units_hash, integer<u64, u16>(units[index])); index++; }
    if (bytes_hash != @UTF8_HASH@u64 || units_hash != @UTF16_HASH@u64) return 4;
    Vector<u32> from8 = success(encoding.decode_utf8_scalars(Slice.from_vector(&bytes)));
    Vector<u32> from16 = success(encoding.decode_utf16_scalars(Slice.from_vector(&units)));
    if (Vector.len(&from8) != @SCALAR_COUNT@usize || Vector.len(&from16) != @SCALAR_COUNT@usize) return 5;
    mut u64 scalar_hash = 0;
    index = 0;
    while (index < Vector.len(&scalars)) {
        if (from8[index] != scalars[index] || from16[index] != scalars[index]) return 6;
        scalar_hash = mix(scalar_hash, integer<u64, u32>(from8[index]));
        index++;
    }
    if (scalar_hash != @SCALAR_HASH@u64) return 7;
    return 0;
}
i32 utf16_single_units() {
    mut u16 units[1] = [0u16];
    mut u32 value = 0;
    while (value < 65536u32) {
        units[0] = integer<u16, u32>(value);
        Result<usize, encoding.EncodingError> result = encoding.validate_utf16(Slice.from_array(&units));
        bool invalid = value >= 55296u32 && value <= 57343u32;
        if (Result.is_err(&result) != invalid) return 1;
        if (!invalid && success(result) != 1) return 2;
        value++;
    }
    return 0;
}
i32 main() {
    i32 short_result = short_inputs(); if (short_result != 0) return short_result;
    i32 units_result = utf16_single_units(); if (units_result != 0) return 10 + units_result;
    i32 scalar_result = all_scalars(); return scalar_result == 0 ? 0 : 20 + scalar_result;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--metadata", type=Path)
    args = parser.parse_args()
    golden = reference()
    output = SOURCE
    for name, value in golden.items():
        output = output.replace("@" + name.upper() + "@", str(value))
    assert "@" not in output
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(output)
    if args.metadata:
        args.metadata.write_text(json.dumps(golden, indent=2) + "\n")
    print(json.dumps(golden, sort_keys=True))


if __name__ == "__main__":
    main()
