# ========================
#
# Created By : miruku_sora2026
#
# File name : generate_encoding_bytes_golden.py
#
# Description :
#     독립 Python 코덱으로 바이트 순서별 직렬화와 텍스트 복원의 기준값을 생성한다.
#     Generate endian serialization and owned-text recovery goldens with independent Python codecs.
#
# ========================
import argparse
import json
import random
from pathlib import Path


def digest(values):
    state = 0
    for value in values:
        state = (state * 257 + value + 17) % 4294967291
    return state


def reference_scalars():
    values = list(range(256)) + [0x7ff, 0x800, 0xd7ff, 0xe000, 0xffff, 0x10000, 0x10ffff,
                                0xfeff, 0xfffe, 0xfdd0, 0x378, 0x1f600, 0x301, 0x10fffe]
    rng = random.Random(0xAE713)
    while len(values) < 1024:
        value = rng.randrange(0x110000)
        if not 0xd800 <= value <= 0xdfff:
            values.append(value)
    return values


def generate():
    values = reference_scalars()
    functions, calls, records = [], [], []
    header = r'''import encoding;
T success<T>(Result<T, encoding.EncodingError> input) {
    switch (move input) { case Result<T, encoding.EncodingError>.Ok { T found = value }: return found; break; default: Never.unreachable(); break; }
}
u64 integer(u8 input) {
    Result<u64, IntegerError> result = Integer.convert<u64>(input);
    switch (move result) { case Result<u64, IntegerError>.Ok { u64 value = value }: return value; break; default: Never.unreachable(); break; }
}
u64 digest(Slice<byte> bytes) {
    mut u64 state = 0; mut usize index = 0;
    while (index < Slice.len(bytes)) { state = (state * 257u64 + integer(Byte.to_u8(bytes[index])) + 17u64) % 4294967291u64; index++; }
    return state;
}
'''
    for index in range(0, len(values), 128):
        batch = values[index:index + 128]
        text = ''.join(map(chr, batch))
        literal = ''.join('\\u{' + format(value, 'x') + '}' for value in batch)
        label = f'corpus_{index // 128}'
        lines = [f'i32 {label}() {{', f'    string_view input = "{literal}";']
        lines.append(f'    if (encoding.utf8_len(input) != {len(text.encode("utf-8"))} || encoding.utf16_len(input) != {len(text.encode("utf-16-le")) // 2} || encoding.utf32_len(input) != {len(batch)}) return 1;')
        lines += ['    Vector<byte> utf8 = success(encoding.encode_utf8(input));',
                  f'    if (digest(Slice.from_vector(&utf8)) != {digest(text.encode("utf-8"))}u64) return 21;',
                  '    string text8 = success(encoding.decode_utf8(Slice.from_vector(&utf8)));',
                  '    if (!string.equals(string.view(&text8), input)) return 22;',
                  '    Vector<u16> utf16 = success(encoding.encode_utf16(input));',
                  '    string text16 = success(encoding.decode_utf16(Slice.from_vector(&utf16)));',
                  '    if (!string.equals(string.view(&text16), input)) return 23;']
        for number, (codec, operation) in enumerate([('utf-16-le','utf16_le'), ('utf-16-be','utf16_be'), ('utf-32-le','utf32_le'), ('utf-32-be','utf32_be')], 2):
            data = text.encode(codec, errors='strict')
            lines += [f'    Vector<byte> encoded_{operation} = success(encoding.encode_{operation}(input));',
                      f'    if (Vector.len(&encoded_{operation}) != {len(data)} || digest(Slice.from_vector(&encoded_{operation})) != {digest(data)}u64) return {number};',
                      f'    string decoded_{operation} = success(encoding.decode_{operation}(Slice.from_vector(&encoded_{operation})));',
                      f'    if (!string.equals(string.view(&decoded_{operation}), input)) return {number + 10};']
            records.append({'batch': label, 'codec': codec, 'bytes':len(data), 'digest':digest(data)})
        lines += ['    Vector<u32> units = success(encoding.encode_utf32(input));',
                  '    string restored = success(encoding.decode_utf32(Slice.from_vector(&units)));',
                  f'    if (Vector.len(&units) != {len(batch)} || !string.equals(string.view(&restored), input)) return 20;',
                  '    return 0;', '}']
        functions.append('\n'.join(lines))
        calls.append(f'    if ({label}() != 0) return {index // 128 + 1};')
    main = '\n'.join(['i32 main() {'] + calls + ['    return 0;', '}'])
    return header + '\n'.join(functions) + '\n' + main + '\n', {'scalar_samples':len(values), 'seed':'0xAE713', 'codec_goldens':records}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--metadata', type=Path, required=True)
    args = parser.parse_args()
    source, metadata = generate()
    args.output.write_text(source, encoding='utf-8')
    args.metadata.write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    print(f'{metadata["scalar_samples"]} scalar samples, {len(metadata["codec_goldens"])} independent endian codec goldens')
