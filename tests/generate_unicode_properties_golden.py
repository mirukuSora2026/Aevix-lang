# ========================
#
# Created By : miruku_sora2026
#
# File name : generate_unicode_properties_golden.py
#
# Description :
#     압축 테이블 생성기와 별개로 원본 UCD를 펼쳐 모든 스칼라의 실행 기준값을 만든다.
#     Independently expand original UCD inputs into execution goldens for every scalar.
#
# ========================
from pathlib import Path
from fractions import Fraction
import array
import argparse
import hashlib
import json
import re
import struct

PROJECT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description='Independently expand pinned offline Unicode property/case/name goldens.')
parser.add_argument('--input-dir', type=Path, required=True)
parser.add_argument('--metadata', type=Path, required=True)
parser.add_argument('--output-dir', type=Path, required=True)
parser.add_argument('--module', default='unicode')
args = parser.parse_args()
BASE = args.output_dir.resolve()
ROOT = args.input_dir
OUT = BASE
OUT.mkdir(parents=True, exist_ok=True)
SIZE = 0x110000
META = json.loads(args.metadata.read_text(encoding='utf-8'))
manifest = json.loads((ROOT / 'manifest.json').read_text(encoding='utf-8'))
for entry in manifest['files']:
    if hashlib.sha256((ROOT / entry['path']).read_bytes()).hexdigest() != entry['sha256']:
        raise SystemExit('Pinned oracle input hash mismatch: ' + entry['path'])


def entries(path):
    for line in (ROOT / path).read_text('utf-8-sig').splitlines():
        line = line.partition('#')[0].strip()
        if line:
            yield [field.strip() for field in line.split(';')]


def bounds(text):
    match = re.fullmatch(r'([0-9A-F]+)(?:\.\.([0-9A-F]+))?', text)
    return int(match[1], 16), int(match[2] or match[1], 16)


def fill(data, interval, value):
    start, end = interval
    for point in range(start, end + 1):
        data[point] = value


alias = {}
for fields in entries('PropertyValueAliases.txt'):
    if len(fields) >= 3:
        for name in fields[1:]:
            alias[fields[0], name] = fields[2]


def official(prop, name):
    return alias.get((prop, name), name)


def ranged(path, prop, fallback, column=1, selection=None):
    default = official(prop, fallback)
    data = [default] * SIZE
    for line in (ROOT / path).read_text('utf-8-sig').splitlines():
        if '@missing:' not in line:
            continue
        fields = [part.strip() for part in line.partition('@missing:')[2].split(';')]
        if selection and fields[1] != selection:
            continue
        fill(data, bounds(fields[0]), official(prop, fields[-1]))
    for fields in entries(path):
        if selection and fields[1] != selection:
            continue
        fill(data, bounds(fields[0]), official(prop, fields[column]))
    return data


db = {}
records = list(entries('UnicodeData.txt'))
cat = ['Cn'] * SIZE
ccc = array.array('I', [0]) * SIZE
digit = array.array('I', [10]) * SIZE
names = {}
simple = {key: array.array('I', range(SIZE)) for key in ('lowercase', 'uppercase', 'titlecase', 'case_fold')}
algorithmic = []
first = None
for fields in records:
    point = int(fields[0], 16)
    if fields[1].endswith(', First>'):
        first = fields
        continue
    low = int(first[0], 16) if first else point
    fill(cat, (low, point), fields[2])
    fill(ccc, (low, point), int(fields[3]))
    if fields[6]:
        fill(digit, (low, point), int(fields[6]))
    if first:
        if first[1].startswith('<CJK Ideograph'):
            algorithmic.append((low, point, 'CJK UNIFIED IDEOGRAPH-'))
        elif first[1].startswith('<Tangut Ideograph'):
            algorithmic.append((low, point, 'TANGUT IDEOGRAPH-'))
        first = None
    elif not fields[1].startswith('<'):
        names[point] = fields[1]
    for key, position in [('uppercase', 12), ('lowercase', 13), ('titlecase', 14)]:
        chosen = fields[position] or (fields[12] if key == 'titlecase' else '')
        if chosen:
            simple[key][point] = int(chosen, 16)

cat_names = sorted(set(cat))
db['general_category'] = [cat_names.index(value) for value in cat]
db['combining_class'] = ccc
db['decimal_value'] = digit
specs = [
    ('bidi_class','extracted/DerivedBidiClass.txt','bc','L'),
    ('east_asian_width','EastAsianWidth.txt','ea','N'),
    ('script','Scripts.txt','sc','Unknown'),
    ('grapheme_break','auxiliary/GraphemeBreakProperty.txt','GCB','Other'),
    ('word_break','auxiliary/WordBreakProperty.txt','WB','Other'),
    ('sentence_break','auxiliary/SentenceBreakProperty.txt','SB','Other'),
    ('line_break','LineBreak.txt','lb','XX'),
]
for name, path, prop, default in specs:
    values = ranged(path, prop, default)
    vocabulary = sorted(set(values))
    # This is semantic vocabulary, not data decoded from generated packed tables.
    assert vocabulary == META['properties'][name]['spellings']
    db[name] = array.array('I', [vocabulary.index(value) for value in values])

