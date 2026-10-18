# ========================
# Created By : miruku_sora2026
# File name : generate_range_golden.py
# Description :
#     Python 수열 목록으로 Range 소스 모듈의 독립 기대값을 만든다.
#     Generate independent expectations for source Range utilities from Python sequences.
# ========================
from pathlib import Path
import random,json,hashlib,argparse
parser=argparse.ArgumentParser(description='Independent Python sequence and arbitrary-precision Range oracle.')
parser.add_argument('--output-dir',type=Path,required=True)
parser.add_argument('--metadata',type=Path)
options=parser.parse_args()
OUT=options.output_dir.resolve();OUT.mkdir(parents=True,exist_ok=True)
PRIMES=(4294967291,4294967279)
BASE=65599
HEADER='''import range;
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok{T output=value}:return output;break;default:Never.unreachable();break;}}
u64 encode<T>(T value,u64 prime) where T implements Copy {
 u128 modulus=success<u128,IntegerError>(Integer.convert<u128>(prime));
 switch(Integer.convert<u128>(value)){
  case Result<u128,IntegerError>.Ok{u128 output=value}:u128 reduced=output%modulus;u64 encoded=success<u64,IntegerError>(Integer.convert<u64>(reduced));return encoded;break;
  default:i128 negative=success<i128,IntegerError>(Integer.convert<i128>(value));u128 predecessor=success<u128,IntegerError>(Integer.convert<u128>(-(negative+1i128)));u128 reduced=(predecessor+1u128)%modulus;u64 residue=success<u64,IntegerError>(Integer.convert<u64>(reduced));return residue==0u64?0u64:prime-residue;break;
 }
}
void feed(&mut u64 hash,u64 token,u64 prime){*hash=(*hash*65599u64+token)%prime;return;}
void value<T>(&mut u64 hash,T input,u64 prime) where T implements Copy {feed(hash,1u64,prime);feed(hash,encode(input,prime),prime);return;}
void truth(&mut u64 hash,bool input,u64 prime){value(hash,input?1u64:0u64,prime);return;}
void optional<T>(&mut u64 hash,Optional<T> input,u64 prime) where T implements Copy {
 switch(move input){case Optional<T>.Some{T item=value}:value(hash,item,prime);break;case Optional<T>.None:feed(hash,0u64,prime);feed(hash,0u64,prime);break;}return;
}
void at_result<T>(&mut u64 hash,Result<T,range.RangeOperationError> input,u64 prime) where T implements Copy {
 switch(move input){case Result<T,range.RangeOperationError>.Ok{T item=value}:value(hash,item,prime);break;
 case Result<T,range.RangeOperationError>.Err{range.RangeOperationError reason=error}:
  switch(reason){case range.RangeOperationError.OutOfBounds{usize position=index}:feed(hash,2u64,prime);feed(hash,encode(position,prime),prime);break;default:Never.unreachable();break;}break;}return;
}
bool is_desc(range.RangeDirection input){switch(input){case range.RangeDirection.Descending:return true;break;case range.RangeDirection.Ascending:return false;break;}}
void sequence<T>(&mut u64 hash,Range<T> input,u64 prime) where T implements Copy {
 mut range.Cursor<T> cursor=range.iter(input);
 while(!cursor.is_exhausted()){optional(hash,cursor.next(),prime);}
 optional(hash,cursor.next(),prime);optional(hash,cursor.next_back(),prime);
 truth(hash,cursor.is_exhausted(),prime);return;
}
void backwards<T>(&mut u64 hash,Range<T> input,u64 prime) where T implements Copy {
 mut range.Cursor<T> cursor=range.iter(input);
 while(!cursor.is_exhausted()){optional(hash,cursor.next_back(),prime);}
 optional(hash,cursor.next(),prime);truth(hash,cursor.is_exhausted(),prime);return;
}
void alternating<T>(&mut u64 hash,Range<T> input,u64 prime) where T implements Copy {
 mut range.Cursor<T> cursor=range.iter(input);mut bool front=true;
 while(!cursor.is_exhausted()){if(front)optional(hash,cursor.next(),prime);else optional(hash,cursor.next_back(),prime);front=!front;}
 optional(hash,cursor.next(),prime);truth(hash,cursor.is_exhausted(),prime);return;
}
'''
def seq(a,b,descending,inclusive,step):
    # This oracle uses Python's sequence constructor and slicing. It does not use rank/span formulas.
    return list(range(a,b+(-1 if descending else 1) if inclusive else b,-step if descending else step))
