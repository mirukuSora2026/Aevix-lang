#!/usr/bin/env python3
# ========================
#
# Created By : miruku_sora2026
#
# File name : generate_float_bits_golden.py
#
# Description :
#     Python struct가 정한 비트·숫자 기준값을 결정적인 난수와 경계값으로 생성한다.
#     Generate independent Python struct bit/numeric goldens with deterministic random and boundary cases.
#
# ========================
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import struct


def generate():
    random_source = random.Random(0xAE7103B17)
    output = ["/* Independent Python struct numeric/bit goldens; no host byte-order assumption. */\n"]
    calls, records = [], []
    numeric_count = 0
    for width, code in [(32, "f"), (64, "d")]:
        fraction = 23 if width == 32 else 52
        exponent = 8 if width == 32 else 11
        sign = 1 << (width - 1)
        infinity = ((1 << exponent) - 1) << fraction
        words = sorted({0, sign, 1, sign | 1, (1 << fraction) - 1,
                        1 << fraction, sign | (1 << fraction), infinity - 1,
                        sign | (infinity - 1), infinity, sign | infinity,
                        infinity | 1, infinity | (1 << (fraction - 1)),
                        infinity | (1 << (fraction - 1)) | 123,
                        sign | infinity | 1, (1 << width) - 1,
                        *[random_source.getrandbits(width) for _ in range(24)]})
        output.append(f"i32 golden_{width}() {{\n")
        for word in words:
            floating = struct.unpack(">" + code, word.to_bytes(width // 8, "big"))[0]
            output.append(f"    if (Float.to_bits(Float.from_bits<f{width}>({word}u{width})) != {word}u{width}) return 1;\n")
            kind = "finite" if math.isfinite(floating) else "nan" if math.isnan(floating) else "infinity"
            record = {"width": width, "word": word, "kind": kind}
            if math.isfinite(floating):
                numeric_count += 1
                literal = repr(floating)
                record["literal"] = literal
                output.append(f"    if (Float.to_bits({literal}f{width}) != {word}u{width}) return 2;\n")
                output.append(f"    if (Float.from_bits<f{width}>({word}u{width}) != {literal}f{width}) return 3;\n")
            else:
                # NaN classification is an independent numeric check; infinity is checked against signed max finite.
                if math.isnan(floating):
                    output.append(f"    f{width} value_{word} = Float.from_bits<f{width}>({word}u{width});\n")
                    output.append(f"    if (!(value_{word} != value_{word})) return 4;\n")
                else:
                    maximum = struct.unpack(">" + code, (infinity - 1).to_bytes(width // 8, "big"))[0]
                    compare = "<" if word & sign else ">"
                    bound = "-" if word & sign else ""
                    output.append(f"    if (!(Float.from_bits<f{width}>({word}u{width}) {compare} {bound}{repr(maximum)}f{width})) return 5;\n")
            records.append(record)
        output.append("    return 0;\n}\n")
        calls.append(f"    if (golden_{width}() != 0) return {width};\n")
    output.append("i32 main() {\n" + "".join(calls) + "    return 0;\n}\n")
    return "".join(output), records, numeric_count


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    arguments = parser.parse_args()
    source, records, numeric_count = generate()
    arguments.output.write_text(source, encoding="utf-8")
    arguments.metadata.write_text(json.dumps({"seed": "0xAE7103B17", "sha256": hashlib.sha256(source.encode()).hexdigest(),
                                              "raw_word_cases": len(records), "finite_numeric_cases": numeric_count,
                                              "cases": records}, indent=2) + "\n", encoding="utf-8")
    print(f"Independent Python struct oracle: {len(records)} raw words, {numeric_count} finite numeric goldens")


if __name__ == "__main__":
    main()
