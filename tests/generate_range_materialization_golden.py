# ========================
#
# Created By : miruku_sora2026
#
# File name : generate_range_materialization_golden.py
#
# Description :
#     독립 수열·정적 배열 기대값과 제어된 할당 및 초기화 관찰용 테스트 입력을 만든다.
#     Generate independent sequence expectations and controlled allocation/initialization probe inputs.
#
# ========================
from pathlib import Path
import argparse,random,json,re

parser=argparse.ArgumentParser(description='Independent exact Range materialization oracle and explicit IR observer seam.')
parser.add_argument('--output-dir',type=Path)
parser.add_argument('--metadata',type=Path)
parser.add_argument('--instrument-ir',type=Path)
parser.add_argument('--output-ir',type=Path)
parser.add_argument('--probe',choices=('allocator','initializer'))
args=parser.parse_args()
if args.instrument_ir:
    if not args.output_ir or not args.probe:parser.error('--instrument-ir requires --output-ir and --probe')
    source=args.instrument_ir.read_bytes().decode('utf-8').replace('\r\n','\n')
    if args.probe=='initializer':
        pattern=r'(?P<gep>^\s+(?P<dest>%\w+) = getelementptr \[4096 x i32\], ptr %repeattmp\d+[^\n]*\n)(?P<store>^\s+store i32 [^,]+, ptr (?P=dest)\n)'
        source,count=re.subn(pattern,lambda m:m.group('gep')+'  call void @range_test_array_store()\n'+m.group('store'),source,flags=re.M)
        if count!=1:parser.error('Expected exactly one 4096-element ArrayRepeat store instruction')
        source+='\ndeclare void @range_test_array_store()\n'
    if source.count('define i32 @main(')!=1:parser.error('Expected exactly one exported i32 main')
    source=source.replace('@malloc','@range_test_malloc').replace('@free','@range_test_free').replace('@main(','@range_test_entry(')
    args.output_ir.write_bytes(source.encode('utf-8'))
    print(json.dumps({'observer_seam':args.probe,'original_production_ir_preserved':str(args.instrument_ir)}))
    raise SystemExit(0)
