// ========================
//
// Created By : miruku_sora2026
//
// File name : language.ts
//
// Description :
//     주석과 문자열을 구분하는 토큰 및 보수적인 스코프 인덱스를 제공한다.
//     Provide comment-aware tokens and a conservative lexical scope index.
//
// ========================

export type TokenKind = 'identifier' | 'number' | 'string' | 'comment' | 'symbol';
export interface Token { kind: TokenKind; text: string; start: number; end: number }
export type SymbolKind = 'struct' | 'enum' | 'interface' | 'typeAlias' | 'function' | 'method' | 'variable' | 'constant' | 'field' | 'parameter' | 'typeParameter' | 'variant';
export interface LanguageSymbol {
    id: string; uri: string; name: string; kind: SymbolKind; type: string;
    start: number; end: number; declarationStart: number; declarationEnd: number;
    scope: number; memberScope?: number; owner?: string; public: boolean;
    signature: string; documentation: string; mutable: boolean;
}
export interface Scope { id: number; start: number; end: number; parent?: number; owner?: string }
export interface ImportRecord {
    path: string; names?: string[]; wildcard: boolean; public: boolean;
    start: number; end: number; selections: { name: string; start: number; end: number }[];
}
export interface DocumentAnalysis {
    uri: string; text: string; tokens: Token[]; code: Token[]; scopes: Scope[];
    symbols: LanguageSymbol[]; imports: ImportRecord[];
    references: Map<number, string[]>; declarations: Map<number, string>; nonReferences: Set<number>;
    moduleName?: string;
}
export interface WorkspaceIndex { documents: Map<string, DocumentAnalysis>; modules: Map<string, DocumentAnalysis[]> }

export const keywords = [
    'import', 'public', 'private', 'type', 'struct', 'enum', 'interface', 'implements', 'where',
    'override', 'mut', 'const', 'persist', 'rsn', 'dyn', 'extern', 'if', 'else', 'while',
    'for', 'in', 'switch', 'case', 'default', 'break', 'continue', 'return', 'move', 'true', 'false'
] as const;
const reserved = new Set<string>(keywords);
export const builtinDocs: Readonly<Record<string, string>> = {
    i8: 'Signed 8-bit integer: -128 to 127.', i16: 'Signed 16-bit integer: -32768 to 32767.',
    i32: 'Signed 32-bit integer. The default integer type.', i64: 'Signed 64-bit integer.',
    i128: 'Signed 128-bit integer with checked arithmetic on every supported 64-bit target.',
    u8: 'Unsigned 8-bit integer: 0 to 255.', u16: 'Unsigned 16-bit integer.', u32: 'Unsigned 32-bit integer.', u64: 'Unsigned 64-bit integer.',
    u128: 'Unsigned 128-bit integer: 0 to 340282366920938463463374607431768211455.',
    isize: 'Signed pointer-sized integer; 64 bits on supported Aevix targets.', usize: 'Unsigned pointer-sized integer; 64 bits on supported Aevix targets.',
    int: 'Alias of i32.', f16: 'IEEE binary16 floating-point value.', bf16: 'Bfloat16 floating-point value, distinct from f16.',
    f32: 'IEEE binary32 floating-point value.', f64: 'IEEE binary64 floating-point value. The default floating-point type.', float: 'Alias of f64.',
    bool: 'Boolean value: true or false.', char: 'One Unicode scalar value.', byte: 'An 8-bit byte, distinct from u8.',
    string: 'An owned UTF-8 string. from_view/from_utf8 allocate explicitly and return Result<string, TextError>. Mutation requires an exclusive borrow.', string_view: 'An allocation-free borrowed UTF-8 string view. String literals are string_view values; byte lengths and Unicode scalar counts are distinct.',
    void: 'A function with no return value.', unit: 'A value type with one possible value.', never: 'A computation that does not return.',
    rsn: 'Infer a binding type from its initializer. Example: rsn value = 10;',
    Array: 'A fixed-size array: Array<T, N>. Declaration shorthand: i32 values[3] = [1, 2, 3];',
    Vector: 'A growable native owner. i32 values[] = [1, 2]?; uses a CollectionError Result; nested [] forms jagged vectors. Growth requires an exclusive borrow.',
    Slice: 'A borrowed read-only sequence of contiguous elements.', MutSlice: 'A borrowed mutable sequence with exclusive access.',
    Tuple: 'An owned collection of different field types. Tuple.make creates a value; get<I>/get_mut<I> borrow a compile-time field index.', Range: 'An allocation-free integer range; start..end is half-open, start..=end is inclusive. Iteration safely stops at integer endpoints.',
    Optional: 'A value that is present or absent; no unchecked null dereference.', Result: 'A success value or an error value. Postfix ? propagates failure.',
    CollectionError: 'Core collection error: AllocationFailure or SizeOverflow. Inspect its Result or propagate the exact error type with ?.',
    InputError: 'InputError<T> preserves an accepted input owner on AllocationFailure or SizeOverflow. Used by Vector.push and Box.new.',
    RangeError: 'Core range error: InvalidStep or CountOverflow. InvalidStep rejects zero step; len can fail even when iteration is valid.',
    TextError: 'UTF-8 text creation and mutation error; allocation and invalid UTF-8 or byte boundaries are explicit Result failures.',
    BoundsError: 'A checked slice operation rejected an invalid start or end index.',
    IntegerError: 'A checked integer conversion rejected an out-of-range input.',
    FloatError: 'A checked floating-point conversion rejected an unsupported or inexact value.',
    Integer: 'Integer conversion namespace. convert<Destination>(value) returns a checked Result.',
    Float: 'Floating-point conversion namespace. convert permits rounding; convert_exact and to_integer_exact require exact values.',
    Fn: 'Callable constraint Fn<Tuple<Arguments...>, Output>: a closure callable through a shared borrow.',
    FnMut: 'Callable constraint FnMut<Tuple<Arguments...>, Output>: a closure callable through an exclusive borrow.',
    FnOnce: 'Callable constraint FnOnce<Tuple<Arguments...>, Output>: a closure callable by consuming its environment.',
    Self: 'The concrete implementing type within an interface.', Copy: 'Explicit permission for implicit bitwise value copying; every field must also support Copy.'
};

