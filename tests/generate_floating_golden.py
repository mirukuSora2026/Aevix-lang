#!/usr/bin/env python3
# ========================
#
# Created By : miruku_sora2026
#
# File name : generate_floating_golden.py
#
# Description :
#     독립 Python 숫자·유리수 기준으로 분류, 이웃 값, 전체 순서의 정확한 비트를 생성한다.
#     Generate exact classification, adjacent-value, and total-order goldens from independent Python numeric/rational models.
#
# ========================
import argparse
import bisect
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import random
import struct

FORMATS = {"f16": (16, 5, 10), "bf16": (16, 8, 7), "f32": (32, 8, 23), "f64": (64, 11, 52)}


def decode(kind, word):
    width, _, _ = FORMATS[kind]
    if kind == "f16":
        return struct.unpack(">e", word.to_bytes(2, "big"))[0]
    if kind == "bf16":
        return struct.unpack(">f", (word << 16).to_bytes(4, "big"))[0]
    return struct.unpack(">" + ("f" if width == 32 else "d"), word.to_bytes(width // 8, "big"))[0]


def classification(kind, word):
    value = decode(kind, word)
    _, exponent, _ = FORMATS[kind]
    minimum = 2.0 ** (2 - (1 << (exponent - 1)))
    if math.isnan(value): return 0
    if math.isinf(value): return 1
    if value == 0: return 2
    if abs(value) < minimum: return 3
    return 4


def flags(kind, word):
    value, group = decode(kind, word), classification(kind, word)
    answers = [math.isnan(value), math.isinf(value), math.isfinite(value), group == 4,
               group == 3, value == 0, math.copysign(1.0, value) < 0, math.copysign(1.0, value) > 0]
    return sum(int(answer) << i for i, answer in enumerate(answers))


def total_order_key(kind, word):
    width, _, fraction = FORMATS[kind]
    value = decode(kind, word)
    negative = bool(word & (1 << (width - 1)))
    if math.isnan(value):
        significand = word & ((1 << fraction) - 1)
        quiet = bool(significand & (1 << (fraction - 1)))
        payload = significand & ((1 << (fraction - 1)) - 1)
        return (0, -int(quiet), -payload) if negative else (7, int(quiet), payload)
    if math.isinf(value): return (1 if negative else 6,)
    if value == 0: return (3 if negative else 4,)
    return (2 if negative else 5, value)


def compare(kind, left, right):
    a, b = total_order_key(kind, left), total_order_key(kind, right)
    return -1 if a < b else 1 if a > b else 0


def one_word(kind):
    _, exponent, fraction = FORMATS[kind]
    return ((1 << (exponent - 1)) - 1) << fraction


def signum_word(kind, word):
    width, _, _ = FORMATS[kind]
    value = decode(kind, word)
    if math.isnan(value) or value == 0: return word
    return one_word(kind) | (word & (1 << (width - 1)))


def power_two(exponent):
    return Fraction(1 << exponent) if exponent >= 0 else Fraction(1, 1 << -exponent)


def rational_next(kind, word, upward):
    width, exponent_bits, fraction_bits = FORMATS[kind]
    sign = 1 << (width - 1)
    infinity = ((1 << exponent_bits) - 1) << fraction_bits
    value = decode(kind, word)
    if math.isnan(value): return word
    if math.isinf(value):
        if (value > 0) == upward: return word
        return (infinity - 1) | (sign if value < 0 else 0)
    if value == 0: return 1 if upward else sign | 1
    exact = Fraction.from_float(value)
    absolute = abs(exact)
    # Use rational magnitude/spacing, rather than incrementing encoded words like the source implementation.
    exponent = absolute.numerator.bit_length() - absolute.denominator.bit_length()
    if absolute < power_two(exponent): exponent -= 1
    minimum_exponent = 2 - (1 << (exponent_bits - 1))
    toward_zero = (value < 0) == upward
    if toward_zero and absolute == power_two(exponent) and exponent > minimum_exponent:
        exponent -= 1
    spacing = power_two(max(exponent, minimum_exponent) - fraction_bits)
    adjacent = exact + spacing if upward else exact - spacing
    if adjacent == 0: return sign if value < 0 else 0
    maximum = Fraction.from_float(decode(kind, infinity - 1))
    if abs(adjacent) > maximum: return infinity | (sign if adjacent < 0 else 0)
    rounded = float(adjacent)
    if width == 32: return int.from_bytes(struct.pack(">f", rounded), "big")
    return int.from_bytes(struct.pack(">d", rounded), "big")


def exhaustive_table(kind):
    width, _, _ = FORMATS[kind]
    sign = 1 << (width - 1)
    non_nan = [(decode(kind, word), word) for word in range(65536) if classification(kind, word) != 0]
    # Numeric ordering supplies adjacent values independently for the complete small formats.
    numeric = sorted({value for value, _ in non_nan})
    encodings = {value: word for value, word in non_nan if value != 0}
    table = bytearray()
    for word in range(65536):
        value = decode(kind, word)
        group = classification(kind, word)
        if group == 0:
            up = down = word
        elif value == 0:
            up, down = 1, sign | 1
        else:
            index = bisect.bisect_left(numeric, value)
            up_value = numeric[min(index + 1, len(numeric) - 1)]
            down_value = numeric[max(index - 1, 0)]
            up = (sign if value < 0 else 0) if up_value == 0 else encodings[up_value]
            down = (sign if value < 0 else 0) if down_value == 0 else encodings[down_value]
        table.extend([group, flags(kind, word)])
        for result in [word & (sign - 1), word ^ sign, signum_word(kind, word), up, down]:
            table.extend(result.to_bytes(2, "little"))
        table.extend(compare(kind, word, other) + 1 for other in [0, word ^ sign, (word + 1) % 65536])
    return bytes(table)

HEADER = '''import floating;
import numeric { FloatClass };
import ordering;
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; default: Never.unreachable(); break; } }
u8 class_code(FloatClass value) { switch(value) { case FloatClass.Nan: return 0u8; break; case FloatClass.Infinite: return 1u8; break; case FloatClass.Zero: return 2u8; break; case FloatClass.Subnormal: return 3u8; break; case FloatClass.Normal: return 4u8; break; } }
'''


def exhaustive_source(directory):
    output = ["import fs;\nimport bytes;\n", HEADER]
    for kind in ["f16", "bf16"]:
        path = json.dumps(str(directory / (kind + ".bin")))
        output.append(f'''i32 exhaustive_{kind}() {{
    Vector<byte> owner = success(fs.read_bytes({path}));
    Slice<byte> expected = Slice.from_vector(&owner);
    if (Slice.len(expected) != 983040usize) return 1;
    mut usize index = 0usize;
    while (index < 65536usize) {{
        u16 word = success(Integer.convert<u16>(index));
        {kind} value = Float.from_bits<{kind}>(word);
        usize offset = index * 15usize;
        if (class_code(floating.classify(value)) != Byte.to_u8(expected[offset])) return 2;
        u8 actual_flags = (floating.is_nan(value) ? 1u8 : 0u8) | (floating.is_infinite(value) ? 2u8 : 0u8) | (floating.is_finite(value) ? 4u8 : 0u8) | (floating.is_normal(value) ? 8u8 : 0u8) | (floating.is_subnormal(value) ? 16u8 : 0u8) | (floating.is_zero(value) ? 32u8 : 0u8) | (floating.is_sign_negative(value) ? 64u8 : 0u8) | (floating.is_sign_positive(value) ? 128u8 : 0u8);
        if (actual_flags != Byte.to_u8(expected[offset + 1usize])) return 3;
        u16 absolute = success(bytes.read_u16_le(expected, offset + 2usize));
        if (Float.to_bits(floating.abs(value)) != absolute) return 4;
        if (Float.to_bits(floating.neg(value)) != success(bytes.read_u16_le(expected,offset + 4usize))) return 5;
        if (Float.to_bits(floating.signum(value)) != success(bytes.read_u16_le(expected,offset + 6usize))) return 6;
        if (Float.to_bits(floating.next_up(value)) != success(bytes.read_u16_le(expected,offset + 8usize))) return 7;
        if (Float.to_bits(floating.next_down(value)) != success(bytes.read_u16_le(expected,offset + 10usize))) return 8;
        if (Float.to_bits(floating.copy_sign(value,Float.from_bits<{kind}>(0u16))) != absolute || Float.to_bits(floating.copy_sign(value,Float.from_bits<{kind}>(32768u16))) != (absolute | 32768u16)) return 9;
        u16 adjacent = success(Integer.convert<u16>((index + 1usize) % 65536usize));
        i32 zero_order = success(Integer.convert<i32>(Byte.to_u8(expected[offset + 12usize]))) - 1;
        i32 mirror_order = success(Integer.convert<i32>(Byte.to_u8(expected[offset + 13usize]))) - 1;
        i32 adjacent_order = success(Integer.convert<i32>(Byte.to_u8(expected[offset + 14usize]))) - 1;
        if (ordering.to_i32(floating.total_cmp(value,Float.from_bits<{kind}>(0u16))) != zero_order) return 10;
        if (ordering.to_i32(floating.total_cmp(value,Float.from_bits<{kind}>(word ^ 32768u16))) != mirror_order) return 11;
        if (ordering.to_i32(floating.total_compare(value,Float.from_bits<{kind}>(adjacent))) != adjacent_order) return 12;
        index++;
    }}
    return 0;
}}
''')
    output.append("i32 main(){if(exhaustive_f16()!=0)return 1;if(exhaustive_bf16()!=0)return 2;return 0;}\n")
    return "".join(output)


def large_goldens():
    output = [HEADER]
    records = []
    random_source = random.Random(0xAE7103F10)
    for kind in ["f32", "f64"]:
        width, exponent_bits, fraction_bits = FORMATS[kind]
        sign = 1 << (width - 1)
        infinity = ((1 << exponent_bits) - 1) << fraction_bits
        words = sorted({0, sign, 1, sign | 1, (1 << fraction_bits) - 1, 1 << fraction_bits,
                        infinity - 1, sign | (infinity - 1), infinity, sign | infinity,
                        infinity | 1, infinity | (1 << (fraction_bits - 1)), infinity | (1 << (fraction_bits - 1)) | 7,
                        sign | infinity | 1, sign | infinity | (1 << (fraction_bits - 1)), (1 << width) - 1,
                        *[random_source.getrandbits(width) for _ in range(24)]})
        output.append(f"i32 goldens_{kind}() {{\n")
        for i, word in enumerate(words):
            other = words[(i * 17 + 3) % len(words)]
            group = classification(kind, word)
            up, down = rational_next(kind, word, True), rational_next(kind, word, False)
            output.append(f"    {kind} value_{i} = Float.from_bits<{kind}>({word}u{width});\n")
            output.append(f"    if(class_code(floating.classify(value_{i}))!={group}u8) return 1;\n")
            output.append(f"    u8 flags_{i} = (floating.is_nan(value_{i}) ? 1u8 : 0u8) | (floating.is_infinite(value_{i}) ? 2u8 : 0u8) | (floating.is_finite(value_{i}) ? 4u8 : 0u8) | (floating.is_normal(value_{i}) ? 8u8 : 0u8) | (floating.is_subnormal(value_{i}) ? 16u8 : 0u8) | (floating.is_zero(value_{i}) ? 32u8 : 0u8) | (floating.is_sign_negative(value_{i}) ? 64u8 : 0u8) | (floating.is_sign_positive(value_{i}) ? 128u8 : 0u8);\n")
            output.append(f"    if(flags_{i}!={flags(kind,word)}u8)return 7;\n")
            output.append(f"    if(Float.to_bits(floating.abs(value_{i}))!={word & (sign-1)}u{width} || Float.to_bits(floating.neg(value_{i}))!={word ^ sign}u{width})return 2;\n")
            output.append(f"    if(Float.to_bits(floating.signum(value_{i}))!={signum_word(kind,word)}u{width})return 3;\n")
            output.append(f"    if(Float.to_bits(floating.next_up(value_{i}))!={up}u{width} || Float.to_bits(floating.next_down(value_{i}))!={down}u{width})return 4;\n")
            output.append(f"    if(Float.to_bits(floating.copy_sign(value_{i},Float.from_bits<{kind}>({other}u{width})))!={((word & (sign-1)) | (other & sign))}u{width})return 5;\n")
            output.append(f"    if(ordering.to_i32(floating.total_cmp(value_{i},Float.from_bits<{kind}>({other}u{width})))!={compare(kind,word,other)})return 6;\n")
            records.append({"format": kind, "word": word, "class": group, "next_up": up, "next_down": down, "other": other, "comparison": compare(kind, word, other)})
        output.append("    return 0;\n}\n")
    output.append("i32 main(){if(goldens_f32()!=0)return 1;if(goldens_f64()!=0)return 2;return 0;}\n")
    return "".join(output), records


def constant_source():
    output = [HEADER, "i32 main(){\n"]
    for kind,(width,exponent,fraction) in FORMATS.items():
        sign = 1 << (width-1)
        infinity = ((1 << exponent)-1) << fraction
        values = {"EPSILON": (((1 << (exponent-1))-1)-fraction) << fraction,
                  "MIN_POSITIVE_NORMAL": 1 << fraction, "MIN_POSITIVE_SUBNORMAL": 1,
                  "MAX_VALUE": infinity-1, "MIN_VALUE": sign | (infinity-1),
                  "POSITIVE_ZERO": 0, "NEGATIVE_ZERO": sign}
        for name,word in values.items():
            output.append(f"    if(Float.to_bits(floating.{name}_{kind.upper()})!={word}u{width})return 1;\n")
        for name,word in {"infinity": infinity,"neg_infinity": sign|infinity,"nan": infinity | (1 << (fraction-1))}.items():
            output.append(f"    if(Float.to_bits(floating.{name}_{kind}())!={word}u{width})return 2;\n")
    output.append("    return 0;\n}\n")
    return "".join(output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", required=True, type=Path)
    arguments = parser.parse_args()
    directory = arguments.output_dir.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    hashes = {}
    for kind in ["f16", "bf16"]:
        table = exhaustive_table(kind)
        (directory / (kind + ".bin")).write_bytes(table)
        hashes[kind] = hashlib.sha256(table).hexdigest()
    (directory / "exhaustive.avx").write_text(exhaustive_source(directory))
    goldens, records = large_goldens()
    (directory / "golden.avx").write_text(goldens)
    (directory / "constants.avx").write_text(constant_source())
    (directory / "golden.json").write_text(json.dumps({"seed": "0xAE7103F10", "records_per_small_format": 65536,
        "record_bytes": 15, "small_table_sha256": hashes, "large_cases": records}, indent=2) + "\n")
    print(f"Floating oracle: 131072 exact small-format records, {len(records)} independent rational large-format vectors")


if __name__ == "__main__":
    main()
