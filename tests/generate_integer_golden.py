# ========================
#
# Created By : miruku_sora2026
#
# File name : generate_integer_golden.py
#
# Description :
#     Python 정밀 정수 계산으로 정수 모듈의 독립 오라클을 생성한다.
#     Generate independent integer-module oracles using Python arbitrary-precision arithmetic.
#
# ========================
from pathlib import Path
import argparse
import hashlib
import json
import random

parser=argparse.ArgumentParser(description='Independent integer arithmetic and representation oracle.')
parser.add_argument('--output-dir',type=Path,required=True)
parser.add_argument('--metadata',type=Path)
args=parser.parse_args()
OUTPUT=args.output_dir.resolve()
OUTPUT.mkdir(parents=True,exist_ok=True)
TYPES=[(f'{p}{w}',w,p=='i') for w in (8,16,32,64,128) for p in ('i','u')]+[('isize',64,True),('usize',64,False)]
PRIMES=(4294967291,4294967279)
BASE=65599
HEADER='''import integer;
T success<T,E>(Result<T,E> input) {
    switch (move input) { case Result<T,E>.Ok { T output = value }: return output; break; default: Never.unreachable(); break; }
}
u64 encode<T>(T value, u64 prime) where T implements Copy {
    u128 modulus = success<u128,IntegerError>(Integer.convert<u128>(prime));
    switch (Integer.convert<u128>(value)) {
        case Result<u128,IntegerError>.Ok { u128 output = value }:
            u128 reduced = output % modulus;
            u64 result = success<u64,IntegerError>(Integer.convert<u64>(reduced));
            return result;
            break;
        default:
            i128 negative = success<i128,IntegerError>(Integer.convert<i128>(value));
            u128 predecessor = success<u128,IntegerError>(Integer.convert<u128>(-(negative + 1i128)));
            u128 magnitude = predecessor + 1u128;
            u128 reduced = magnitude % modulus;
            u64 residue = success<u64,IntegerError>(Integer.convert<u64>(reduced));
            return residue == 0u64 ? 0u64 : prime - residue;
            break;
    }
}
void feed(&mut u64 hash, u64 token, u64 prime) { *hash = (*hash * 65599u64 + token) % prime; return; }
void feed_value<T>(&mut u64 hash, T value, u64 prime) where T implements Copy { feed(hash, 1u64, prime); feed(hash, encode(value, prime), prime); return; }
void feed_bool(&mut u64 hash, bool value, u64 prime) { feed_value(hash, value ? 1u64 : 0u64, prime); return; }
void feed_result<T>(&mut u64 hash, Result<T,integer.IntegerArithmeticError> value, u64 prime) where T implements Copy {
    switch (move value) {
        case Result<T,integer.IntegerArithmeticError>.Ok { T output = value }: feed_value(hash, output, prime); break;
        case Result<T,integer.IntegerArithmeticError>.Err { integer.IntegerArithmeticError reason = error }:
            switch (reason) {
                case integer.IntegerArithmeticError.Overflow: feed(hash, 2u64, prime); feed(hash, 0u64, prime); break;
                case integer.IntegerArithmeticError.DivisionByZero: feed(hash, 3u64, prime); feed(hash, 0u64, prime); break;
            }
            break;
    }
    return;
}
void feed_pair<T>(&mut u64 hash, Tuple<T,bool> value, u64 prime) where T implements Copy { feed_value(hash,*Tuple.get<0>(&value),prime); feed_bool(hash,*Tuple.get<1>(&value),prime); return; }
'''

def normal(v,w,signed):
    v%=1<<w
    return v-(1<<w) if signed and v>=(1<<(w-1)) else v