export const keywordDocs: Readonly<Record<string, string>> = {
    type: 'Declare a transparent type alias: type UserId = u64; The alias retains the original type identity.',
    mut: 'Bindings are immutable by default. mut explicitly permits mutation.',
    const: 'A compile-time constant whose initializer must be evaluable at compile time.',
    persist: 'Persistent storage. Module values initialize before entry; local values initialize on first use.',
    public: 'Expose a module or type declaration. Local bindings keep lexical block scope.',
    private: 'Restrict a declaration to its module or declaring type. This is the default for module declarations and struct fields.',
    import: 'Import full module paths at the start of the file. import math.vector { Point }; exposes Point directly. Conflicting short names require full paths.',
    implements: 'Declare interface implementations. Static dispatch is the default.',
    where: 'Generic constraints follow the parameter list: T copy<T>(T value) where T implements Copy { return value; }',
    override: 'Required when replacing an interface default implementation or resolving conflicting defaults.',
    dyn: 'Explicit dynamic interface view: &dyn Named for shared reads, &mut dyn Named for exclusive mutation. Only dynamically callable methods are available; Box<dyn Named> requires import memory { Box };.',
    move: 'Explicit ownership consumption, for example switch (move event). Copy types retain normal Copy semantics.',
    switch: 'A statement with isolated case scopes. Enum cases must be exhaustive or contain default. Write break explicitly.',
    case: 'An enum payload pattern may bind named fields: case Event.Move { i32 dx = x }: ... break;',
    for: 'Supports C-style for (...; ...; ...), enhanced for (T value : values), and range for (T value in 0..10).',
    return: 'Return explicitly from every returning path in a non-void function. Returning a dangling borrow is rejected.',
    extern: 'Declare a C function with extern "C" ReturnType name(...);. The compiler enforces its supported ABI boundary.'
};

export const builtinAPIs: Readonly<Record<string, Readonly<Record<string, string>>>> = {
    string: {
        byte_len: 'Return the UTF-8 byte length of a string_view or borrowed string.',
        scalar_count: 'Count Unicode scalar values; this is not the count of displayed grapheme clusters.',
        is_empty: 'Test whether the UTF-8 view or borrowed owner contains no bytes.',
        view: 'Borrow a string as string_view without allocating or copying.',
        as_bytes: 'Borrow UTF-8 storage as a read-only Slice<byte>.',
        equals: 'Compare UTF-8 text without allocating.',
        from_view: 'Create an owned string from a view; returns Result<string, TextError>.',
        from_utf8: 'Validate and copy a Slice<byte> into an owned string; returns Result<string, TextError>.',
        slice_bytes: 'Check byte bounds and UTF-8 scalar boundaries; returns Result<string_view, TextError>.',
        append: 'Append a string_view through &mut string; returns Result<unit, TextError>.',
        push_char: 'Append one Unicode scalar through &mut string; returns Result<unit, TextError>.',
        clear: 'Clear an exclusively borrowed string while retaining its capacity.'
    },
    Tuple: {
        make: 'Create a Tuple from ordered values using normal move/Copy rules.',
        get: 'get<I>(&tuple) borrows a field with a compile-time checked index.',
        get_mut: 'get_mut<I>(&mut tuple) exclusively borrows a field with a compile-time checked index.',
        len: 'Return the number of fields in a borrowed tuple.',
        is_empty: 'Test whether a borrowed tuple has zero fields.',
        copy: 'Copy all fields; every field must implement Copy.'
    },
    Integer: { convert: 'convert<Destination>(value) performs a checked integer conversion and returns Result<Destination, IntegerError>.' },
    Float: {
        convert: 'convert<Destination>(value) converts to a float destination and explicitly permits rounding.',
        convert_exact: 'convert_exact<Destination>(value) requires exact representation in the float destination.',
        to_integer_exact: 'to_integer_exact<Destination>(value) requires a finite, integral, in-range float input.'
    },
    Never: { unreachable: 'Stop execution safely. This expression has type never and does not return.' }
};

const complexAPIs: Readonly<Record<string, string>> = {
    new: 'Construct matching real and imaginary components without allocating.',
    zero: 'Construct zero real and imaginary components.', one: 'Construct one plus zero imaginary.',
    imaginary_unit: 'Construct zero plus one imaginary.',
    add: 'Add matching complex components.', sub: 'Subtract matching complex components.',
    mul: 'Multiply two complex values using ordinary component-format operations.',
    neg: 'Negate both components.', conjugate: 'Negate the imaginary component.',
    eq: 'Compare both components using ordinary floating equality.',
    is_nan: 'Test whether either component is NaN.', is_finite: 'Test whether both components are finite.',
    norm_squared: 'Return real*real + imag*imag; ordinary intermediate rounding and overflow apply.',
    magnitude: 'Compute a stable magnitude through the numeric runtime and round to the component format.',
    norm: 'Return the magnitude.',
    div: 'Return Result<Self, ComplexError>; a zero divisor returns ZeroDivisor.',
    reciprocal: 'Return one divided by the input with explicit zero-divisor failure.'
};

