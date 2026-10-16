// ========================
//
// Created By : miruku_sora2026
//
// File name : language.test.ts
//
// Description :
//     편집 인덱스의 스코프, 모듈 충돌, 제네릭과 UTF-16 토큰 위치를 검사한다.
//     Verify editor indexing scopes, module collisions, generics, and UTF-16 token offsets.
//
// ========================

import assert from 'node:assert/strict';
import test from 'node:test';
import { analyzeDocument, buildIndex, builtinDocs, builtinMemberDocumentation, importedTypeCompletions, importedTypeDocumentation, namespaceAPIs, referencesTo, symbolAt, tokenize } from '../src/language';

const uri = 'file:///workspace/src/main.avx';

test('implemented core errors distinguish owned recovery from traps', () => {
    assert.match(builtinDocs.CollectionError, /AllocationFailure.*SizeOverflow/);
    assert.match(builtinDocs.InputError, /preserves.*input owner/);
    assert.match(builtinDocs.RangeError, /InvalidStep.*CountOverflow/);
    assert.match(builtinDocs.Vector, /CollectionError Result/);
    assert.match(builtinDocs.Range, /allocation-free/);
});

function at(text: string, needle: string, occurrence = 0): number {
    let offset = -1;
    for (let i = 0; i <= occurrence; ++i) offset = text.indexOf(needle, offset + 1);
    assert.ok(offset >= 0, needle);
    return offset;
}

test('token offsets reconstruct exact UTF-16 slices without treating quoted markers as comments', () => {
    const source = '/* 한글 😀 */ string text = "// quoted \\" content"; char letter = \'한\'; // tail\ni32 next = 4;';
    const tokens = tokenize(source);
    for (const token of tokens) {
        assert.equal(source.slice(token.start, token.end), token.text);
        assert.ok(token.end > token.start);
    }
    assert.equal(tokens.filter((token) => token.kind === 'comment').length, 2);
    assert.equal(tokens.filter((token) => token.kind === 'string').length, 2);
    assert.equal(tokens.find((token) => token.text === 'next')?.start, source.indexOf('next'));
});

test('range syntax is never consumed as a decimal number', () => {
    const tokens = tokenize('0..10 0..=10 1.5e-3 0xff 1_000');
    assert.deepEqual(tokens.map((token) => token.text), ['0', '..', '10', '0', '..=', '10', '1.5e-3', '0xff', '1_000']);
});

test('commented and quoted declaration text never enters the symbol index', () => {
    const source = '// i32 fake = 3;\n/* struct Ghost {}; */\nstring text = "i32 hidden = 1;";\ni32 value = 1;';
    const doc = analyzeDocument(uri, source, 'main');
    assert.deepEqual(doc.symbols.map((symbol) => symbol.name), ['text', 'value']);
});

test('type-first declarations preserve mutability, docs, generic parameters and nested types', () => {
    const source = '/* @return identity value */\npublic T identity<T>(T value) where T implements Copy { return value; }\nstruct Pair<U> { public U first; private U second; };\nmut i32 count = 1;\nPair<Pair<i32>> nested;';
    const doc = analyzeDocument(uri, source, 'main');
    assert.ok(doc.symbols.some((symbol) => symbol.name === 'identity' && symbol.kind === 'function' && symbol.public));
    assert.ok(doc.symbols.some((symbol) => symbol.name === 'identity' && symbol.documentation.includes('@return')));
    assert.ok(doc.symbols.some((symbol) => symbol.name === 'T' && symbol.kind === 'typeParameter'));
    assert.ok(doc.symbols.some((symbol) => symbol.name === 'U' && symbol.kind === 'typeParameter'));
    assert.ok(doc.symbols.some((symbol) => symbol.name === 'count' && symbol.mutable));
    assert.equal(doc.symbols.find((symbol) => symbol.name === 'nested')?.type, 'Pair<Pair<i32>>');
});

test('parameters and nested locals resolve to declarations within their lifetime', () => {
    const source = 'i32 add(i32 input) { i32 outer = input; { i32 inner = outer; } return outer; }';
    const doc = analyzeDocument(uri, source, 'main');
    const index = buildIndex([doc]);
    assert.equal(symbolAt(doc, at(source, 'input', 1), index)?.kind, 'parameter');
    const outer = doc.symbols.find((symbol) => symbol.name === 'outer');
    assert.ok(outer);
    assert.equal(symbolAt(doc, at(source, 'outer', 2), index)?.id, outer.id);
    assert.equal(referencesTo(index, outer.id, false).length, 2);
    assert.equal(symbolAt(doc, 0), undefined, 'return type text not mistaken for declaration use');
});