blocks = sorted({line[1] for line in entries('Blocks.txt')})
db['block'] = array.array('I', [0]) * SIZE
for fields in entries('Blocks.txt'):
    fill(db['block'], bounds(fields[0]), blocks.index(fields[1]) + 1)
ages = ranged('DerivedAge.txt', 'age', 'Unassigned')
db['age'] = array.array('I', [0 if value == 'Unassigned' else int(value.split('_')[0][1:]) * 256 + int(value.split('_')[1]) for value in ages])
for name, prop in [('nfc_quick_check','NFC_QC'),('nfd_quick_check','NFD_QC'),('nfkc_quick_check','NFKC_QC'),('nfkd_quick_check','NFKD_QC')]:
    raw = ranged('DerivedNormalizationProps.txt', prop, 'Yes', column=2, selection=prop)
    db[name] = array.array('I', [{'N':0,'No':0,'M':1,'Maybe':1,'Y':2,'Yes':2}[value] for value in raw])

booleans = ['is_alphabetic','is_case_ignorable','is_cased','is_default_ignorable','is_join_control','is_lowercase','is_uppercase','is_variation_selector','is_whitespace','is_xid_continue','is_xid_start','is_assigned','is_unassigned','is_decimal_digit','is_format','is_letter','is_mark','is_letter_or_number','is_numeric']
ucd_flags = ['Alphabetic','Case_Ignorable','Cased','Default_Ignorable_Code_Point','Join_Control','Lowercase','Uppercase','Variation_Selector','White_Space','XID_Continue','XID_Start']
flags = array.array('I', [0]) * SIZE
for path in ['DerivedCoreProperties.txt','PropList.txt']:
    for fields in entries(path):
        if fields[1] not in ucd_flags:
            continue
        bit = 1 << ucd_flags.index(fields[1])
        start, end = bounds(fields[0])
        for point in range(start, end + 1):
            flags[point] |= bit
for point, value in enumerate(cat):
    tests = [value != 'Cn', value == 'Cn', value == 'Nd', value == 'Cf', value.startswith('L'), value.startswith('M'), value.startswith(('L','N'))]
    for index, enabled in enumerate(tests, 11):
        flags[point] |= int(enabled) << index

numeric = {}
for fields in entries('extracted/DerivedNumericValues.txt'):
    start, end = bounds(fields[0]); rational = Fraction(fields[3])
    for point in range(start, end + 1):
        numeric[point] = rational
        flags[point] |= 1 << 18

script_names = META['properties']['script']['spellings']
script_extensions = {}
for fields in entries('ScriptExtensions.txt'):
    group = tuple(sorted(script_names.index(official('sc', value)) for value in fields[1].split()))
    start, end = bounds(fields[0])
    for point in range(start, end + 1):
        script_extensions[point] = group

full = {key: {} for key in simple}
for fields in entries('CaseFolding.txt'):
    point = int(fields[0], 16)
    sequence = [int(value, 16) for value in fields[2].split()]
    if fields[1] in ['C','S']:
        assert len(sequence) == 1
        simple['case_fold'][point] = sequence[0]
    if fields[1] in ['C','F']:
        full['case_fold'][point] = sequence
for key in full:
    for point, mapped in enumerate(simple[key]):
        if point != mapped and point not in full[key]:
            full[key][point] = [mapped]
for fields in entries('SpecialCasing.txt'):
    if len(fields) > 4 and fields[4]:
        continue
    point = int(fields[0], 16)
    for key, position in [('lowercase',1),('titlecase',2),('uppercase',3)]:
        full[key][point] = [int(value, 16) for value in fields[position].split()]

# Independently reconstruct Hangul names using the normative decomposition indexes.
jamo = {int(fields[0], 16):fields[1] for fields in entries('Jamo.txt')}
for leading in range(19):
    for vowel in range(21):
        for final in range(28):
            point = 0xAC00 + leading * 588 + vowel * 28 + final
            names[point] = 'HANGUL SYLLABLE ' + jamo[0x1100+leading] + jamo[0x1161+vowel] + (jamo[0x11A7+final] if final else '')
for low, high, prefix in algorithmic:
    for point in range(low, high + 1):
        names[point] = prefix + format(point, '04X')
aliases = {}
for fields in entries('NameAliases.txt'):
    aliases.setdefault(int(fields[0], 16), []).append(fields[1])