def digest(tokens,p):
    h=17
    for status,v in tokens:h=(h*BASE+status)%p;h=(h*BASE+(v%p if status else 0))%p
    return h
V=lambda x:(1,int(x))
def opt(values,i=0):return V(values[i])if i<len(values)else(0,0)
def feedseq(values):return [V(x)for x in values]+[(0,0),(0,0),V(True)]
def feedback(values):return [V(x)for x in reversed(values)]+[(0,0),V(True)]
def feedalternate(values):
    items=list(values);output=[];front=True
    while items:output.append(V(items.pop(0 if front else -1)));front=not front
    return output+[(0,0),V(True)]
def arr(t,name,values):return f'{t} {name}[{len(values)}]=['+','.join(str(v)+t for v in values)+'];\n'
TYPES=[(f'{p}{w}',w,p=='i')for w in(8,16,32,64,128)for p in('i','u')]+[('isize',64,True),('usize',64,False)]
META=[]
# Exhaust every endpoint pair for i8 and u8. Constructor kind and positive step cycle across pairs.
for t,w,signed in TYPES[:2]:
 lo=-(1<<(w-1))if signed else 0;hi=(1<<(w-1))-1 if signed else(1<<w)-1
 samples=list(range(lo,hi+1));expected=[[],[]]
 for a in samples:
  tokens=[]
  for bi,b in enumerate(samples):
   mode=(a-lo+bi)%4;descending=bool(mode&1);inclusive=bool(mode&2);step=[1,2,3,127,128,255][(a-lo+bi)%6]
   s=seq(a,b,descending,inclusive,step)
   tokens +=[V(a in s),V(b in s),V(0 in s),opt(s),opt(s,len(s)-1)if s else(0,0)]
   for index in(0,1,2,255,256,(1<<64)-1):tokens+=[opt(s,index)]
  for pi,p in enumerate(PRIMES):expected[pi].append(digest(tokens,p))
 out=[HEADER]
 for pi,p in enumerate(PRIMES):out.append(arr('u64',f'expected{pi}',expected[pi]))
 out.append(f'''i32 main(){{
 {t} samples[256]=['''+','.join(str(x)+t for x in samples)+'''];
 u8 steps[6]=[1u8,2u8,3u8,127u8,128u8,255u8];
 usize indices[6]=[0usize,1usize,2usize,255usize,256usize,18446744073709551615usize];
 u64 primes[2]=[4294967291u64,4294967279u64];
 for(mut usize pi=0usize;pi<2usize;pi++){
  u64 prime=primes[pi];
  for(mut usize ai=0usize;ai<256usize;ai++){
   mut u64 hash=17u64;
   for(mut usize bi=0usize;bi<256usize;bi++){
    '''+f'''{t} a=samples[ai];{t} b=samples[bi];usize mode=(ai+bi)%4usize;
    bool descending=(mode&1usize)!=0usize;bool inclusive=(mode&2usize)!=0usize;
    Range<{t}> base=descending?(inclusive?Range.descending_closed(a,b):Range.descending(a,b)):(inclusive?Range.closed(a,b):Range.new(a,b));
    Range<{t}> descriptor=success(Range.with_step(base,steps[(ai+bi)%6usize]));
    truth(&mut hash,range.contains(&descriptor,a),prime);truth(&mut hash,range.contains(&descriptor,b),prime);truth(&mut hash,range.contains(&descriptor,0{t}),prime);
    optional(&mut hash,range.first(&descriptor),prime);optional(&mut hash,range.last(&descriptor),prime);
    for(mut usize ix=0usize;ix<6usize;ix++)optional(&mut hash,range.get(&descriptor,indices[ix]),prime);
   }}
   u64 expected=pi==0usize?expected0[ai]:expected1[ai];if(hash!=expected)return 1;
  }}
 }}return 0;}}''')
 path=OUT/f'exhaustive_{t}.avx';path.write_bytes('\n'.join(out).encode('utf-8'));META.append({'fixture':path.name,'endpoint_pairs':65536,'prime_repetitions':2,'kind':'exhaustive_endpoint_pairs_with_cycling_constructor_and_step'})
