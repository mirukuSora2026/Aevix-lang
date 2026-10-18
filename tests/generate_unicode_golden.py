# ========================
#
# Created By : miruku_sora2026
#
# File name : generate_unicode_golden.py
#
# Description :
#     Python 표준 UTF 코덱에서 모든 유효 스칼라의 독립적인 고정 레코드를 만든다.
#     Generate independent fixed records for every valid scalar using Python's standard UTF codecs.
#
# ========================
import argparse
import hashlib
import json
from pathlib import Path

HEADER = '''import unicode;
import fs;
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok {T value=value}:return value;break;default:Never.unreachable();break;}}
T represent<T,U>(U input){return success(Integer.convert<T>(input));}
'''


def generate(destination):
    data = bytearray()
    count = 0
    for scalar in range(0x110000):
        if 0xD800 <= scalar <= 0xDFFF:
            continue
        value = chr(scalar)
        utf8 = value.encode('utf-8')
        utf16 = value.encode('utf-16-le')
        flags = int(scalar <= 127) | (int(scalar <= 65535) << 1) | (int(scalar > 65535) << 2)
        flags |= int(scalar < 32 or 127 <= scalar <= 159) << 3
        flags |= int(0xE000 <= scalar <= 0xF8FF or 0xF0000 <= scalar <= 0xFFFFD or 0x100000 <= scalar <= 0x10FFFD) << 4
        flags |= int(0xFDD0 <= scalar <= 0xFDEF or scalar & 65535 >= 65534) << 5
        data.extend(utf8.ljust(4, b'\0'))
        data.extend(utf16.ljust(4, b'\0'))
        data.extend((len(utf8), len(utf16) // 2, flags))
        count += 1
    payload = destination / 'scalars.bin'
    payload.write_bytes(data)
    source = HEADER + 'i32 main(){\n    Vector<byte> data=success(fs.read_bytes(' + json.dumps(str(payload)) + '));\n'
    source += '''    Slice<byte> oracle=Slice.from_vector(&data);
    if(Slice.len(oracle)!=12232704usize)return 90;
    mut u32 raw=0u32;mut usize at=0usize;
    while(raw<=1114111u32){
        if(raw<55296u32||raw>57343u32){
            char value=success(unicode.from_u32(raw));
            if(unicode.to_u32(value)!=raw||unicode.to_u32(success(unicode.from_utf32(raw)))!=raw)return 1;
            Tuple<Array<byte,4>,u32> eight=unicode.to_utf8(value);
            Tuple<Array<u16,2>,u32> sixteen=unicode.to_utf16(value);
            &Array<byte,4> encoded8=Tuple.get<0>(&eight);
            &Array<u16,2> encoded16=Tuple.get<0>(&sixteen);
            u32 length8=*Tuple.get<1>(&eight);u32 length16=*Tuple.get<1>(&sixteen);
            if(length8!=represent<u32>(Byte.to_u8(oracle[at+8usize]))||length16!=represent<u32>(Byte.to_u8(oracle[at+9usize])))return 2;
            if(length8!=unicode.utf8_length(value)||length16!=unicode.utf16_length(value))return 3;
            mut usize i=0usize;
            while(i<4usize){if((*encoded8)[i]!=oracle[at+i])return 4;i++;}
            mut usize j=0usize;
            while(j<2usize){u16 expected=represent<u16>(Byte.to_u8(oracle[at+4usize+j*2usize]))|(represent<u16>(Byte.to_u8(oracle[at+5usize+j*2usize]))<<8);if((*encoded16)[j]!=expected)return 5;j++;}
            Slice<byte> prefix8=success(Slice.slice(Slice.from_array(encoded8),0usize,represent<usize>(length8)));
            Slice<u16> prefix16=success(Slice.slice(Slice.from_array(encoded16),0usize,represent<usize>(length16)));
            if(unicode.to_u32(success(unicode.from_utf8(prefix8)))!=raw||unicode.to_u32(success(unicode.from_utf16(prefix16)))!=raw)return 6;
            if(success(unicode.validate_utf8_prefix(prefix8))!=represent<usize>(length8)||success(unicode.validate_utf16_prefix(prefix16))!=represent<usize>(length16))return 7;
            u8 expected_flags=Byte.to_u8(oracle[at+10usize]);mut u8 flags=0u8;
            if(unicode.is_ascii(value))flags|=1u8;if(unicode.is_bmp(value))flags|=2u8;if(unicode.is_supplementary(value))flags|=4u8;
            if(unicode.is_control(value))flags|=8u8;if(unicode.is_private_use(value))flags|=16u8;if(unicode.is_noncharacter(value))flags|=32u8;
            if(flags!=expected_flags||!unicode.is_valid_scalar(raw))return 8;
            at+=11usize;
        }else if(unicode.is_valid_scalar(raw))return 9;
        raw++;
    }
    return at==Slice.len(oracle)?0:10;
}
'''
    (destination / 'exhaustive.avx').write_text(source)
    metadata = {'records': count, 'bytes': len(data), 'record_width': 11, 'sha256': hashlib.sha256(data).hexdigest(),
                'oracle': 'Python chr.encode UTF8/UTF16LE; explicit fixed Unicode17 ranges for Cc/Co/noncharacters, no UCD table or host locale.',
                'scope': 'Scalar validity, exact encodings, zero padding, lengths, scalar range predicates and roundtrips; not Unicode property/case/segmentation proof.'}
    (destination / 'oracle.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(f'Unicode scalar oracle: {count} independently encoded records, {len(data)} bytes')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    generate(args.output_dir)