header = '''import unicode_preview;
import fs;
import system;
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok {T value=value}:return value;break;default:Never.unreachable();break;}}
T represent<T,U>(U input){return success(Integer.convert<T>(input));}
u32 read32(Slice<byte> data,usize offset){return represent<u32>(Byte.to_u8(data[offset]))|(represent<u32>(Byte.to_u8(data[offset+1usize]))<<8)|(represent<u32>(Byte.to_u8(data[offset+2usize]))<<16)|(represent<u32>(Byte.to_u8(data[offset+3usize]))<<24);}
u64 read64(Slice<byte> data,usize offset){return represent<u64>(read32(data,offset))|(represent<u64>(read32(data,offset+4usize))<<32);}
bool equal_bytes(Slice<byte> actual,Slice<byte> expected,usize start,usize length){if(Slice.len(actual)!=length)return false;mut usize at=0usize;while(at<length){if(actual[at]!=expected[start+at])return false;at++;}return true;}
'''
helpers = ''
for name, variants in META['enums'].items():
    helpers += 'u32 id_' + name + '(unicode_preview.' + name + ' value){switch(value){\n'
    for index, variant in enumerate(variants):
        helpers += 'case unicode_preview.' + name + '.' + variant + ':return ' + str(index) + 'u32;break;\n'
    helpers += '}}\n'

property_order = ['general_category','combining_class','decimal_value','bidi_class','east_asian_width','script','grapheme_break','word_break','sentence_break','line_break','block','age','nfc_quick_check','nfd_quick_check','nfkc_quick_check','nfkd_quick_check']
payload = bytearray()
text = bytearray()
count = 0
for point in range(SIZE):
    if 0xD800 <= point <= 0xDFFF:
        continue
    count += 1
    for name in property_order:
        payload.extend(struct.pack('<I', db[name][point]))
    payload.extend(struct.pack('<I', flags[point]))
    fraction = numeric.get(point)
    payload.extend(struct.pack('<qQ', fraction.numerator if fraction is not None else 0, fraction.denominator if fraction is not None else 0))
    group = script_extensions.get(point, (db['script'][point],))
    payload.append(len(group))
    payload.extend(bytes(group))
    for key in simple:
        payload.extend(struct.pack('<I', simple[key][point]))
    for key in full:
        mapping = full[key].get(point, [point])
        encoded = ''.join(chr(value) for value in mapping).encode('utf-8')
        text.append(len(encoded)); text.extend(encoded)
    canonical_name = names.get(point, '').encode('ascii')
    assert len(canonical_name) < 256
    text.append(len(canonical_name)); text.extend(canonical_name)
    aliases_here = aliases.get(point, [])
    text.append(len(aliases_here))
    for alias_name in aliases_here:
        encoded = alias_name.encode('ascii'); text.append(len(encoded)); text.extend(encoded)
(BASE / 'properties.bin').write_bytes(payload)
(BASE / 'text.bin').write_bytes(text)

source = header + helpers + 'i32 main(){\n'
source += 'Vector<byte> data=success(fs.read_bytes(' + json.dumps(str(BASE / 'properties.bin')) + '));Slice<byte> expected=Slice.from_vector(&data);mut usize at=0usize;mut u32 point=0u32;\n'
source += 'while(point<=1114111u32){if(point<55296u32||point>57343u32){char c=success(Char.from_u32(point));\n'
for name in property_order:
    carrier = META['properties'][name]['type']
    if carrier in META['enums']:
        actual = 'id_' + carrier + '(unicode_preview.' + name + '(c))'
        source += 'if(' + actual + '!=read32(expected,at)){system.println("' + name + ' "+point);return 1;}at+=4usize;\n'
    elif name == 'combining_class':
        source += 'if(represent<u32>(unicode_preview.combining_class(c))!=read32(expected,at)){system.println("ccc "+point);return 2;}at+=4usize;\n'
    elif name == 'decimal_value':
        source += 'mut u32 digit=10u32;switch(unicode_preview.decimal_value(c)){case Optional<u8>.Some {u8 found=value}:digit=represent<u32>(found);break;case Optional<u8>.None:break;}if(digit!=read32(expected,at)){system.println("decimal "+point);return 3;}at+=4usize;\n'
    elif name == 'block':
        source += 'mut u32 block=0u32;switch(unicode_preview.block(c)){case Optional<unicode_preview.UnicodeBlock>.Some {unicode_preview.UnicodeBlock found=value}:block=id_UnicodeBlock(found)+1u32;break;case Optional<unicode_preview.UnicodeBlock>.None:break;}if(block!=read32(expected,at)){system.println("block "+point);return 4;}at+=4usize;\n'
    elif name == 'age':
        source += 'mut u32 version=0u32;switch(unicode_preview.age(c)){case Optional<unicode_preview.UnicodeVersion>.Some {unicode_preview.UnicodeVersion found=value}:version=represent<u32>(found.major)*256u32+represent<u32>(found.minor);if(found.patch!=0u8)return 5;break;case Optional<unicode_preview.UnicodeVersion>.None:break;}if(version!=read32(expected,at)){system.println("age "+point);return 6;}at+=4usize;\n'