# All twelve element types: sequences are intentionally bounded, while endpoints include extrema.
for t,w,signed in TYPES:
 lo=-(1<<(w-1))if signed else 0;hi=(1<<(w-1))-1 if signed else(1<<w)-1;u='usize'if t=='isize'else('u'+str(w)if signed else t);maxstep=(1<<w)-1
 points=[lo,lo+1,lo+3,hi-3,hi-1,hi,0,1,3]
 if signed:points +=[-1,-3]
 cases=[]
 for a in sorted(set(points)):
  for b in sorted(set(points)):
   for descending,inclusive in((False,False),(False,True),(True,False),(True,True)):
    # Keep cursors bounded even for full i128 domains. Large strides still exercise rank reconstruction.
    distance=abs(a-b);step=max(1,(distance+5)//6)
    cases.append((a,b,descending,inclusive,step))
 rng=random.Random(20261018+w+signed)
 for _ in range(48):
  a=rng.randint(lo,hi);b=rng.randint(lo,hi);desc=bool(rng.getrandbits(1));inc=bool(rng.getrandbits(1));step=max(1,(abs(a-b)+5)//6);cases.append((a,b,desc,inc,step))
 expected=[[],[]]
 for a,b,desc,inc,step in cases:
  s=seq(a,b,desc,inc,step);tokens=[]
  tokens +=[V(desc),V(True),V(s==list(reversed(s)))]
  tokens +=feedseq(s)+feedback(s)+feedalternate(s)+feedseq(list(reversed(s)))
  # Independent cursor copies and reverse iterator first element.
  tokens +=[opt(s),opt(s),opt(list(reversed(s)))]
  for n in(0,1,2,4,8,(1<<64)-1):
   tokens +=feedseq(s[:n])+feedseq(s[n:])
   tokens +=[opt(s,n),(1,s[n])if n<len(s)else(2,n)]
   if n<=len(s):tokens +=[V(True)]+feedseq(s[:n])+feedseq(s[n:])
   else:tokens +=[(2,n)]
  for factor in(0,1,2,maxstep):
   if factor==0:tokens +=[(3,0)]
   elif step*factor>maxstep:tokens +=[(4,0)]
   else:tokens +=[V(True)]+feedseq(seq(a,b,desc,inc,step*factor))
  for pi,p in enumerate(PRIMES):expected[pi].append(digest(tokens,p))
 out=[HEADER]
 out +=[arr(t,'begins',[x[0]for x in cases]),arr(t,'ends',[x[1]for x in cases]),arr(u,'steps',[x[4]for x in cases]),arr('u8','modes',[(1 if x[2]else 0)+(2 if x[3]else 0)for x in cases])]
 for pi,p in enumerate(PRIMES):out.append(arr('u64',f'expected{pi}',expected[pi]))
 out.append(f'''i32 main(){{
 usize counts[6]=[0usize,1usize,2usize,4usize,8usize,18446744073709551615usize];
 {u} factors[4]=[0{u},1{u},2{u},{maxstep}{u}];u64 primes[2]=[4294967291u64,4294967279u64];
 for(mut usize pi=0usize;pi<2usize;pi++){{
  u64 prime=primes[pi];
  for(mut usize ci=0usize;ci<{len(cases)}usize;ci++){{
   {t} a=begins[ci];{t} b=ends[ci];u8 mode=modes[ci];bool desc=(mode&1u8)!=0u8;bool inc=(mode&2u8)!=0u8;
   Range<{t}> base=desc?(inc?Range.descending_closed(a,b):Range.descending(a,b)):(inc?Range.closed(a,b):Range.new(a,b));
   Range<{t}> descriptor=success(Range.with_step(base,steps[ci]));Range<{t}> reversed=range.reverse(descriptor);
   mut u64 hash=17u64;
   truth(&mut hash,is_desc(range.direction(&descriptor)),prime);truth(&mut hash,range.equal(&descriptor,&descriptor),prime);truth(&mut hash,range.sequence_equal(&descriptor,&reversed),prime);
   sequence(&mut hash,descriptor,prime);backwards(&mut hash,descriptor,prime);alternating(&mut hash,descriptor,prime);sequence(&mut hash,reversed,prime);
   mut range.Cursor<{t}> cursor=range.iter(descriptor);mut range.Cursor<{t}> copied=cursor;mut range.Cursor<{t}> reverse_cursor=range.rev_iter(descriptor);
   optional(&mut hash,cursor.next(),prime);optional(&mut hash,copied.next(),prime);optional(&mut hash,reverse_cursor.next(),prime);
   for(mut usize ni=0usize;ni<6usize;ni++){{usize n=counts[ni];
    sequence(&mut hash,range.take(descriptor,n),prime);sequence(&mut hash,range.skip(descriptor,n),prime);
    optional(&mut hash,range.get(&descriptor,n),prime);at_result(&mut hash,range.at(&descriptor,n),prime);
    switch(move range.split_at(descriptor,n)){{
     case Result<Tuple<Range<{t}>,Range<{t}>>,range.RangeOperationError>.Ok{{Tuple<Range<{t}>,Range<{t}>> pair=value}}:
      truth(&mut hash,true,prime);sequence(&mut hash,*Tuple.get<0>(&pair),prime);sequence(&mut hash,*Tuple.get<1>(&pair),prime);break;
     case Result<Tuple<Range<{t}>,Range<{t}>>,range.RangeOperationError>.Err{{range.RangeOperationError reason=error}}:
      switch(reason){{case range.RangeOperationError.OutOfBounds{{usize position=index}}:feed(&mut hash,2u64,prime);feed(&mut hash,encode(position,prime),prime);break;default:Never.unreachable();break;}}break;
    }}
   }}
   for(mut usize fi=0usize;fi<4usize;fi++){{
    switch(move range.step_by(descriptor,factors[fi])){{
     case Result<Range<{t}>,range.RangeOperationError>.Ok{{Range<{t}> scaled=value}}:truth(&mut hash,true,prime);sequence(&mut hash,scaled,prime);break;
     case Result<Range<{t}>,range.RangeOperationError>.Err{{range.RangeOperationError reason=error}}:
      switch(reason){{case range.RangeOperationError.InvalidStep:feed(&mut hash,3u64,prime);feed(&mut hash,0u64,prime);break;case range.RangeOperationError.StepOverflow:feed(&mut hash,4u64,prime);feed(&mut hash,0u64,prime);break;default:Never.unreachable();break;}}break;
    }}
   }}
   u64 expected=pi==0usize?expected0[ci]:expected1[ci];if(hash!=expected)return 2;
  }}
 }}return 0;}}''')
 path=OUT/f'operations_{t}.avx';path.write_bytes('\n'.join(out).encode('utf-8'));META.append({'fixture':path.name,'bounded_sequence_cases':len(cases),'prime_repetitions':2,'kind':'all_helper_and_cursor_operations'})

# Exact wide-domain assertions supplement probabilistic fingerprints.
TYPES=[(f'{p}{w}',w,p=='i')for w in(8,16,32,64,128)for p in('i','u')]+[('isize',64,True),('usize',64,False)]
source=['''import range;
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok{T output=value}:return output;break;default:Never.unreachable();break;}}
T some<T>(Optional<T> input){switch(move input){case Optional<T>.Some{T output=value}:return output;break;default:Never.unreachable();break;}}
bool absent<T>(Optional<T> input){switch(move input){case Optional<T>.None:return true;break;default:return false;break;}}
i32 main(){''']
checks=0
for t,w,signed in TYPES:
 lo=-(1<<(w-1))if signed else 0;hi=(1<<(w-1))-1 if signed else(1<<w)-1;u='usize'if t=='isize'else('u'+str(w)if signed else t);maxstep=(1<<w)-1
 lit=lambda n:str(n)+t
 def assert_(condition):
  global checks;checks+=1;source.append(f'if(!({condition}))return {checks};')
 source.append('{')
 source.append(f'Range<{t}> full=Range.closed({lit(lo)},{lit(hi)});Range<{t}> reversed=range.reverse(full);')
 assert_(f'some(range.first(&full))=={lit(lo)}&&some(range.last(&full))=={lit(hi)}')
 assert_(f'some(range.first(&reversed))=={lit(hi)}&&some(range.last(&reversed))=={lit(lo)}')
 assert_(f'range.contains(&full,{lit(lo)})&&range.contains(&full,{lit(hi)})')
 source.append(f'Range<{t}> sparse=success(range.step_by(full,{maxstep}{u}));mut range.Cursor<{t}> sparse_cursor=range.iter(sparse);')
 assert_(f'some(sparse_cursor.next())=={lit(lo)}&&some(sparse_cursor.next_back())=={lit(hi)}&&sparse_cursor.is_exhausted()&&absent(sparse_cursor.next())&&absent(sparse_cursor.next_back())')
 source.append(f'Range<{t}> exclusive=success(Range.with_step(Range.new({lit(lo)},{lit(hi)}),{maxstep}{u}));')
 assert_(f'some(range.last(&exclusive))=={lit(lo)}&&absent(range.get(&exclusive,1usize))')
 source.append(f'Range<{t}> single_forward=Range.closed({lit(hi)},{lit(hi)});Range<{t}> single_backward=Range.descending_closed({lit(hi)},{lit(hi)});')
 assert_('range.sequence_equal(&single_forward,&single_backward)&&!range.equal(&single_forward,&single_backward)')
 source.append(f'Range<{t}> empty=range.empty<{t}>();Range<{t}> other_empty=Range.descending({lit(lo)},{lit(hi)});')
 assert_('range.sequence_equal(&empty,&other_empty)&&absent(range.first(&other_empty))&&absent(range.last(&other_empty))')
 assert_('!range.equal(&empty,&other_empty)')
 source.append(f'''switch(move range.step_by(sparse,2{u})){{case Result<Range<{t}>,range.RangeOperationError>.Err{{range.RangeOperationError reason=error}}:switch(reason){{case range.RangeOperationError.StepOverflow:if(!string.equals(reason.code(),"range.step_overflow")||!string.equals(reason.message(),"The scaled step does not fit its unsigned magnitude type."))return 240;break;default:return 241;break;}}break;default:return 242;break;}}''')
 if w>=64:
  index=(1<<64)-1;last_take=lo+index-1
  source.append(f'Range<{t}> taken=range.take(full,18446744073709551615usize);Range<{t}> skipped=range.skip(full,18446744073709551615usize);')
  assert_(f'some(range.last(&taken))=={lit(last_take)}')
  if w==64:
   assert_(f'some(range.first(&skipped))=={lit(hi)}&&some(range.last(&skipped))=={lit(hi)}')
  else:
   assert_(f'some(range.get(&full,18446744073709551615usize))=={lit(lo+index)}&&some(range.first(&skipped))=={lit(lo+index)}&&some(range.last(&skipped))=={lit(hi)}')
  source.append(f'Tuple<Range<{t}>,Range<{t}>> split=success(range.split_at(full,18446744073709551615usize));Range<{t}> left=*Tuple.get<0>(&split);Range<{t}> right=*Tuple.get<1>(&split);')
  assert_('range.sequence_equal(&left,&taken)&&range.sequence_equal(&right,&skipped)')
 source.append('}')
source.append('return 0;}')
(OUT/'exact_edges.avx').write_bytes(('\n'.join(source)+'\n').encode('utf-8'))

META.append({'fixture':'exact_edges.avx','exact_assertions':checks,'types':12,'kind':'exact_full_domain_and_extrema'})
metadata={'oracle':'Python range construction and list slicing; no source rank/span helpers imported',
 'fingerprints':'Two 32-bit primes are probabilistic summaries, supplemented by exact edge assertions',
 'fixtures':META}
metadata_path=options.metadata if options.metadata else OUT/'range_golden.json'
metadata_path.parent.mkdir(parents=True,exist_ok=True)
metadata_path.write_bytes((json.dumps(metadata,indent=2)+'\n').encode('utf-8'))
print(json.dumps({'fixtures':len(META),'endpoint_pairs':131072,'bounded_sequence_cases':4344,'exact_assertions':checks}))