const importedTypes: Readonly<Record<string, { module: string; description: string }>> = {
    Simd: { module: 'simd', description: 'Fixed CPU SIMD lanes up to 128 bits; requires an explicit simd import.' },
    SimdMask: { module: 'simd', description: 'One boolean per fixed CPU SIMD lane; requires an explicit simd import.' },
    Floating: { module: 'numeric', description: 'Sealed scalar-format capability for f16, bf16, f32 and f64. Import from numeric; user structs cannot implement it.' },
    Complex: { module: 'complex', description: 'Complex<T = f64> owns two matching floating components. Import explicitly from complex.' },
    complex: { module: 'complex', description: 'Alias of Complex<f64>; requires import complex { complex };.' },
    ComplexError: { module: 'complex', description: 'The imported complex error has ZeroDivisor; ordinary nonfinite floating results are not separate error variants.' }
};

const importedAPIs: Readonly<Record<string, { module: string; selection?: string; members: Readonly<Record<string, string>> }>> = {
    Complex: { module: 'complex', selection: 'Complex', members: complexAPIs },
    complex: { module: 'complex', selection: 'complex', members: complexAPIs },
    Simd: { module: 'simd', selection: 'Simd', members: {
        splat: 'Repeat one scalar into fixed CPU lanes without allocating.',
        from_array: 'Copy a fixed array into a matching SIMD value.',
        to_array: 'Copy SIMD lanes into a fixed array.',
        load: 'Read contiguous lanes from a borrowed fixed array after a bounds check.',
        store: 'Write contiguous lanes through an exclusive fixed-array borrow after a bounds check.',
        add: 'Add matching lanes; integer overflow stops execution.',
        sub: 'Subtract matching lanes; integer overflow stops execution.',
        mul: 'Multiply matching lanes; integer overflow stops execution.',
        div: 'Divide matching lanes; integer zero and signed overflow are checked.',
        eq: 'Compare matching lanes for equality and return a mask.',
        ne: 'Compare matching lanes for inequality and return a mask.',
        lt: 'Compare matching lanes for less-than and return a mask.',
        le: 'Compare matching lanes for less-than-or-equal and return a mask.',
        gt: 'Compare matching lanes for greater-than and return a mask.',
        ge: 'Compare matching lanes for greater-than-or-equal and return a mask.',
        select: 'Select lanes from two eagerly evaluated values using a matching mask.',
        sum: 'Fold lanes from left to right, checking every integer addition; no reassociation.',
        sqrt: 'Compute the floating-point square root in every lane.',
        extract: 'Read one scalar lane after checking its usize index.',
        lanes: 'Return the compile-time lane count as usize.'
    } },
    SimdMask: { module: 'simd', selection: 'SimdMask', members: {
        any: 'Return whether at least one lane is true.', all: 'Return whether every lane is true.',
        and: 'Combine matching boolean lanes with logical AND.', or: 'Combine matching boolean lanes with logical OR.',
        xor: 'Combine matching boolean lanes with logical XOR.', not: 'Invert each boolean lane.',
        lanes: 'Return the number of mask lanes as usize.'
    } },
    parallel: { module: 'parallel', members: {
        for_each: 'Visit disjoint scalar MutSlice elements with a validated named worker; join every worker before returning.',
        worker_count: 'Return the native worker limit, capped at 32. This does not predict a speedup.'
    } }
};

export function namespaceAPIs(doc: DocumentAnalysis, receiver: string): Readonly<Record<string, string>> | undefined {
    if (builtinAPIs[receiver]) return builtinAPIs[receiver];
    for (const [name, specification] of Object.entries(importedAPIs)) {
        const qualified = specification.selection ? `${specification.module}.${name}` : specification.module;
        if (receiver !== name && receiver !== qualified) continue;
        const imported = doc.imports.some(item => item.path === specification.module
            && (!specification.selection || (receiver === qualified && item.names === undefined)
                || item.wildcard || item.names?.includes(specification.selection)));
        if (imported) return specification.selection ? specification.members : Object.fromEntries(
            Object.entries(specification.members).filter(([member]) => doc.imports.some(item => item.path === specification.module
                && (item.names === undefined || item.wildcard || item.names.includes(member)))));
    }
    return undefined;
}

export function importedTypeDocumentation(doc: DocumentAnalysis, offset: number): string | undefined {
    const token = tokenAt(doc, offset);
    if (token?.kind !== 'identifier') return undefined;
    const entry = importedTypes[token.text];
    if (!entry) return undefined;
    const qualifier = /([A-Za-z_][A-Za-z_0-9.]*)\s*\.\s*$/.exec(doc.text.slice(0, token.start));
    const imported = doc.imports.some(item => item.path === entry.module
        && (!qualifier || qualifier[1] === entry.module)
        && ((qualifier && item.names === undefined) || item.wildcard || item.names?.includes(token.text)));
    return imported ? entry.description : undefined;
}

export function importedTypeCompletions(doc: DocumentAnalysis): { name: string; description: string }[] {
    return Object.entries(importedTypes).filter(([name, entry]) => doc.imports.some(item => item.path === entry.module
        && (item.wildcard || item.names?.includes(name)))).map(([name, entry]) => ({ name, description: entry.description }));
}

export function builtinMemberDocumentation(doc: DocumentAnalysis, offset: number): string | undefined {
    const token = tokenAt(doc, offset);
    if (token?.kind !== 'identifier') return undefined;
    const receiver = /([A-Za-z_][A-Za-z_0-9]*(?:\.[A-Za-z_][A-Za-z_0-9]*)*)\s*\.\s*$/.exec(doc.text.slice(0, token.start));
    return receiver ? namespaceAPIs(doc, receiver[1])?.[token.text] : undefined;
}