test('field labels and named arguments do not become local rename edits', () => {
    const source = 'struct Item { public i32 value; }; i32 main() { i32 value = 2; Item item = Item { value = value }; return pick(value = value); }';
    const doc = analyzeDocument(uri, source, 'main');
    const index = buildIndex([doc]);
    const local = doc.symbols.find((symbol) => symbol.name === 'value' && symbol.kind === 'variable');
    assert.ok(local);
    const references = referencesTo(index, local.id, false);
    assert.deepEqual(references.map((reference) => reference.start), [source.indexOf('value };'), source.indexOf('value);')]);
});

test('selective imports expose only selected names including full paths', () => {
    const target = analyzeDocument('file:///workspace/src/math/vector.avx', 'public struct Point {}; public i32 dot() { return 0; }', 'math.vector');
    const source = 'import math.vector { Point }; Point point; math.vector.Point qualified; i32 main() { return math.vector.dot(); }';
    const doc = analyzeDocument(uri, source, 'main');
    const index = buildIndex([doc, target]);
    assert.equal(symbolAt(doc, at(source, 'Point', 1), index)?.name, 'Point');
    assert.equal(symbolAt(doc, at(source, 'Point', 2), index)?.uri, target.uri);
    assert.equal(symbolAt(doc, source.indexOf('dot'), index), undefined);
});

test('module-only imports require qualification and do not leak private names', () => {
    const target = analyzeDocument('file:///workspace/src/helper.avx', 'public struct Point {}; struct Secret {};', 'helper');
    const source = 'import helper; helper.Point good; Point missing; helper.Secret forbidden;';
    const doc = analyzeDocument(uri, source, 'main');
    const index = buildIndex([doc, target]);
    assert.equal(symbolAt(doc, at(source, 'Point'), index)?.name, 'Point');
    assert.equal(symbolAt(doc, at(source, 'Point', 1), index), undefined);
    assert.equal(symbolAt(doc, source.indexOf('Secret'), index), undefined);
});

test('different original declarations stay ambiguous while full paths choose one', () => {
    const first = analyzeDocument('file:///workspace/src/math.avx', 'public struct Point {};', 'math');
    const second = analyzeDocument('file:///workspace/src/ui.avx', 'public struct Point {};', 'ui');
    const source = 'import math { Point }; import ui { * }; Point ambiguous; math.Point first; ui.Point second;';
    const doc = analyzeDocument(uri, source, 'main');
    const index = buildIndex([doc, first, second]);
    assert.equal(symbolAt(doc, at(source, 'Point', 1), index), undefined);
    assert.equal(symbolAt(doc, at(source, 'Point', 2), index)?.uri, first.uri);
    assert.equal(symbolAt(doc, at(source, 'Point', 3), index)?.uri, second.uri);
});

test('reexports deduplicate original declarations and tolerate cycles', () => {
    const origin = analyzeDocument('file:///workspace/src/math/vector.avx', 'public struct Point {};', 'math.vector');
    const reexport = analyzeDocument('file:///workspace/src/geometry.avx', 'public import math.vector { Point }; public import loop { Ghost };', 'geometry');
    const cycle = analyzeDocument('file:///workspace/src/loop.avx', 'public import geometry { Ghost };', 'loop');
    const source = 'import math.vector { Point }; import geometry { Point }; Point point; geometry.Point other;';
    const doc = analyzeDocument(uri, source, 'main');
    const index = buildIndex([doc, origin, reexport, cycle]);
    assert.equal(symbolAt(doc, at(source, 'Point', 2), index)?.uri, origin.uri);
    assert.equal(symbolAt(doc, at(source, 'Point', 3), index)?.uri, origin.uri);
});

test('enum variants and each case payload binding have separate scopes', () => {
    const source = 'enum Event { Left { public i32 value; }; Right { public i32 value; }; }; i32 main() { Event event = Event.Left { value = 1 }; switch (event) { case Event.Left { i32 local = value }: return local; break; case Event.Right { i32 local = value }: return local; break; } }';
    const doc = analyzeDocument(uri, source, 'main');
    const index = buildIndex([doc]);
    assert.equal(doc.symbols.filter((symbol) => symbol.kind === 'variant').length, 2);
    const first = symbolAt(doc, at(source, 'local', 1), index);
    const second = symbolAt(doc, at(source, 'local', 3), index);
    assert.ok(first && second);
    assert.notEqual(first.id, second.id);
    assert.notEqual(first.scope, second.scope);
});