def clamp(v,lo,hi): return max(lo,min(hi,v))
def checked(v,lo,hi): return (1,v) if lo<=v<=hi else (2,0)
def trunc(a,b): return (abs(a)//abs(b))*(-1 if (a<0)!=(b<0) else 1)

def pair_values(a,b,w,signed):
    lo=-(1<<(w-1)) if signed else 0
    hi=(1<<(w-1))-1 if signed else (1<<w)-1
    math=[a+b,a-b,a*b]
    div=(3,0) if b==0 else checked(trunc(a,b),lo,hi)
    rem=(3,0) if b==0 else (1,a-trunc(a,b)*b)
    out=[checked(v,lo,hi) for v in math]+[div,rem]
    out += [(1,normal(v,w,signed)) for v in math]
    out += [(1,clamp(v,lo,hi)) for v in math]
    for v in math: out += [(1,normal(v,w,signed)),(1,int(not(lo<=v<=hi)))]
    out += [(1,abs(a-b)),(1,int(a==0 if b==0 else a%b==0))]
    return out

def unary_values(a,w,signed):
    raw=a% (1<<w)
    mask=(1<<w)-1
    count=raw.bit_count()
    leading=w-raw.bit_length()
    trailing=w if raw==0 else (raw&-raw).bit_length()-1
    complement=raw^mask
    leading_ones=w-complement.bit_length()
    trailing_ones=w if complement==0 else (complement&-complement).bit_length()-1
    reverse=int(f'{raw:0{w}b}'[::-1],2)
    swap=int.from_bytes(raw.to_bytes(w//8,'little'),'big')
    out=[(1,int(a==0)),(1,int(a<0)),(1,int(a>0)),(1,int(a%2==0)),(1,int(a%2!=0)),(1,int(a>0 and a&(a-1)==0))]
    out += [(1,v) for v in (count,w-count,leading,trailing,leading_ones,trailing_ones,abs(a).bit_length(),normal(reverse,w,signed),normal(swap,w,signed),0 if raw==0 else 1<<(raw.bit_length()-1),raw&-raw)]
    if signed:
        lo=-(1<<(w-1));hi=(1<<(w-1))-1
        out += [(1,abs(a)),checked(-a,lo,hi),checked(abs(a),lo,hi),(1,normal(abs(a),w,True)),(1,clamp(-a,lo,hi)),(1,clamp(abs(a),lo,hi)),(1,normal(-a,w,True)),(1,int(a==lo)),(1,normal(abs(a),w,True)),(1,int(a==lo))]
    out += [(1,normal(-a,w,signed))]
    return out

def power_values(a,e,w,signed):
    lo=-(1<<(w-1)) if signed else 0
    hi=(1<<(w-1))-1 if signed else (1<<w)-1
    # Huge exponents are bounded by width unless the base is zero or a unit.
    exact=pow(a,e) if abs(a)<=1 or e<=w+1 else (lo-1 if a<0 and e%2 else hi+1)
    overflow=not(lo<=exact<=hi)
    wrapped=normal(pow(a,e,1<<w),w,signed)
    return [checked(exact,lo,hi),(1,wrapped),(1,clamp(exact,lo,hi)),(1,wrapped),(1,int(overflow))]

def rotate_values(a,count,w,signed):
    raw=a%(1<<w);amount=count%w;mask=(1<<w)-1
    l=raw if amount==0 else ((raw<<amount)|(raw>>(w-amount)))&mask
    r=raw if amount==0 else ((raw>>amount)|(raw<<(w-amount)))&mask
    return [(1,normal(l,w,signed)),(1,normal(r,w,signed))]

def digest(values,prime):
    h=17
    for status,value in values:
        h=(h*BASE+status)%prime
        h=(h*BASE+(value%prime if status==1 else 0))%prime
    return h

def avx_array(t,name,values):
    return f'{t} {name}[{len(values)}] = ['+', '.join(str(v)+t for v in values)+'];\n'

PAIR_CODE='''
feed_result(&mut hash, integer.checked_add(a,b),prime);
feed_result(&mut hash, integer.checked_sub(a,b),prime);
feed_result(&mut hash, integer.checked_mul(a,b),prime);
feed_result(&mut hash, integer.checked_div(a,b),prime);
feed_result(&mut hash, integer.checked_rem(a,b),prime);
feed_value(&mut hash, integer.wrapping_add(a,b),prime);
feed_value(&mut hash, integer.wrapping_sub(a,b),prime);
feed_value(&mut hash, integer.wrapping_mul(a,b),prime);
feed_value(&mut hash, integer.saturating_add(a,b),prime);
feed_value(&mut hash, integer.saturating_sub(a,b),prime);
feed_value(&mut hash, integer.saturating_mul(a,b),prime);
feed_pair(&mut hash, integer.overflowing_add(a,b),prime);
feed_pair(&mut hash, integer.overflowing_sub(a,b),prime);
feed_pair(&mut hash, integer.overflowing_mul(a,b),prime);
feed_value(&mut hash, integer.abs_diff(a,b),prime);
feed_bool(&mut hash, integer.is_multiple_of(a,b),prime);
'''
UNARY_NAMES=['is_zero','is_negative','is_positive','is_even','is_odd','is_power_of_two','count_ones','count_zeros','leading_zeros','trailing_zeros','leading_ones','trailing_ones','bit_length','reverse_bits','swap_bytes','highest_one_bit','lowest_one_bit']
UNARY_CODE=''.join(f'feed_{"bool" if n.startswith("is_") else "value"}(&mut hash,integer.{n}(a),prime);\n' for n in UNARY_NAMES)
POWER_CODE='''feed_result(&mut hash,integer.checked_pow(a,exponent),prime);
feed_value(&mut hash,integer.wrapping_pow(a,exponent),prime);
feed_value(&mut hash,integer.saturating_pow(a,exponent),prime);
feed_pair(&mut hash,integer.overflowing_pow(a,exponent),prime);
'''
metadata=[]
for t,w,signed in TYPES:
    rng=random.Random(0xAE71_2026+w*17+signed)
    lo=-(1<<(w-1)) if signed else 0
    hi=(1<<(w-1))-1 if signed else (1<<w)-1
    if w==8:
        samples=list(range(lo,hi+1))
        pairs=[(a,b) for a in samples for b in samples]
    else:
        boundary=sorted({lo,lo+1,lo+2,hi,hi-1,hi-2,0,1,2,3,7,1<<(w-2)}|({-1,-2,-3,-7} if signed else set()))
        pairs=[(a,b) for a in boundary for b in boundary]+[(rng.randint(lo,hi),rng.randint(lo,hi)) for _ in range(96)]
        samples=sorted(set(boundary+[rng.randint(lo,hi) for _ in range(32)]))
    exponents=[0,1,2,3,4,5,7,8,15,16,31,32,63,64,127,128,129,255,256,1023,4294967295]
    rotations=list(range(2*w+3))+[4294967295]
    out=[HEADER]
    pair_expect=[];unary_expect=[];power_expect=[];rotate_expect=[]
    for p in PRIMES:
        if w==8:
            pair_expect.append([digest([x for b in samples for x in pair_values(a,b,w,signed)],p) for a in samples])
        else:
            pair_expect.append([digest(pair_values(a,b,w,signed),p) for a,b in pairs])
        unary_expect.append([digest(unary_values(a,w,signed),p) for a in samples])
        power_expect.append([digest([x for e in exponents for x in power_values(a,e,w,signed)],p) for a in samples])
        rotate_expect.append([digest([x for c in rotations for x in rotate_values(a,c,w,signed)],p) for a in samples])
    out += [avx_array(t,'sample',samples),avx_array('u32','powers',exponents),avx_array('u32','rotations',rotations)]
    if w!=8:
        out += [avx_array(t,'lefts',[a for a,b in pairs]),avx_array(t,'rights',[b for a,b in pairs])]
    for k in range(2):
        out += [avx_array('u64',f'pair{k}',pair_expect[k]),avx_array('u64',f'unary{k}',unary_expect[k]),avx_array('u64',f'power{k}',power_expect[k]),avx_array('u64',f'rotate{k}',rotate_expect[k])]
    out.append('i32 main() {\n')
    out.append(f'    if (integer.MIN_{t.upper()} != {lo}{t} || integer.MAX_{t.upper()} != {hi}{t}) return 1;\n')
    for k,p in enumerate(PRIMES):
        out.append(f'{{ u64 prime = {p}u64; mut usize i = 0usize; while (i < {len(pair_expect[k])}usize) {{ mut u64 hash=17u64; ')
        if w==8:
            out.append(f'{t} a=sample[i]; mut usize j=0usize; while (j < 256usize) {{ {t} b=sample[j]; {PAIR_CODE} j++; }}')
        else:
            out.append(f'{t} a=lefts[i]; {t} b=rights[i]; {PAIR_CODE}')
        out.append(f'if(hash!=pair{k}[i]) return {10+k}; i++; }} }}\n')
        out.append(f'{{ u64 prime={p}u64; mut usize i=0usize; while(i<{len(samples)}usize) {{ {t} a=sample[i]; mut u64 hash=17u64; {UNARY_CODE}')
        if signed:
            out.append('''feed_value(&mut hash,integer.unsigned_abs(a),prime);
feed_result(&mut hash,integer.checked_neg(a),prime);feed_result(&mut hash,integer.checked_abs(a),prime);
feed_value(&mut hash,integer.wrapping_abs(a),prime);feed_value(&mut hash,integer.saturating_neg(a),prime);feed_value(&mut hash,integer.saturating_abs(a),prime);
feed_pair(&mut hash,integer.overflowing_neg(a),prime);feed_pair(&mut hash,integer.overflowing_abs(a),prime);
''')
        out.append(f'feed_value(&mut hash,integer.wrapping_neg(a),prime); if(hash!=unary{k}[i]) return {20+k}; i++; }} }}\n')
        out.append(f'{{ u64 prime={p}u64; mut usize i=0usize; while(i<{len(samples)}usize) {{ {t} a=sample[i]; mut u64 hash=17u64; mut usize j=0usize; while(j<{len(exponents)}usize) {{ u32 exponent=powers[j]; {POWER_CODE} j++; }} if(hash!=power{k}[i]) return {30+k}; i++; }} }}\n')
        out.append(f'{{ u64 prime={p}u64; mut usize i=0usize; while(i<{len(samples)}usize) {{ {t} a=sample[i]; mut u64 hash=17u64; mut usize j=0usize; while(j<{len(rotations)}usize) {{ u32 amount=rotations[j]; feed_value(&mut hash,integer.rotate_left(a,amount),prime); feed_value(&mut hash,integer.rotate_right(a,amount),prime); j++; }} if(hash!=rotate{k}[i]) return {40+k}; i++; }} }}\n')
    out.append('return 0; }\n')
    path=OUTPUT/f'integer_{t}.avx'
    path.write_text('\n'.join(out))
    metadata.append({'type':t,'width':w,'signed':signed,'pairs':len(pairs),'unary_inputs':len(samples),'power_cases':len(samples)*len(exponents),'rotate_cases':len(samples)*len(rotations),'fixture_sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
result={'schema_version':1,'reference':'Python arbitrary-precision integers; truncation by magnitudes; powers modulo 2^width; independent representation operations','comparison':'Two independently reduced per-row fingerprints with primes4294967291 and4294967279; fingerprints are probabilistic rather than individual exact output assertions. Direct targeted extrema tests are separate.','seed':'0xAE712026 + width*17 + signed','records':metadata}
(args.metadata if args.metadata else OUTPUT/'integer_golden.json').write_text(json.dumps(result,indent=2)+'\n')
direct=[HEADER]
for t,w,signed in TYPES:
    lo=-(1<<(w-1)) if signed else 0
    hi=(1<<(w-1))-1 if signed else (1<<w)-1
    def v(n): return str(n)+t
    expectations=[
        ('wrapping_add',f'{v(hi)},{v(1)}',lo),
        ('wrapping_sub',f'{v(lo)},{v(1)}',hi),
        ('wrapping_mul',f'{v(hi)},{v(hi)}',1),
        ('saturating_add',f'{v(hi)},{v(1)}',hi),
        ('saturating_sub',f'{v(lo)},{v(1)}',lo),
        ('saturating_mul',f'{v(hi)},{v(hi)}',hi),
        ('wrapping_pow',f'{v(hi)},2u32',1),
        ('saturating_pow',f'{v(hi)},2u32',hi),
        ('wrapping_neg',v(lo),lo if signed else 0),
        ('reverse_bits',v(1),lo if signed else 1<<(w-1)),
        ('rotate_left',f'{v(1)},{w}u32',1),
        ('rotate_right',f'{v(1)},{w+1}u32',lo if signed else 1<<(w-1)),
    ]
    if signed:
        expectations += [('saturating_neg',v(lo),hi),('saturating_abs',v(lo),hi),('wrapping_abs',v(lo),lo),('checked_pow',f'-2{t},{w-1}u32',lo),('checked_mul',f'{v(lo)},{v(1)}',lo),('checked_rem',f'{v(lo)},-1{t}',0)]
    direct.append(f'i32 direct_{t}() {{\n')
    for i,(name,args,expected) in enumerate(expectations,1):
        expression=f'integer.{name}({args})'
        if name.startswith('checked'): expression=f'success({expression})'
        direct.append(f'if({expression}!={v(expected)}) return {i};\n')
    errortests=[('checked_add',f'{v(hi)},{v(1)}','Overflow'),('checked_sub',f'{v(lo)},{v(1)}','Overflow'),('checked_mul',f'{v(hi)},{v(hi)}','Overflow'),('checked_pow',f'2{t},{w}u32','Overflow'),('checked_div',f'{v(lo)},{v(0)}','DivisionByZero'),('checked_rem',f'{v(hi)},{v(0)}','DivisionByZero')]
    if signed: errortests += [('checked_neg',v(lo),'Overflow'),('checked_abs',v(lo),'Overflow'),('checked_div',f'{v(lo)},-1{t}','Overflow')]
    for i,(name,args,error) in enumerate(errortests,40):
        direct.append(f'''switch(integer.{name}({args})) {{
case Result<{t},integer.IntegerArithmeticError>.Err {{ integer.IntegerArithmeticError reason=error }}:
switch(reason) {{ case integer.IntegerArithmeticError.{error}: break; default: return {i}; break; }} break;
default: return {i}; break;
}}\n''')
    direct.append('return 0; }\n')
direct.append('''bool codes() {
    integer.IntegerArithmeticError overflow=integer.IntegerArithmeticError.Overflow;
    integer.IntegerArithmeticError division=integer.IntegerArithmeticError.DivisionByZero;
    &dyn error.Error a=&overflow;
    &dyn error.Error b=&division;
    return string.equals(a.code(),"integer.overflow") && string.equals(b.code(),"integer.division_by_zero") && string.equals(a.message(),"The exact result does not fit its integer type.") && string.equals(b.message(),"Integer division requires a nonzero divisor.");
}
i32 main() {
''')
for i,(t,w,signed) in enumerate(TYPES,1): direct.append(f'if(direct_{t}()!=0) return {i};\n')
direct.append('if(!codes())return 100;return 0; }\n')
# Explicit import is required even when the integer module itself imports Error.
direct[0]=direct[0].replace('import integer;','import integer;\nimport error;')
(OUTPUT/'integer_extrema.avx').write_text('\n'.join(direct))
print(json.dumps({'fixtures':len(metadata),'pairs':sum(x['pairs'] for x in metadata),'powers':sum(x['power_cases'] for x in metadata),'rotations':sum(x['rotate_cases'] for x in metadata)}))
