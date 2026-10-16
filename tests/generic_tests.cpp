// ========================
//
// Created By : miruku_sora2026
//
// File name : generic_tests.cpp
//
// Description :
//     제네릭 구체화와 정적 인터페이스 계약을 실제 프런트엔드 파이프라인으로 검증한다.
//     Verify generic materialization and static interface contracts through the real frontend pipeline.
//
// ========================

#include "aevix/Frontend/Sema/GenericResolver.hpp"
#include "aevix/Frontend/Sema/CoreErrorTypes.hpp"
#include "aevix/Frontend/Sema/TypeChecker.hpp"
#include "aevix/Frontend/Sema/ControlFlowChecker.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
    unsigned checks = 0, failures = 0;
    void expect(bool condition, const char* message)
    {
        ++checks;
        if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    }
    struct Fixture
    {
        std::filesystem::path path;
        aevix::SourceManager sources;
        aevix::Diagnostics diagnostics;
        std::optional<aevix::ModuleGraph> graph;
        aevix::NameResolution names;
        aevix::TypeCheckResult types;
        bool genericStage = false;
        Fixture()
        {
            static unsigned serial = 0;
            path = std::filesystem::temp_directory_path() / ("aevix-generic-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
            std::filesystem::create_directories(path);
        }
        ~Fixture() { std::error_code error; std::filesystem::remove_all(path, error); }
        void write(const char* file, std::string_view text)
        {
            std::ofstream output(path / file); output << text;
            if (!output) throw std::runtime_error("cannot write generic test source");
        }
        bool run(std::string_view text)
        {
            write("main.avx", text);
            aevix::ModuleOptions options; options.sourceRoot = path;
            graph = aevix::ModuleLoader(sources, diagnostics).load(path / "main.avx", options);
            if (!graph || !diagnostics.empty()) return false;
            if (!aevix::installCoreErrorTypes(*graph, sources, diagnostics)) return false;
            names = aevix::NameResolver(diagnostics).resolve(*graph);
            if (!names.succeeded || !diagnostics.empty()) return false;
            genericStage = true;
            auto concrete = aevix::GenericResolver(diagnostics).resolve(*graph, names);
            if (!concrete) return false;
            names = std::move(*concrete);
            types = aevix::TypeChecker(diagnostics).check(*graph, names);
            if (!types.succeeded || !diagnostics.empty()) return false;
            return aevix::ControlFlowChecker(diagnostics).check(*graph, names, types) && diagnostics.empty();
        }
        bool contains(std::string_view text)
        {
            for (const auto& diagnostic : diagnostics) if (diagnostic.message.find(text) != std::string::npos) return true;
            return false;
        }
    };
    void accepts(std::string_view text, const char* message)
    {
        Fixture fixture;
        const bool passed = fixture.run(text);
        if (!passed) for (const auto& error : fixture.diagnostics) std::cerr << "  " << error.message << '\n';
        expect(passed, message);
    }
    void rejects(std::string_view text, std::string_view diagnostic, const char* message)
    {
        Fixture fixture;
        const bool passed = fixture.run(text);
        if (passed || !fixture.contains(diagnostic)) for (const auto& error : fixture.diagnostics) std::cerr << "  " << error.message << '\n';
        expect(!passed && fixture.genericStage && fixture.contains(diagnostic), message);
        for (const auto& error : fixture.diagnostics) expect(fixture.sources.contains(error.range), "generic failure has positioned diagnostic");
    }
}
int main()
{
    accepts("T identity<T>(T value) { return value; } i32 main() { return identity<i32>(7); }", "explicit generic function");
    accepts("T identity<T>(T value) { return value; } i32 main() { return identity(7); }", "inferred generic function");
    accepts("T identity<T>(T value) { return value; } i32 main() { i16 small = 7; i16 result = identity(small); return 0; }", "inference preserves typed arguments");
    accepts("T identity<T>(T value) { return value; } i32 main() { rsn value = identity(7); return identity(value); }", "inference follows rsn and nested calls");
    accepts("T choose<T>(T value, bool enabled = true) { return value; } i32 main() { return choose(enabled = true, value = 7); }", "named arguments and defaults in generic call");
    accepts("T read<T>(&T value) where T implements Copy { return *value; } i32 main() { i32 item = 7; return read(&item); }", "reference generic inference and Copy constraint");
    accepts("struct Pair<T> { public T first; public T second; }; i32 main() { Pair<i32> pair = Pair<i32> { first = 2, second = 3 }; return pair.first + pair.second; }", "generic struct and field access");
    accepts("struct Pair<T> implements Copy where T implements Copy { public T value; }; i32 main() { Pair<i32> pair = Pair<i32> { value = 3 }; return pair.value; }", "generic struct Copy constraint");
    accepts("struct Pair<T> where T implements Copy { public T value; public T get(&Self self) { return (*self).value; } }; i32 main() { Pair<i32> pair = Pair<i32> { value = 3 }; return pair.get(); }", "generic struct method Self substitution");
    accepts("struct Pair<T> where T implements Copy { T value; public Pair<T> new(T input) { return Pair<T> { value = input }; } public T get(&Self self) { return (*self).value; } }; i32 main() { Pair<i32> pair = Pair<i32>.new(7); return pair.get(); }", "generic struct factory preserves private initialization context");
    accepts("enum Outcome<T> { Value { public T item; }; Empty; }; i32 main() { Outcome<i32> value = Outcome<i32>.Value { item = 7 }; switch (value) { case Outcome<i32>.Value { i32 item = item }: return item; break; case Outcome<i32>.Empty: return 0; break; } }", "generic enum payload and exhaustive switch");
    accepts("interface Named { public i32 get(&Self self); }; struct Item implements Named { public i32 get(&Item self) { return 7; } }; i32 main() { Item item = Item {}; return item.get(); }", "static interface required method");
    accepts("interface Named { public i32 get(&Self self); public bool active(&Self self) { return self.get() > 0; } }; struct Item implements Named { public i32 get(&Item self) { return 7; } }; i32 main() { Item item = Item {}; return item.active() ? 0 : 1; }", "static interface default method materialized");
    accepts("interface Named { public i32 get(&Self self) { return 1; } }; struct Item implements Named { public override i32 get(&Item self) { return 7; } }; i32 main() { Item item = Item {}; return item.get(); }", "explicit interface override");
    accepts("interface Left { public i32 get(&Self self) { return 1; } }; interface Right { public i32 get(&Self self) { return 2; } }; struct Item implements Left, Right { public override i32 get(&Item self) { return 7; } }; i32 main() { Item item = Item {}; return item.get(); }", "conflicting defaults resolved with override");
    accepts("interface Value<T> { public T get(&Self self); }; struct Item implements Value<i32> { public i32 get(&Item self) { return 7; } }; i32 main() { Item item = Item {}; return item.get(); }", "generic interface specialization");
    accepts("i32 identity(i32 value) { return value; } T identity<T>(T value) { return value; } i32 main() { return identity(7); }", "equal fit selects non-generic overload");
    {
        Fixture fixture;
        fixture.write("helper.avx", "public T identity<T>(T value) { return value; } public struct Pair<T> { public T value; }; ");
        const bool passed = fixture.run("import helper { identity, Pair }; i32 main() { Pair<i32> item = Pair<i32> { value = identity(7) }; return item.value; }");
        if (!passed) for (const auto& error : fixture.diagnostics) std::cerr << "  " << error.message << '\n';
        expect(passed, "imported generic declarations preserve module context");
    }
    {
        Fixture fixture;
        fixture.write("helper.avx", "public T identity<T>(T value) { return value; }");
        expect(fixture.run("import helper { identity }; struct Secret { public i32 value; }; i32 main() { Secret item = Secret { value = 7 }; Secret result = identity(item); return result.value; }"), "imported generic accepts caller private concrete type");
    }
    {
        Fixture fixture;
        fixture.write("contract.avx", "const i32 OFFSET = 7; public interface Named { public i32 get(&Self self) { return OFFSET; } };");
        const bool passed = fixture.run("import contract { Named }; struct Item implements Named {}; i32 main() { Item item = Item {}; return item.get(); }");
        if (!passed) for (const auto& error : fixture.diagnostics) std::cerr << "  " << error.message << '\n';
        expect(passed, "imported default preserves private lexical capture");
    }
    accepts("T identity<T>(T value) { return value; } i32 main() { i32 a = identity(1); i32 b = identity<i32>(2); return identity(a + b); }", "repeated generic specialization reuses one concrete declaration");
    accepts("struct Inner<T> { public T value; }; struct Outer<T> { public T inner; }; i32 main() { Inner<i32> inside = Inner<i32> { value = 7 }; Outer<Inner<i32>> outside = Outer<Inner<i32>> { inner = inside }; return outside.inner.value; }", "nested generic types specialize recursively");
    accepts("interface Named { public i32 get(&Self self); }; struct Item implements Named { public i32 get(&Item self) { return 7; } }; i32 read<T>(&T value) where T implements Named { return value.get(); } i32 main() { Item item = Item {}; return read(&item); }", "custom interface where constraint");
    rejects("interface Named { public i32 get(&Self self); }; i32 read<T>(&T value) where T implements Named { return value.get(); } i32 main() { i32 value = 7; return read(&value); }", "does not satisfy", "custom interface constraint rejects missing implementation");
    rejects("interface Named { public i32 get(&Self self) { return (*self).value; } }; struct Item implements Named { public i32 value; };", "only members", "default cannot access undeclared concrete fields");
    rejects("T pair<T>(T a, T b) { return a; } i32 main() { return pair(1, true); }", "infer", "inconsistent generic arguments rejected");
    rejects("T zero<T>() { T value; return value; } i32 main() { rsn value = zero(); return 0; }", "infer", "uninferred type parameters require explicit arguments");
    rejects("T identity<T>(T value) where T implements Copy { return value; } i32 main() { mut string text; string result = identity(text); return 0; }", "does not satisfy", "unsatisfied Copy constraint rejected");
    rejects("interface Named { public i32 get(&Self self); }; struct Item implements Named {};", "Missing required", "missing required interface method rejected");
    rejects("interface Named { public i32 get(&Self self) { return 1; } }; struct Item implements Named { public i32 get(&Item self) { return 7; } };", "requires override", "override annotation required");
    rejects("interface Named { public i32 get(&Self self); }; struct Item implements Named { public bool get(&Item self) { return true; } };", "return type", "interface return mismatch rejected");
    rejects("interface Left { public i32 get(&Self self) { return 1; } }; interface Right { public i32 get(&Self self) { return 2; } }; struct Item implements Left, Right {};", "Conflicting", "ambiguous defaults rejected");
    accepts("struct Item { public T identity<T>(&Item self, T value) { return value; } }; i32 main() { Item item = Item {}; return item.identity(7); }", "generic instance method infers argument");
    accepts("struct Item { public T identity<T>(&Item self, T value) { return value; } }; i32 main() { Item item = Item {}; return item.identity<i32>(7); }", "generic instance method accepts explicit arguments");
    accepts("struct Item { public T identity<T>(T value) { return value; } }; i32 main() { return Item.identity<i32>(7); }", "generic type function");
    accepts("struct Item<T> { public T stored; public U identity<U>(&Self self, U value) { return value; } }; i32 main() { Item<bool> item = Item<bool> { stored = true }; return item.identity(7); }", "generic method on generic struct");
    accepts("enum Item<T> { Empty; public U identity<U>(&Self self, U value) { return value; } }; i32 main() { Item<bool> item = Item<bool>.Empty; return item.identity<i32>(7); }", "generic method on generic enum");
    accepts("interface Identity { public T identity<T>(&Self self, T value); }; struct Item implements Identity { public U identity<U>(&Item self, U value) { return value; } }; i32 main() { Item item = Item {}; return item.identity(7); }", "generic interface alpha equivalent required method");
    accepts("interface Identity { public T identity<T>(&Self self, T value) { return value; } }; struct Item implements Identity {}; i32 main() { Item item = Item {}; return item.identity(7); }", "generic interface default specializes on demand");
    accepts("interface Identity { public T identity<T>(&Self self, T value) { return value; } }; struct Item implements Identity { public override U identity<U>(&Item self, U value) { return value; } }; i32 main() { Item item = Item {}; return item.identity<i32>(7); }", "generic default alpha equivalent override");
    accepts("interface Identity { public T identity<T>(&Self self, T value) where T implements Copy; }; struct Item implements Identity { public U identity<U>(&Item self, U value) where U implements Copy { return value; } }; i32 main() { Item item = Item {}; return item.identity(7); }", "generic interface alpha equivalent requirements");
    accepts("T first<T>(T left, T right) { return left; } i32 main() { i64 wide = 7; i64 result = first(1, wide); return 0; }", "typed later input precedes literal default");
    accepts("struct Marker<T> {}; Marker<T> marker<T>() { return Marker<T> {}; } i32 main() { Marker<i64> value = marker(); return 0; }", "expected destination fills return only type variable");
    accepts("enum Outcome<T, E> { Value { public T item; }; Failure { public E error; }; }; Outcome<T, E> success<T, E>(T value) { return Outcome<T, E>.Value { item = value }; } i32 main() { Outcome<i32, bool> result = success(7); return 0; }", "expected destination fills only unresolved result parameter");
    accepts("i64 choose(i64 value) { return value; } T choose<T>(T value) { return value; } i32 main() { return choose(7); }", "literal default outranks genericity");
    accepts("interface Named { public i32 get(&Self self); }; i32 choose<T>(T value) where T implements Named { return 1; } i32 choose<T>(&T value) { return 2; } i32 main() { i32 value = 7; return choose(&value); }", "unsatisfied constraints remove an overload before ambiguity");
    rejects("struct Item { public i32 get(&Item self) { return 7; } }; i32 read<T>(&T value) { return value.get(); } i32 main() { Item value = Item {}; return read(&value); }", "Generic contract", "incidental concrete method is rejected at declaration");
    rejects("i32 read<T>(&T value) { return value.get(); } i32 main() { return 0; }", "Generic contract", "unused generic template contract is checked");
    rejects("struct Item { public i32 field; }; i32 read<T>(&T value) { return (*value).field; } i32 main() { Item value = Item { field = 7 }; return read(&value); }", "Generic contract", "incidental concrete field is rejected at declaration");
    rejects("T identity<T>(T value) { return value; } i32 main() { i64 value = identity(7); return 0; }", "requires i64", "destination cannot override input inferred type");
    rejects("interface Identity { public T identity<T>(&Self self, T value) { return value; } }; struct Item implements Identity { public U identity<U>(&Item self, U value) { return value; } };", "requires override", "generic default replacement requires override");
    rejects("interface Identity { public T identity<T>(&Self self, T value); }; struct Item implements Identity { public U identity<U>(&Item self, U value) where U implements Copy { return value; } };", "requirements do not match", "generic implementation cannot strengthen contract requirements");
    rejects("T add<T>(T left, T right) { return left + right; } i32 main() { return add(1, 2); }", "does not guarantee operator", "unbounded arithmetic is not a universal operation");
    rejects("T read<T>(&T value) { return *value; } i32 main() { i32 value = 7; return read(&value); }", "requires implements Copy", "unbounded borrowed read requires Copy at declaration");
    rejects("T helper<T>(T value) where T implements Copy { return value; } T relay<T>(T value) { return helper(value); } i32 main() { return relay(7); }", "called function's implements", "generic helper requirements must follow from caller bounds");
    accepts("T helper<T>(T value) where T implements Copy { return value; } T relay<T>(T value) where T implements Copy { return helper(value); } i32 main() { return relay(7); }", "declared bound entails generic helper requirement");
    accepts("interface Reader<T> { public T read(&Self self); }; struct Item implements Reader<i32> { public i32 read(&Item self) { return 7; } }; U read<T, U>(&T value) where T implements Reader<U> { return value.read(); } i32 main() { Item item = Item {}; return read<Item, i32>(&item); }", "dependent generic interface requirements substitute all arguments");
    accepts("struct Marker<T> {}; Marker<T> marker<T>() { return Marker<T> {}; } Marker<i64> build() { return marker(); } i32 main() { Marker<i64> value = build(); return 0; }", "function return context supplies generic holes");
    accepts("struct Marker<T> {}; Marker<T> marker<T>() { return Marker<T> {}; } i32 consume(Marker<i64> value) { return 7; } i32 main() { return consume(marker()); }", "parameter context supplies nested generic holes");
    accepts("interface Read { public i32 read(&Self self); }; interface Make { public T identity<T>(&Self self, T value) where T implements Read { return value; } }; struct Value implements Read { public i32 read(&Value self) { return 7; } }; struct Item implements Make {}; i32 main() { Item item = Item {}; Value value = item.identity(Value {}); return value.read(); }", "generic interface default retains method requirements");
    rejects("T duplicate<T>(T value) { T other = value; return value; } i32 main() { return duplicate(7); }", "after a possible move", "incidental Copy instantiation cannot legalize unbounded double use");
    rejects("T duplicate<T>(T value, bool choose) { if (choose) { T other = value; } return value; } i32 main() { return duplicate(7, true); }", "after a possible move", "generic move in one branch forbids later use");
    rejects("T duplicate<T>(T value) { rsn other = value; return value; } i32 main() { return duplicate(7); }", "after a possible move", "rsn local retains unbounded move semantics");
    accepts("T select<T>(T value, bool choose) { if (choose) { return value; } else { return value; } } i32 main() { return select(7, true); }", "independent terminating branches may move the same input");
    accepts("T duplicate<T>(T value) where T implements Copy { T other = value; return value; } i32 main() { return duplicate(7); }", "declared Copy permits repeated reads");
    accepts("T relay<T>(T value) { T other = value; return other; } i32 main() { return relay(7); }", "unbounded value may move through an owned local");
    rejects("interface Read { public i32 read(&Self self, i32 value); }; struct Item implements Read { public i32 read(&Item self, i32 value) { return value; } public i32 read(&Item self, bool value) { return 7; } }; i32 call<T>(&T value) where T implements Read { return value.read(true); } i32 main() { Item item = Item {}; return call(&item); }", "no matching method signature", "incidental concrete overload is absent from generic bound");
    accepts("interface Read { public i32 read(&Self self, i32 value); }; struct Item implements Read { public i32 read(&Item self, i32 value) { return value; } }; i32 call<T>(&T value) where T implements Read { return value.read(value = 7); } i32 main() { Item item = Item {}; return call(&item); }", "generic bound method matches named argument signature");
    rejects("interface Read { public i32 read(&Self self, i32 value); }; i32 call<T>(&T value) where T implements Read { return value.read(); } i32 main() { return 0; }", "no matching method signature", "unused generic body validates bound method arity");
    rejects("T zero<T>() { return 0; } i32 main() { return zero<i32>(); }", "arbitrary type parameter", "concrete return cannot manufacture an arbitrary generic type");
    rejects("T after_switch<T>(T value, i32 choice) { switch (choice) { case 0: T moved = value; break; default: break; } return value; } i32 main() { return after_switch(7, 0); }", "after a possible move", "case break does not hide later generic moved use");
    accepts("usize count<T>(T values[]) { usize first = Slice.len(values); return Slice.len(values); } i32 main() { return 0; }", "generic pack remains a reusable read-only Slice view");
    accepts("usize count<T>(T values[]) { usize first = Slice.len(values); return Slice.len(values); } i32 main() { return count(7, 8) == 2 ? 0 : 1; }", "instantiated generic pack preserves Slice local typing");
    rejects("T first<T>(T values[]) { return values[0]; } i32 main() { return first(7); }", "requires implements Copy", "generic pack cannot return an unbounded borrowed element by accidental Copy");
    accepts("T first<T>(T values[]) where T implements Copy { return values[0]; } i32 main() { return first(7); }", "Copy bound permits owned reads from generic pack");
    accepts("T identity<T>(T value) { return value; } i32 first<T>(T values[]) where T implements Copy { T value = identity(values[0]); return 0; } i32 main() { return first(7); }", "generic call infers pack element index type");
    accepts("&T first<T>(&T values[]) { return values[0]; } i32 main() { i32 value = 7; &i32 view = first(&value); return *view; }", "generic pack may copy a shared reference without Copy on its referent");
    accepts("interface Value { public i32 get(&Self self); }; struct Number implements Value { public i32 get(&Number self) { return 7; } }; T identity<T>(T value) { return value; } i32 main() { Number value = Number {}; &dyn Value view = &value; return identity(view.get()); }", "generic input inference sees borrowed dyn method result");
    accepts("import memory { Box }; interface Value { public i32 get(&Self self); }; struct Number implements Value { public i32 get(&Number self) { return 7; } }; T identity<T>(T value) { return value; } Result<i32, InputError<Number>> run() { Box<Number> concrete = Box.new(Number {})?; Box<dyn Value> owner = Box.into_dyn(concrete); return Result.ok(identity(owner.get())); } i32 main() { return 0; }", "generic input inference sees owning dyn method result");
    rejects("interface Value { public U identity<U>(&Self self, U value); }; struct Number implements Value { public T identity<T>(&Number self, T value) { return value; } }; i32 main() { Number value = Number {}; &dyn Value view = &value; return view.identity<i32>(7); }", "Unsupported", "generic methods remain unavailable through dyn views");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