test('incomplete edits never crash declaration indexing', () => {
    const examples = ['struct Item {', 'i32 main(i32 value', 'import math.vector {', 'Pair<Pair<', 'switch (event) { case Event.Value { i32 value =', '/* unfinished', 'string text = "unfinished'];
    for (const source of examples) assert.doesNotThrow(() => buildIndex([analyzeDocument(uri, source, 'main')]));
});

test('loop bindings do not leak past the loop statement', () => {
    for (const loop of ['for (mut i32 counter = 0; counter < 3; counter++) { i32 copy = counter; }',
        'for (i32 counter in 0..3) { i32 copy = counter; }',
        'for (i32 counter : values) { i32 copy = counter; }']) {
        const source = `i32 main() { ${loop} return counter; }`;
        const doc = analyzeDocument(uri, source, 'main');
        const index = buildIndex([doc]);
        assert.ok(symbolAt(doc, source.indexOf('counter; }'), index), `loop body binds ${loop}`);
        assert.equal(symbolAt(doc, source.lastIndexOf('counter'), index), undefined, `outside loop cannot bind ${loop}`);
    }
});

test('block comments match the compiler first-terminator rule', () => {
    const source = '/* outer /* inner */ i32 visible = 1;';
    const doc = analyzeDocument(uri, source, 'main');
    assert.ok(doc.symbols.some((symbol) => symbol.name === 'visible'));
});

test('generic return types and constraints refer to their own type parameter declarations', () => {
    for (const source of ['T identity<T>(T value) where T implements Copy { return value; }',
        'struct Pair<T> where T implements Copy { public T value; };']) {
        const doc = analyzeDocument(uri, source, 'main');
        const index = buildIndex([doc]);
        const parameter = doc.symbols.find((symbol) => symbol.kind === 'typeParameter' && symbol.name === 'T');
        assert.ok(parameter);
        const positions = doc.code.filter((token) => token.text === 'T').map((token) => token.start);
        assert.deepEqual(referencesTo(index, parameter.id).map((reference) => reference.start), positions, source);
    }
});

test('transparent aliases are declarations, references, and public import targets', () => {
    const definition = analyzeDocument('file:///types.avx', 'public type UserId = u64;', 'types');
    const source = 'import types { UserId }; UserId identity(UserId value) { return value; }';
    const doc = analyzeDocument(uri, source, 'main');
    const index = buildIndex([definition, doc]);
    const alias = definition.symbols.find(symbol => symbol.name === 'UserId');
    assert.equal(alias?.kind, 'typeAlias');
    assert.equal(alias?.type, 'u64');
    assert.equal(symbolAt(doc, source.indexOf('UserId identity'), index)?.id, alias?.id);
    assert.equal(referencesTo(index, alias!.id).length, 4);
});

test('numeric suffix tokens keep signed exponents and suffixes together', () => {
    const numbers = tokenize('1e-3f64 42u8 0xffu8 0..10').filter(token => token.kind === 'number');
    assert.deepEqual(numbers.map(token => token.text), ['1e-3f64', '42u8', '0xffu8', '0', '10']);
});

test('new built-in API hover is limited to explicit namespaces', () => {
    const source = 'string.byte_len(view); Tuple.get<0>(&record); Float.convert_exact<f32>(value); unknown.byte_len(view);';
    const doc = analyzeDocument(uri, source, 'main');
    assert.match(builtinMemberDocumentation(doc, source.indexOf('byte_len'))!, /UTF-8 byte length/);
    assert.match(builtinMemberDocumentation(doc, source.indexOf('get<'))!, /compile-time checked index/);
    assert.match(builtinMemberDocumentation(doc, source.indexOf('convert_exact'))!, /exact representation/);
    assert.equal(builtinMemberDocumentation(doc, source.lastIndexOf('byte_len')), undefined);
});