source += 'mut u32 flags=0u32;\n'
for index, name in enumerate(booleans):
    source += 'if(unicode_preview.' + name + '(c))flags|=' + str(1 << index) + 'u32;\n'
source += 'if(flags!=read32(expected,at)){system.println("flags "+point);return 7;}at+=4usize;\n'
source += '''u64 numerator=read64(expected,at);u64 denominator=read64(expected,at+8usize);at+=16usize;
switch(unicode_preview.numeric_value(c)){case Optional<unicode_preview.UnicodeNumeric>.Some {unicode_preview.UnicodeNumeric found=value}:if(denominator==0u64||found.denominator!=denominator){system.println("numeric "+point);return 8;}if(found.numerator<0i64){if(numerator!=18446744073709551615u64||found.numerator!=-1i64)return 9;}else if(represent<u64>(found.numerator)!=numerator)return 10;break;case Optional<unicode_preview.UnicodeNumeric>.None:if(denominator!=0u64)return 11;break;}
usize group_size=represent<usize>(Byte.to_u8(expected[at]));at++;
Vector<unicode_preview.Script> scripts=success(unicode_preview.script_extensions(c));if(Vector.len(&scripts)!=group_size){system.println("script_extensions "+point);return 12;}
mut usize item=0usize;while(item<group_size){if(id_Script(scripts[item])!=represent<u32>(Byte.to_u8(expected[at+item])))return 13;item++;}at+=group_size;
'''
for key in simple:
    source += 'if(Char.to_u32(unicode_preview.simple_' + key + '(c))!=read32(expected,at)){system.println("simple_' + key + ' "+point);return 14;}at+=4usize;\n'
source += '}point++;}return at==Slice.len(expected)?0:15;}\n'
(OUT / 'properties_full.avx').write_bytes(source.replace('unicode_preview', args.module).encode('utf-8'))

source = header + 'i32 main(){\n'
source += 'Vector<byte> data=success(fs.read_bytes(' + json.dumps(str(BASE / 'text.bin')) + '));Slice<byte> expected=Slice.from_vector(&data);mut usize at=0usize;mut u32 point=0u32;\n'
source += 'while(point<=1114111u32){if(point<55296u32||point>57343u32){char c=success(Char.from_u32(point));\n'
for key in full:
    api = 'case_fold' if key == 'case_fold' else 'to_' + key
    source += '{usize length=represent<usize>(Byte.to_u8(expected[at]));at++;string actual=success(unicode_preview.' + api + '(c));if(!equal_bytes(string.as_bytes(&actual),expected,at,length)){system.println("' + api + ' "+point);return 1;}at+=length;}\n'
source += '''usize name_length=represent<usize>(Byte.to_u8(expected[at]));at++;
Optional<string> actual_name=success(unicode_preview.name(c));
switch(move actual_name){case Optional<string>.Some {string actual=value}:if(name_length==0usize||!equal_bytes(string.as_bytes(&actual),expected,at,name_length)){system.println("name "+point);return 2;}if(success(unicode_preview.lookup_name(string.view(&actual)))!=c){system.println("lookup_name "+point);return 3;}break;case Optional<string>.None:if(name_length!=0usize){system.println("missing name "+point);return 4;}break;}at+=name_length;
usize alias_count=represent<usize>(Byte.to_u8(expected[at]));at++;
Vector<string> actual_aliases=success(unicode_preview.name_aliases(c));if(Vector.len(&actual_aliases)!=alias_count)return 5;
mut usize item=0usize;while(item<alias_count){usize length=represent<usize>(Byte.to_u8(expected[at]));at++;if(!equal_bytes(string.as_bytes(&actual_aliases[item]),expected,at,length)){system.println("alias "+point);return 6;}if(success(unicode_preview.lookup_name(string.view(&actual_aliases[item])))!=c)return 7;at+=length;item++;}
}point++;}return at==Slice.len(expected)?0:8;}
'''
(OUT / 'text_full.avx').write_bytes(source.replace('unicode_preview', args.module).encode('utf-8'))

summary = {'scalar_count': count, 'properties_bytes':len(payload), 'text_bytes':len(text), 'named_scalars':len(names), 'alias_entries':sum(map(len, aliases.values())),
           'properties_sha256':hashlib.sha256(payload).hexdigest(), 'text_sha256':hashlib.sha256(text).hexdigest(),
           'policy':'Independent direct expansion of official pinned UCD files; no generated packed table is decoded.'}
(BASE / 'oracle-manifest.json').write_bytes((json.dumps(summary,indent=2)+'\n').encode('utf-8'))
print(json.dumps(summary))