// UTF-16 오프셋을 유지해 VS Code 위치와 일치시킨다.
// Preserve UTF-16 offsets so editor positions match the original document.
export function tokenize(text: string): Token[] {
    const result: Token[] = [];
    const push = (kind: TokenKind, start: number, end: number) => result.push({ kind, start, end, text: text.slice(start, end) });
    let at = 0;
    while (at < text.length) {
        if (/\s/.test(text[at])) { ++at; continue; }
        const start = at;
        if (text.startsWith('//', at)) {
            at = text.indexOf('\n', at + 2); if (at < 0) { at = text.length; }
            push('comment', start, at); continue;
        }
        if (text.startsWith('/*', at)) {
            at += 2;
            while (at < text.length && !text.startsWith('*/', at)) { ++at; }
            if (at < text.length) { at += 2; }
            push('comment', start, at); continue;
        }
        if (text[at] === '"' || text[at] === "'") {
            const quote = text[at++];
            while (at < text.length) {
                if (text[at] === '\\') { at = Math.min(at + 2, text.length); }
                else if (text[at++] === quote || text[at - 1] === '\n') { break; }
            }
            push('string', start, at); continue;
        }
        if (/[A-Za-z_]/.test(text[at])) {
            while (at < text.length && /[A-Za-z_0-9]/.test(text[at])) { ++at; }
            push('identifier', start, at); continue;
        }
        if (/[0-9]/.test(text[at])) {
            while (at < text.length && /[A-Za-z_0-9]/.test(text[at])) { ++at; }
            if (text[at] === '.' && text[at + 1] !== '.') {
                ++at; while (at < text.length && /[A-Za-z_0-9]/.test(text[at])) { ++at; }
            }
            if (/[eE]/.test(text[at - 1] ?? '') && /[+-]/.test(text[at] ?? '')) {
                ++at; while (at < text.length && /[A-Za-z_0-9]/.test(text[at])) { ++at; }
            }
            push('number', start, at); continue;
        }
        const operator = ['..=', '<<=', '>>=', '++', '--', '..', '==', '!=', '<=', '>=', '&&', '||', '<<', '>>', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^='].find(value => text.startsWith(value, at));
        at += operator?.length ?? 1; push('symbol', start, at);
    }
    return result;
}

function angleEnd(tokens: Token[], begin: number): number {
    let depth = 0;
    for (let i = begin; i < tokens.length; ++i) {
        if (tokens[i].text === '<') { ++depth; }
        else if (tokens[i].text === '>') { --depth; }
        else if (tokens[i].text === '>>') { depth -= 2; }
        if (depth <= 0) { return i; }
        if ([';', '{', '}'].includes(tokens[i].text)) { return -1; }
    }
    return -1;
}
function typeEnd(tokens: Token[], begin: number): number {
    let at = begin;
    if (tokens[at]?.text === '&') { ++at; if (tokens[at]?.text === 'mut') { ++at; } }
    if (tokens[at]?.text === 'dyn') { ++at; }
    if (tokens[at]?.kind !== 'identifier' || (reserved.has(tokens[at].text) && tokens[at].text !== 'rsn')) { return begin; }
    ++at;
    if (tokens[at]?.text === '<') { const end = angleEnd(tokens, at); if (end < 0) { return begin; } at = end + 1; }
    while (tokens[at]?.text === '.' && tokens[at + 1]?.kind === 'identifier') {
        at += 2;
        if (tokens[at]?.text === '<') { const end = angleEnd(tokens, at); if (end < 0) { return begin; } at = end + 1; }
    }
    return at;
}
function matchingPairs(code: Token[]): Map<number, number> {
    const pairs = new Map<number, number>(); const stack: { index: number; close: string }[] = [];
    const close: Record<string, string> = { '(': ')', '[': ']', '{': '}' };
    for (let i = 0; i < code.length; ++i) {
        if (close[code[i].text]) { stack.push({ index: i, close: close[code[i].text] }); }
        else if ([')', ']', '}'].includes(code[i].text)) {
            if (stack.at(-1)?.close !== code[i].text) { continue; }
            const open = stack.pop()!; pairs.set(open.index, i); pairs.set(i, open.index);
        }
    }
    return pairs;
}
export function tokenAt(analysis: DocumentAnalysis, offset: number): Token | undefined {
    let low = 0, high = analysis.tokens.length;
    while (low < high) { const mid = (low + high) >>> 1; if (analysis.tokens[mid].end <= offset) { low = mid + 1; } else { high = mid; } }
    const token = analysis.tokens[low]; return token && token.start <= offset && offset < token.end ? token : undefined;
}
export function scopeAt(analysis: Pick<DocumentAnalysis, 'scopes'>, offset: number): number {
    let selected = 0;
    for (const scope of analysis.scopes) {
        if (scope.start <= offset && offset < scope.end && scope.start >= analysis.scopes[selected].start && scope.end <= analysis.scopes[selected].end) { selected = scope.id; }
    }
    return selected;
}
function normalizedType(text: string): string { return text.replace(/\s+/g, ' ').replace(/\s*([<>,.])\s*/g, '$1').trim(); }
export function baseType(type: string): string { return type.replace(/^&\s*(mut\s+)?/, '').replace(/^dyn\s+/, '').split('<')[0].replace(/\[.*$/, '').trim(); }
function docsBefore(tokens: Token[], text: string, offset: number): string {
    let comment: Token | undefined;
    for (const token of tokens) { if (token.end <= offset && token.kind === 'comment') { comment = token; } else if (token.start >= offset) { break; } }
    if (!comment || text.slice(comment.end, offset).trim() || !comment.text.startsWith('/*')) { return ''; }
    return comment.text.replace(/^\/\*+|\*\/$/g, '').split('\n').map(line => line.replace(/^\s*\* ?/, '')).join('\n').trim();
}

// 불완전한 편집 중에도 선언과 블록을 인덱싱하고, 모호한 바인딩은 확정하지 않는다.
// Index declarations and blocks in incomplete edits without guessing ambiguous bindings.
export function analyzeDocument(uri: string, text: string, moduleName?: string): DocumentAnalysis {
    const tokens = tokenize(text); const code = tokens.filter(token => token.kind !== 'comment');
    const pairs = matchingPairs(code); const scopes: Scope[] = [{ id: 0, start: 0, end: text.length + 1 }];
    const braceScopes = new Map<number, number>(); const stack = [0];
    for (let i = 0; i < code.length; ++i) {
        if (code[i].text === '{') {
            const id = scopes.length; const close = pairs.get(i);
            scopes.push({ id, start: code[i].end, end: close === undefined ? text.length + 1 : code[close].start, parent: stack.at(-1) });
            braceScopes.set(i, id); stack.push(id);
        } else if (code[i].text === '}' && stack.length > 1) { stack.pop(); }
    }
    const result: DocumentAnalysis = { uri, text, tokens, code, scopes, symbols: [], imports: [], references: new Map(), declarations: new Map(), nonReferences: new Set(), moduleName };
    const skipped = new Set<number>(); const memberOwners = new Map<number, LanguageSymbol>();
    const add = (nameAt: number, kind: SymbolKind, type: string, scope: number, begin: number, end: number, flags: string[], signatureEnd?: number): LanguageSymbol => {
        const name = code[nameAt]; const owner = memberOwners.get(scope);
        const symbol: LanguageSymbol = {
            id: `${uri}#${name.start}`, uri, name: name.text, kind, type, start: name.start, end: name.end,
            declarationStart: code[begin].start, declarationEnd: end, scope, owner: owner?.id,
            public: flags.includes('public') || (kind === 'variant' && !flags.includes('private')) || owner?.kind === 'interface',
            mutable: flags.includes('mut'), signature: text.slice(code[begin].start, signatureEnd ?? end).trim().replace(/\s+/g, ' '),
            documentation: docsBefore(tokens, text, code[begin].start)
        };
        result.symbols.push(symbol); result.declarations.set(name.start, symbol.id); return symbol;
    };
    const modifiers = new Set(['public', 'private', 'override', 'mut', 'const', 'persist', 'extern']);
    const prefix = (begin: number): { at: number; flags: string[] } => {
        let at = begin; const flags: string[] = [];
        while (modifiers.has(code[at]?.text)) { flags.push(code[at++].text); if (flags.at(-1) === 'extern' && code[at]?.kind === 'string') { ++at; } }
        return { at, flags };
    };
    const addTypeParameters = (begin: number, scope: number) => {
        if (code[begin]?.text !== '<') { return; }
        const end = angleEnd(code, begin); if (end < 0) { return; }
        for (let i = begin + 1; i < end;) {
            const constant = code[i].text === 'const';
            const nameAt = constant ? typeEnd(code, i + 1) : i;
            if (nameAt < end && code[nameAt].kind === 'identifier' && !reserved.has(code[nameAt].text)) {
                add(nameAt, 'typeParameter', constant ? normalizedType(text.slice(code[i + 1].start, code[nameAt].start)) : code[nameAt].text,
                    scope, i, code[nameAt].end, constant ? ['const'] : []);
                skipped.add(nameAt);
            }
            let next = nameAt + 1;
            while (next < end && code[next].text !== ',') {
                if (['(', '[', '{'].includes(code[next].text) && pairs.has(next)) next = pairs.get(next)! + 1;
                else if (code[next].text === '<' && angleEnd(code, next) >= next) next = angleEnd(code, next) + 1;
                else ++next;
            }
            i = next + 1;
        }
    };
    for (let i = 0; i < code.length; ++i) {
        if (skipped.has(i)) { continue; }
        const prior = code[i - 1]?.text;
        const isStart = i === 0 || [';', '{', '}', ':'].includes(prior) || (prior === '(' && code[i - 2]?.text === 'for');
        if (!isStart) { continue; }
        const { at, flags } = prefix(i); const first = code[at]; if (!first) { continue; }
        if (first.text === 'import') {
            let next = at + 1; const pieces: string[] = [];
            while (code[next]?.kind === 'identifier') {
                pieces.push(code[next].text); skipped.add(next++);
                if (code[next]?.text !== '.') { break; } skipped.add(next++);
            }
            const selections: ImportRecord['selections'] = []; let wildcard = false; let names: string[] | undefined;
            if (code[next]?.text === '{') {
                names = []; ++next;
                while (next < code.length && code[next].text !== '}' && code[next].text !== ';') {
                    skipped.add(next);
                    if (code[next].text === '*') { wildcard = true; }
                    if (code[next].kind === 'identifier') { names.push(code[next].text); selections.push({ name: code[next].text, start: code[next].start, end: code[next].end }); }
                    ++next;
                }
                if (code[next]?.text === '}') { ++next; }
            }
            if (pieces.length > 0) { result.imports.push({ path: pieces.join('.'), names, wildcard, public: flags.includes('public'), start: code[i].start, end: code[next]?.end ?? text.length, selections }); }
            for (let j = i; j <= next; ++j) { skipped.add(j); } continue;
        }
        if (first.text === 'type' && code[at + 1]?.kind === 'identifier' && code[at + 2]?.text === '=') {
            let end = at + 3;
            while (end < code.length && code[end].text !== ';') ++end;
            if (end === at + 3 || !code[end]) continue;
            add(at + 1, 'typeAlias', normalizedType(text.slice(code[at + 3].start, code[end].start)),
                scopeAt(result, first.start), i, code[end].end, flags);
            skipped.add(at + 1);
            continue;
        }
        if (['struct', 'enum', 'interface'].includes(first.text) && code[at + 1]?.kind === 'identifier') {
            let body = at + 2; while (body < code.length && !['{', ';'].includes(code[body].text)) { ++body; }
            const close = pairs.get(body); const scope = scopeAt(result, first.start);
            const symbol = add(at + 1, first.text as SymbolKind, code[at + 1].text, scope, i, close === undefined ? code[at + 1].end : code[close].end, flags, code[body]?.start);
            if (braceScopes.has(body)) { symbol.memberScope = braceScopes.get(body); scopes[symbol.memberScope!].start = code[i].start; scopes[symbol.memberScope!].owner = symbol.id; memberOwners.set(symbol.memberScope!, symbol); addTypeParameters(at + 2, symbol.memberScope!); }
            skipped.add(at + 1); continue;
        }
        const scope = scopeAt(result, first.start); const owner = memberOwners.get(scope);
        if (owner?.kind === 'enum' && first.kind === 'identifier' && [';', '{'].includes(code[at + 1]?.text)) {
            const close = pairs.get(at + 1); const symbol = add(at, 'variant', owner.name, scope, i, close === undefined ? code[at + 1].end : code[close].end, flags, code[at + 1].start);
            if (braceScopes.has(at + 1)) { symbol.memberScope = braceScopes.get(at + 1); scopes[symbol.memberScope!].owner = symbol.id; memberOwners.set(symbol.memberScope!, symbol); }
            continue;
        }
        const nameAt = typeEnd(code, at); const name = code[nameAt];
        if (nameAt === at || name?.kind !== 'identifier' || reserved.has(name.text)) { continue; }
        let after = nameAt + 1;
        if (code[after]?.text === '<') { const close = angleEnd(code, after); if (close < 0) { continue; } after = close + 1; }
        const type = normalizedType(text.slice(first.start, name.start));
        if (code[after]?.text === '(' && pairs.has(after)) {
            const paramsEnd = pairs.get(after)!; let body = paramsEnd + 1;
            while (body < code.length && !['{', ';', '=', '}'].includes(code[body].text)) { ++body; }
            if (!['{', ';'].includes(code[body]?.text)) { continue; }
            const bodyEnd = pairs.get(body); const symbol = add(nameAt, owner ? 'method' : 'function', type, scope, i, bodyEnd === undefined ? code[body].end : code[bodyEnd].end, flags, code[body].start);
            const functionScope = braceScopes.get(body) ?? scopes.push({ id: scopes.length, start: code[after].start, end: code[body].end, parent: scope }) - 1;
            symbol.memberScope = functionScope; scopes[functionScope].start = code[i].start; scopes[functionScope].owner = symbol.id;
            // 매개변수와 제네릭 이름은 본문의 스코프에만 연결한다.
            // Bind parameters and generic names to this function scope only.
            addTypeParameters(nameAt + 1, functionScope);
            let param = after + 1;
            while (param < paramsEnd) {
                let p = param; const paramFlags: string[] = [];
                if (code[p]?.text === 'mut') { paramFlags.push('mut'); ++p; }
                const n = typeEnd(code, p);
                if (n > p && code[n]?.kind === 'identifier') {
                    add(n, 'parameter', normalizedType(text.slice(code[p].start, code[n].start)), functionScope, param, code[n].end, paramFlags);
                }
                let cursor = n > p ? n + 1 : p + 1;
                while (cursor < paramsEnd && code[cursor].text !== ',') {
                    if (pairs.has(cursor) && ['(', '[', '{'].includes(code[cursor].text)) { cursor = pairs.get(cursor)! + 1; }
                    else if (code[cursor].text === '<' && angleEnd(code, cursor) >= cursor) { cursor = angleEnd(code, cursor) + 1; }
                    else { ++cursor; }
                }
                param = cursor + 1;
            }
            for (let j = i; j <= paramsEnd; ++j) { skipped.add(j); } continue;
        }
        if (!['=', ';', '[', ':', 'in'].includes(code[nameAt + 1]?.text)) { continue; }
        let end = nameAt + 1;
        while (end < code.length && ![';', ':'].includes(code[end].text)) {
            if (['[', '(', '{'].includes(code[end].text) && pairs.has(end)) { end = pairs.get(end)! + 1; } else { ++end; }
        }
        add(nameAt, owner ? 'field' : flags.includes('const') ? 'constant' : 'variable', type, scope, i, code[end]?.end ?? name.end, flags);
        for (let j = i; j <= nameAt; ++j) { skipped.add(j); }
    }
    // 람다의 명시적 매개변수는 해당 본문에서만 선언으로 연결한다.
    // Bind explicit lambda parameters only within their own body scope.
    for (let i = 1; i < code.length; ++i) {
        if (code[i].text !== '(' || code[i - 1].text !== ']' || !pairs.has(i)) continue;
        const end = pairs.get(i)!;
        if (code[end + 1]?.text !== '->' && !(code[end + 1]?.text === '-' && code[end + 2]?.text === '>')) continue;
        let body = end + (code[end + 1].text === '->' ? 2 : 3);
        while (body < code.length && !['{', ';'].includes(code[body].text)) ++body;
        const scope = braceScopes.get(body);
        if (scope === undefined) continue;
        scopes[scope].start = code[i].start;
        for (let param = i + 1; param < end;) {
            const start = code[param].text === 'mut' ? param + 1 : param;
            const nameAt = typeEnd(code, start);
            if (nameAt > start && nameAt < end && code[nameAt].kind === 'identifier') {
                add(nameAt, 'parameter', normalizedType(text.slice(code[start].start, code[nameAt].start)),
                    scope, param, code[nameAt].end, start === param ? [] : ['mut']);
            }
            let next = Math.max(nameAt + 1, start + 1);
            while (next < end && code[next].text !== ',') {
                if (['(', '[', '{'].includes(code[next].text) && pairs.has(next)) next = pairs.get(next)! + 1;
                else ++next;
            }
            param = next + 1;
        }
    }
    // case의 필드 바인딩은 패턴 중괄호가 아니라 해당 분기 끝까지 살아 있다.
    // Case payload bindings live through the branch rather than just the pattern braces.
    for (let i = 0; i < code.length; ++i) {
        if (code[i].text !== 'case' && code[i].text !== 'default') { continue; }
        const parent = scopeAt(result, code[i].start); let colon = i + 1;
        while (colon < code.length && code[colon].text !== ':') { if (code[colon].text === '{' && pairs.has(colon)) { colon = pairs.get(colon)! + 1; } else { ++colon; } }
        if (colon >= code.length) { continue; }
        let end = colon + 1;
        while (end < code.length && code[end].start < scopes[parent].end && !(scopeAt(result, code[end].start) === parent && ['case', 'default'].includes(code[end].text))) { ++end; }
        const branch: Scope = { id: scopes.length, start: code[i].start, end: code[end]?.start ?? scopes[parent].end, parent };
        scopes.push(branch);
        for (const child of scopes) { if (child.id !== branch.id && child.parent === parent && child.start > branch.start && child.end <= branch.end) { child.parent = branch.id; } }
        for (let j = i + 1; j < colon; ++j) {
            const n = typeEnd(code, j);
            if (n > j && code[n]?.kind === 'identifier' && code[n + 1]?.text === '=') {
                const existing = result.symbols.find(s => s.start === code[n].start);
                if (existing) { existing.scope = branch.id; }
                else { add(n, 'variable', normalizedType(text.slice(code[j].start, code[n].start)), branch.id, j, code[n].end, []); }
                if (code[n + 2]) { skipped.add(n + 2); result.nonReferences.add(code[n + 2].start); }
                j = n + 2;
            }
        }
        for (const symbol of result.symbols) {
            if (symbol.start > code[colon].end && symbol.start < branch.end && symbol.scope === parent) { symbol.scope = branch.id; }
        }
    }
    for (let i = 0; i < code.length; ++i) {
        if (code[i].text !== 'for' || code[i + 1]?.text !== '(' || !pairs.has(i + 1)) { continue; }
        const close = pairs.get(i + 1)!; let bodyEnd = close + 1;
        if (code[bodyEnd]?.text === '{' && pairs.has(bodyEnd)) { bodyEnd = pairs.get(bodyEnd)!; }
        else { while (bodyEnd < code.length && code[bodyEnd].text !== ';') { ++bodyEnd; } }
        const parent = scopeAt(result, code[i].start);
        const loop: Scope = { id: scopes.length, start: code[i].start, end: code[bodyEnd]?.end ?? text.length, parent };
        scopes.push(loop);
        for (const child of scopes) { if (child.id !== loop.id && child.parent === parent && child.start > loop.start && child.end <= loop.end) { child.parent = loop.id; } }
        for (const symbol of result.symbols) { if (symbol.scope === parent && symbol.start > loop.start && symbol.start < loop.end) { symbol.scope = loop.id; } }
    }
    rebuildReferences(result); return result;
}

function localCandidates(doc: DocumentAnalysis, name: string, offset: number): LanguageSymbol[] {
    let scope: number | undefined = scopeAt(doc, offset);
    while (scope !== undefined) {
        const candidates = doc.symbols.filter(symbol => symbol.scope === scope && symbol.name === name && (symbol.kind !== 'variable' || symbol.start <= offset));
        if (candidates.length) { return candidates; }
        scope = doc.scopes[scope].parent;
    }
    return [];
}
function publicExports(doc: DocumentAnalysis, name: string, index: WorkspaceIndex, seen = new Set<string>()): LanguageSymbol[] {
    const key = `${doc.uri}:${name}`; if (seen.has(key)) { return []; } seen.add(key);
    const own = doc.symbols.filter(s => s.scope === 0 && s.public && s.name === name);
    for (const item of doc.imports) {
        if (item.public && item.names?.includes(name)) {
            for (const target of index.modules.get(item.path) ?? []) { own.push(...publicExports(target, name, index, seen)); }
        }
    }
    return [...new Map(own.map(s => [s.id, s])).values()];
}
function importedCandidates(doc: DocumentAnalysis, name: string, index: WorkspaceIndex): LanguageSymbol[] {
    const result: LanguageSymbol[] = [];
    for (const item of doc.imports) {
        if (!item.names?.includes(name) && !item.wildcard) { continue; }
        for (const target of index.modules.get(item.path) ?? []) { result.push(...publicExports(target, name, index)); }
    }
    return result;
}
function qualifiedCandidates(doc: DocumentAnalysis, tokenIndex: number, index: WorkspaceIndex): LanguageSymbol[] {
    const chain = [doc.code[tokenIndex].text]; let begin = tokenIndex;
    while (begin >= 2 && doc.code[begin - 1].text === '.' && doc.code[begin - 2].kind === 'identifier') { begin -= 2; chain.unshift(doc.code[begin].text); }
    if (chain.length < 2) { return []; }
    const path = chain.slice(0, -1).join('.'); const name = chain.at(-1)!;
    const imported = doc.imports.find(item => item.path === path && (item.names === undefined || item.wildcard || item.names.includes(name)));
    if (imported || path === doc.moduleName) {
        return (index.modules.get(path) ?? []).flatMap(target => path === doc.moduleName ? target.symbols.filter(s => s.scope === 0 && s.name === name) : publicExports(target, name, index));
    }
    return [];
}
export function canAccessMember(definition: DocumentAnalysis, member: LanguageSymbol, requester: DocumentAnalysis, offset: number): boolean {
    if (member.public) { return true; }
    if (definition.uri !== requester.uri) { return false; }
    let ownerId = member.owner;
    while (ownerId) {
        const owner = definition.symbols.find(symbol => symbol.id === ownerId);
        if (!owner) { break; }
        const scope = owner.memberScope === undefined ? undefined : definition.scopes[owner.memberScope];
        if (scope && scope.start <= offset && offset < scope.end) { return true; }
        ownerId = owner.owner;
    }
    return false;
}
function members(doc: DocumentAnalysis, tokenIndex: number, index?: WorkspaceIndex): LanguageSymbol[] {
    if (doc.code[tokenIndex - 1]?.text !== '.' || doc.code[tokenIndex - 2]?.kind !== 'identifier') { return []; }
    const receiver = doc.code[tokenIndex - 2]; const name = doc.code[tokenIndex].text;
    let targets = localCandidates(doc, receiver.text, receiver.start);
    if (index) { targets.push(...importedCandidates(doc, receiver.text, index)); }
    const declarations = [doc, ...(index ? [...index.documents.values()].filter(d => d.uri !== doc.uri) : [])];
    const out: LanguageSymbol[] = [];
    for (const target of targets) {
        const typeName = ['struct', 'enum', 'interface', 'variant'].includes(target.kind) ? target.name : baseType(target.type);
        let types = localCandidates(doc, typeName, receiver.start);
        if (index) { types.push(...importedCandidates(doc, typeName, index)); }
        if (['struct', 'enum', 'interface', 'variant'].includes(target.kind)) { types = [target]; }
        for (const type of types) {
            if (type.memberScope === undefined) { continue; }
            const definition = declarations.find(d => d.uri === type.uri);
            if (definition) { out.push(...definition.symbols.filter(s => s.scope === type.memberScope && s.name === name && canAccessMember(definition, s, doc, doc.code[tokenIndex].start))); }
        }
    }
    return out;
}
export function buildIndex(documents: DocumentAnalysis[]): WorkspaceIndex {
    const index: WorkspaceIndex = { documents: new Map(), modules: new Map() };
    for (const document of documents) {
        index.documents.set(document.uri, document);
        if (document.moduleName) { const group = index.modules.get(document.moduleName) ?? []; group.push(document); index.modules.set(document.moduleName, group); }
    }
    for (const document of documents) { rebuildReferences(document, index); }
    return index;
}
function isNamedLabel(code: Token[], index: number): boolean {
    if (code[index + 1]?.text !== '=' || ![',', '{', '('].includes(code[index - 1]?.text)) { return false; }
    let depth = 0;
    for (let i = index - 1; i >= 0; --i) {
        if ([')', ']', '}'].includes(code[i].text)) { ++depth; }
        else if (['(', '[', '{'].includes(code[i].text)) {
            if (depth > 0) { --depth; continue; }
            const before = code[i - 1];
            if (!before || code[i].text === '[') { return false; }
            return (before.kind === 'identifier' && !reserved.has(before.text)) || before.text === '>' || before.text === '>>';
        } else if (code[i].text === ';' && depth === 0) { return false; }
    }
    return false;
}
export function rebuildReferences(doc: DocumentAnalysis, index?: WorkspaceIndex): void {
    doc.references.clear();
    for (let i = 0; i < doc.code.length; ++i) {
        const token = doc.code[i]; if (token.kind !== 'identifier' || reserved.has(token.text) || doc.nonReferences.has(token.start)) { continue; }
        const declared = doc.declarations.get(token.start);
        if (declared) { doc.references.set(token.start, [declared]); continue; }
        const item = doc.imports.find(imp => token.start >= imp.start && token.end <= imp.end);
        if (item) {
            if (index && item.selections.some(s => s.start === token.start)) {
                doc.references.set(token.start, [...new Set((index.modules.get(item.path) ?? []).flatMap(target => publicExports(target, token.text, index)).map(s => s.id))]);
            }
            continue;
        }
        // 필드 이름·이름 지정 인수는 지역 변수 참조로 잘못 바꾸지 않는다.
        // Never rename field labels or named arguments as if they were local uses.
        if (isNamedLabel(doc.code, i) && !doc.declarations.has(token.start)) { continue; }
        let found: LanguageSymbol[];
        if (doc.code[i - 1]?.text === '.') { found = index ? qualifiedCandidates(doc, i, index) : []; if (!found.length) { found = members(doc, i, index); } }
        else { found = localCandidates(doc, token.text, token.start); if (index) { found.push(...importedCandidates(doc, token.text, index)); } }
        doc.references.set(token.start, [...new Set(found.map(s => s.id))]);
    }
}
export function symbolAt(doc: DocumentAnalysis, offset: number, index?: WorkspaceIndex): LanguageSymbol | undefined {
    const token = tokenAt(doc, offset); if (!token || token.kind !== 'identifier') { return undefined; }
    const ids = doc.references.get(token.start); if (ids?.length !== 1) { return undefined; }
    for (const document of index?.documents.values() ?? [doc]) { const symbol = document.symbols.find(s => s.id === ids[0]); if (symbol) { return symbol; } }
    return undefined;
}
export function referencesTo(index: WorkspaceIndex, id: string, includeDeclaration = true): { uri: string; start: number; end: number }[] {
    const found: { uri: string; start: number; end: number }[] = [];
    for (const doc of index.documents.values()) {
        for (const token of doc.code) {
            const ids = doc.references.get(token.start);
            if (ids?.length === 1 && ids[0] === id && (includeDeclaration || !doc.declarations.has(token.start))) { found.push({ uri: doc.uri, start: token.start, end: token.end }); }
        }
    }
    return found;
}