test('specialist CPU API documentation follows selective and module imports', () => {
    const analyze = (imports: string) => analyzeDocument(uri, `${imports}\ni32 main() { return 0; }`, 'main');
    assert.equal(namespaceAPIs(analyze(''), 'Simd'), undefined);
    assert.equal(namespaceAPIs(analyze('import simd;'), 'Simd'), undefined);
    assert.match(namespaceAPIs(analyze('import simd;'), 'simd.Simd')!.sum, /left to right/);
    const selective = analyze('import simd { Simd };');
    assert.match(namespaceAPIs(selective, 'Simd')!.store, /exclusive/);
    assert.equal(namespaceAPIs(selective, 'SimdMask'), undefined);
    assert.equal(namespaceAPIs(selective, 'simd.SimdMask'), undefined);
    assert.equal(Object.keys(namespaceAPIs(analyze('import simd { * };'), 'SimdMask')!).length, 7);
    assert.equal(namespaceAPIs(analyze(''), 'parallel'), undefined);
    assert.match(namespaceAPIs(analyze('import parallel;'), 'parallel')!.for_each, /join every worker/);
    const selectedParallel = namespaceAPIs(analyze('import parallel { worker_count };'), 'parallel')!;
    assert.deepEqual(Object.keys(selectedParallel), ['worker_count']);
    const source = 'import simd; i32 main() { simd.Simd.sum(value); return 0; }';
    const doc = analyzeDocument(uri, source, 'main');
    assert.match(builtinMemberDocumentation(doc, source.indexOf('sum'))!, /no reassociation/);
});

test('const generic names and default arguments retain declaration identities', () => {
    const source = 'struct Buffer<T = i32, const usize N = (2 + 2)> { public Array<T, N> values; public usize len(&Self self) { return N; } };';
    const doc = analyzeDocument(uri, source, 'main');
    const index = buildIndex([doc]);
    const parameters = doc.symbols.filter(symbol => symbol.kind === 'typeParameter');
    assert.deepEqual(parameters.map(symbol => symbol.name), ['T', 'N']);
    assert.equal(parameters[1].type, 'usize');
    assert.equal(referencesTo(index, parameters[1].id).length, 3);
    assert.ok(!parameters.some(symbol => symbol.name === 'const' || symbol.name === 'usize'));
});

test('lambda parameters resolve inside their body and do not leak', () => {
    const source = 'i32 main() { i32 factor = 2; rsn callback = [factor](i32 value) -> i32 { return value * factor; }; return value; }';
    const doc = analyzeDocument(uri, source, 'main');
    const index = buildIndex([doc]);
    const parameter = doc.symbols.find(symbol => symbol.kind === 'parameter' && symbol.name === 'value');
    assert.ok(parameter);
    assert.equal(symbolAt(doc, source.indexOf('value *'), index)?.id, parameter.id);
    assert.equal(symbolAt(doc, source.lastIndexOf('value'), index), undefined);
    const outer = doc.symbols.find(symbol => symbol.name === 'factor');
    assert.equal(symbolAt(doc, source.indexOf('factor; }'), index)?.id, outer?.id);
});

test('complex and numeric documentation requires the matching import', () => {
    const bare = analyzeDocument(uri, 'Complex value;', 'main');
    assert.equal(importedTypeDocumentation(bare, 0), undefined);
    assert.deepEqual(importedTypeCompletions(bare), []);
    const source = 'import complex { Complex, ComplexError }; import numeric { Floating }; Complex<f32> value;';
    const selected = analyzeDocument(uri, source, 'main');
    assert.match(importedTypeDocumentation(selected, source.lastIndexOf('Complex<f32>'))!, /two matching floating/);
    assert.deepEqual(importedTypeCompletions(selected).map(item => item.name).sort(), ['Complex', 'ComplexError', 'Floating']);
    assert.match(namespaceAPIs(selected, 'Complex')!.div, /ZeroDivisor/);
    assert.equal(namespaceAPIs(selected, 'complex'), undefined);
    const qualifiedSource = 'import complex; complex.Complex<f32> value; complex.complex alias;';
    const qualified = analyzeDocument(uri, qualifiedSource, 'main');
    assert.deepEqual(importedTypeCompletions(qualified), []);
    assert.match(importedTypeDocumentation(qualified, qualifiedSource.indexOf('Complex<f32>'))!, /Complex<T = f64>/);
    assert.match(importedTypeDocumentation(qualified, qualifiedSource.indexOf('complex alias'))!, /Alias/);
    assert.match(namespaceAPIs(qualified, 'complex.complex')!.magnitude, /stable magnitude/);
});