if not args.output_dir:parser.error('--output-dir is required when generating fixtures')
O=args.output_dir.resolve();O.mkdir(parents=True,exist_ok=True)
TYPES=[(f'{p}{w}',w,p=='i')for w in(8,16,32,64,128)for p in('i','u')]+[('isize',64,True),('usize',64,False)]
HEADER='''import range;
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok{T output=value}:return output;break;default:Never.unreachable();break;}}
i32 main(){'''
records=[]
for t,w,signed in TYPES:
 lo=-(1<<(w-1))if signed else 0;hi=(1<<(w-1))-1 if signed else(1<<w)-1;u='usize'if t=='isize'else('u'+str(w)if signed else t);maxstep=(1<<w)-1
 cases=[(lo,lo+3,False,False,1),(hi,hi-2,True,True,1),(lo,hi,False,True,max(1,(hi-lo+2)//3)),(hi,lo,True,False,max(1,(hi-lo+3)//4)),(hi,lo,False,False,1),(hi,hi,False,False,1),(hi,hi,False,True,1),(lo,hi,False,True,maxstep)]
 if w==8:cases.append((lo,hi,False,True,1))
 rng=random.Random(20261018+w+signed)
 for _ in range(8):
  a=rng.randint(lo,hi);b=rng.randint(lo,hi);cases.append((a,b,bool(rng.getrandbits(1)),bool(rng.getrandbits(1)),max(1,(abs(a-b)+4)//5)))
 code=[HEADER]
 for i,(a,b,desc,inc,step)in enumerate(cases):
  values=list(range(a,b+(-1 if desc else 1)if inc else b,-step if desc else step));N=len(values);wrong=1 if N==0 else 0;ctor='descending_closed'if desc and inc else'descending'if desc else'closed'if inc else'new';l=lambda n:str(n)+t
  code.append('{'+f'Range<{t}> descriptor=success(Range.with_step(Range.{ctor}({l(a)},{l(b)}),{step}{u}));Range<{t}> original=descriptor;')
  code.append(f'{t} expected[{N}]=['+','.join(l(x)for x in values)+'];')
  code.append(f'Vector<{t}> actual=success(range.collect_vector(descriptor));if(Vector.len(&actual)!={N}usize||!range.equal(&descriptor,&original))return 1;')
  code.append(f'Array<{t},{N}> fixed=success(range.to_array<{N}>(descriptor));if(!range.equal(&descriptor,&original))return 2;')
  code.append(f'for(mut usize i=0usize;i<{N}usize;i++)if(actual[i]!=expected[i]||fixed[i]!=expected[i])return 3;')
  code.append(f'''switch(move range.to_array<{wrong}>(descriptor)){{
   case Result<Array<{t},{wrong}>,range.RangeArrayError>.Err{{range.RangeArrayError reason=error}}:
    switch(reason){{case range.RangeArrayError.LengthMismatch{{usize expected_count=expected,usize actual_count=actual}}:if(expected_count!={wrong}usize||actual_count!={N}usize)return 4;break;default:return 5;break;}}break;
   default:return 6;break;
  }}''')
  code.append('}')
 if w>=64:
  code.append('{'+f'Range<{t}> full=Range.closed({lo}{t},{hi}{t});')
  code.append(f'''switch(move range.collect_vector(full)){{case Result<Vector<{t}>,CollectionError>.Err{{CollectionError reason=error}}:switch(move reason){{case CollectionError.SizeOverflow:break;default:return 7;break;}}break;default:return 8;break;}}
  switch(move range.to_array<0>(full)){{case Result<Array<{t},0>,range.RangeArrayError>.Err{{range.RangeArrayError reason=error}}:switch(reason){{case range.RangeArrayError.CountOverflow:break;default:return 9;break;}}break;default:return 10;break;}}''')
  threshold=((1<<63)-1)//(w//8)
  code.append(f'Range<{t}> oversize=Range.new(0{t},{threshold+1}{t});')
  code.append(f'''switch(move range.collect_vector(oversize)){{case Result<Vector<{t}>,CollectionError>.Err{{CollectionError reason=error}}:switch(move reason){{case CollectionError.SizeOverflow:break;default:return 11;break;}}break;default:return 12;break;}}''')
  code.append('}')
 code.append('return 0;}');p=O/f'materialize_{t}.avx';p.write_bytes(('\n'.join(code)+'\n').encode('utf-8'));records.append({'fixture':p.name,'sequence_cases':len(cases),'exact_expected_elements':sum(len(list(range(a,b+(-1 if d else 1)if c else b,-s if d else s)))for a,b,d,c,s in cases),'count_and_byte_overflow':w>=64})

TYPES=[(f'{p}{w}',w,p=='i')for w in(8,16,32,64,128)for p in('i','u')]+[('isize',64,True),('usize',64,False)]
source=['import range; i32 main(){']
for t,w,signed in TYPES:
 source.append('''{Range<TYPE> descriptor=Range.closed(0TYPE,2TYPE);
 switch(move range.collect_vector(descriptor)){
 case Result<Vector<TYPE>,CollectionError>.Ok{Vector<TYPE> values=value}:if(Vector.len(&values)!=3usize||values[0]!=0TYPE||values[2]!=2TYPE)return 1;break;
 case Result<Vector<TYPE>,CollectionError>.Err{CollectionError reason=error}:switch(move reason){case CollectionError.AllocationFailure:break;default:return 2;break;}break;
 }}'''.replace('TYPE',t))
source.append('return 0;}');(O/'allocator.avx').write_bytes(('\n'.join(source)+'\n').encode('utf-8'))
# No output allocation is permitted for empty, count-overflow, byte-overflow, or fixed-array success/mismatch paths.
source=['''import range;
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok{T output=value}:return output;break;default:Never.unreachable();break;}}
i32 main(){''']
for t,w,signed in TYPES:
 lo=-(1<<(w-1))if signed else 0;hi=(1<<(w-1))-1 if signed else(1<<w)-1
 source.append('''{Range<TYPE> empty=range.empty<TYPE>();Vector<TYPE> none=success(range.collect_vector(empty));if(Vector.len(&none)!=0usize)return 1;
 Array<TYPE,0> zero=success(range.to_array<0>(empty));
 Range<TYPE> small=Range.closed(0TYPE,2TYPE);Array<TYPE,3> fixed=success(range.to_array<3>(small));if(fixed[2]!=2TYPE)return 2;
 switch(move range.to_array<1>(small)){case Result<Array<TYPE,1>,range.RangeArrayError>.Err:break;default:return 3;break;}
 }'''.replace('TYPE',t))
 if w>=64:
  limit=((1<<63)-1)//(w//8)
  source.append(f'''{{Range<{t}> oversized=Range.new(0{t},{limit+1}{t});
 switch(move range.collect_vector(oversized)){{case Result<Vector<{t}>,CollectionError>.Err:break;default:return 4;break;}}
 Range<{t}> full=Range.closed({lo}{t},{hi}{t});
 switch(move range.collect_vector(full)){{case Result<Vector<{t}>,CollectionError>.Err:break;default:return 5;break;}}
 switch(move range.to_array<0>(full)){{case Result<Array<{t},0>,range.RangeArrayError>.Err:break;default:return 6;break;}}
 }}''')
source.append('return 0;}');(O/'no_allocation.avx').write_bytes(('\n'.join(source)+'\n').encode('utf-8'))

TYPES=[('i64',8),('u64',8),('i128',16),('u128',16),('isize',8),('usize',8)]
source=['import range; i32 main(){']
for t,size in TYPES:
 count=((1<<63)-1)//size
 source.append(f'''{{Range<{t}> descriptor=Range.new(0{t},{count}{t});
 switch(move range.collect_vector(descriptor)){{
 case Result<Vector<{t}>,CollectionError>.Err{{CollectionError reason=error}}:switch(move reason){{case CollectionError.AllocationFailure:break;default:return 1;break;}}break;
 default:return 2;break;}}
 }}''')
source.append('return 0;}');fixture=O/'boundary.avx';fixture.write_bytes(('\n'.join(source)+'\n').encode('utf-8'))


for mode,stop in [('mismatch',0),('match',4095)]:
    ok_return='1'if mode=='mismatch'else'output[0]==0i32&&output[4095]==4095i32?0:2'
    err_return='0'if mode=='mismatch'else'3'
    source=f"""import range; i32 main(){{Range<i32> values=Range.closed(0i32,{stop}i32);
 switch(move range.to_array<4096>(values)){{
 case Result<Array<i32,4096>,range.RangeArrayError>.Ok{{Array<i32,4096> output=value}}:return {ok_return};break;
 case Result<Array<i32,4096>,range.RangeArrayError>.Err:return {err_return};break;
 }} }}"""
    (O/f'init_{mode}.avx').write_bytes((source+'\n').encode('utf-8'))

negative_inputs={'runtime_extent.avx': 'import range;\ni32 main(){usize width=3usize;Range<i32> values=Range.closed(0i32,2i32);range.to_array<width>(values);return 0;}\n', 'negative_extent.avx': 'import range;\ni32 main(){Range<i32> values=Range.closed(0i32,2i32);range.to_array<-1>(values);return 0;}\n', 'layout_overflow.avx': 'import range;\ni32 main(){Range<i128> values=Range.new(0i128,0i128);range.to_array<18446744073709551615usize>(values);return 0;}\n', 'noncopy_repeat.avx': 'struct Owned {public i32 value;};\ni32 main(){Owned values[2]=[Owned{value=1};2];return 0;}\n'}
for name,source in negative_inputs.items():(O/name).write_bytes(source.encode('utf-8'))
metadata={'oracle':'Exact Python range/list sequence expectations; no implementation rank/span helpers imported.',
 'sequence_cases':sum(row['sequence_cases']for row in records),'expected_elements_per_container':sum(row['exact_expected_elements']for row in records),
 'fixtures':records,'native_probes':['allocator','no_allocation','boundary','init_mismatch','init_match'],
 'negative_fixtures':sorted(negative_inputs),
 'observer_boundary':'Only generated LLVM allocation symbol names or the explicit ArrayRepeat store instruction are instrumented. Original IR is retained; this is not process-wide tracing.',
 'fixed_array_abi_boundary':'An early mismatch skips explicit ArrayRepeat initialization. Current aggregate Result storage, zero-initialization and copying can still cost O(N), including on errors.'}
path=args.metadata if args.metadata else O/'range_materialization_golden.json'
path.write_bytes((json.dumps(metadata,indent=2)+'\n').encode('utf-8'))
print(json.dumps({key:metadata[key]for key in('sequence_cases','expected_elements_per_container')}))
