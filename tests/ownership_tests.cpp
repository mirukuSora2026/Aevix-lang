// ========================
//
// Created By : miruku_sora2026
//
// File name : ownership_tests.cpp
//
// Description :
//     실제 소스를 AIR로 변환하여 이동·대여·수명 진단과 경로별 정리 계획을 검증한다.
//     Lower real sources to AIR to verify move, loan, lifetime diagnostics, and cleanup on each path.
//
// ========================

#include "aevix/Frontend/Ownership/BorrowChecker.hpp"
#include "aevix/Frontend/Sema/ControlFlowChecker.hpp"
#include "aevix/Frontend/Sema/GenericResolver.hpp"
#include "aevix/Frontend/Sema/NameResolver.hpp"
#include "aevix/IR/Lowering/ASTLowering.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    unsigned checks = 0;
    unsigned failures = 0;
    void expect(bool condition, std::string_view label)
    {
        ++checks;
        if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
    }

    struct Fixture final
    {
        std::filesystem::path directory;
        aevix::SourceManager sources;
        aevix::Diagnostics diagnostics;
        std::optional<aevix::ModuleGraph> graph;
        std::optional<aevix::NameResolution> names;
        std::optional<aevix::TypeCheckResult> types;
        std::optional<aevix::AIRModule> air;
        aevix::OwnershipResult result;
        bool reachedOwnership = false;

        explicit Fixture(std::string_view source)
        {
            static unsigned serial = 0;
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            directory = std::filesystem::temp_directory_path()
                / ("aevix-ownership-" + std::to_string(stamp) + "-" + std::to_string(++serial));
            std::filesystem::create_directories(directory / "src");
            std::ofstream output(directory / "src/main.avx");
            output << source;
            if (!output) throw std::runtime_error("cannot write ownership fixture");
        }
        ~Fixture()
        {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }
        bool run()
        {
            aevix::ModuleOptions options;
            options.sourceRoot = directory / "src";
            graph = aevix::ModuleLoader(sources, diagnostics).load(directory / "src/main.avx", options);
            if (!graph || !diagnostics.empty()) return false;
            names = aevix::NameResolver(diagnostics).resolve(*graph);
            if (!names->succeeded || !diagnostics.empty()) return false;
            auto concrete = aevix::GenericResolver(diagnostics).resolve(*graph, *names);
            if (!concrete || !diagnostics.empty()) return false;
            names = std::move(concrete);
            types = aevix::TypeChecker(diagnostics).check(*graph, *names);
            if (!types->succeeded || !diagnostics.empty()) return false;
            if (!aevix::ControlFlowChecker(diagnostics).check(*graph, *names, *types)) return false;
            air = aevix::lowerToAIR(*graph, *names, *types, diagnostics);
            if (!air || !aevix::verifyAIR(*air, types->types, diagnostics)) return false;
            reachedOwnership = true;
            result = aevix::BorrowChecker(diagnostics).check(*air, *names, *types);
            return result.succeeded && diagnostics.empty();
        }
        void dump(std::string_view label) const
        {
            std::cerr << label << ": ownership stage=" << reachedOwnership << '\n';
            for (const auto& diagnostic : diagnostics) std::cerr << "  " << diagnostic.message << '\n';
        }
        void checkDiagnostics() const
        {
            for (const auto& diagnostic : diagnostics)
                expect(diagnostic.range.begin.isValid() && sources.contains(diagnostic.range),
                       "ownership diagnostic retains valid source position");
        }
    };

    void valid(std::string_view label, std::string_view source)
    {
        Fixture test(source);
        const auto success = test.run();
        if (!success) test.dump(label);
        expect(success && test.reachedOwnership, label);
    }
    void invalid(std::string_view label, std::string_view source, std::string_view message,
                 bool ownershipStage = true)
    {
        Fixture test(source);
        const auto success = test.run();
        bool found = false;
        for (const auto& diagnostic : test.diagnostics)
            found = found || diagnostic.message.find(message) != std::string::npos;
        if (success || test.reachedOwnership != ownershipStage || !found) test.dump(label);
        expect(!success && test.reachedOwnership == ownershipStage && found, label);
        test.checkDiagnostics();
    }

    constexpr std::string_view owner = "struct Owner { public i32 value; };\n";
    std::string owned(std::string_view body) { return std::string(owner) + std::string(body); }

    void moves()
    {
        valid("scalars copy", "i32 f() { i32 a = 1; i32 b = a; return a + b; }");
        valid("Copy struct preserves source", "struct P implements Copy { public i32 x; }; i32 f() { P a = P { x = 1 }; P b = a; return a.x + b.x; }");
        valid("whole owner can move once", owned("i32 f() { Owner a = Owner { value = 1 }; Owner b = a; return b.value; }"));
        invalid("use after owner move", owned("i32 f() { Owner a = Owner { value = 1 }; Owner b = a; return a.value; }"), "moved");
        invalid("double value argument move", owned("void sink(Owner item) { return; } void f() { Owner a = Owner { value = 1 }; sink(a); sink(a); return; }"), "moved");
        invalid("array moves even with Copy elements", "i32 f() { i32 a[2] = [1, 2]; i32 b[2] = a; return a[0]; }", "moved");
        invalid("one branch moves owner", owned("i32 f(bool yes) { Owner a = Owner { value = 1 }; if (yes) { Owner b = a; } return a.value; }"), "moved");
        invalid("loop cannot repeatedly consume owner", owned("void sink(Owner item) { return; } void f(bool yes) { Owner a = Owner { value = 1 }; while (yes) { sink(a); } return; }"), "moved");
        valid("reinitialization restores mutable moved owner", owned("i32 f() { mut Owner a = Owner { value = 1 }; Owner b = a; a = Owner { value = 2 }; return a.value + b.value; }"));
        invalid("partial owning move is explicit unsupported", "struct Leaf { public i32 x; }; struct Tree { public Leaf leaf; }; i32 f() { Tree a = Tree { leaf = Leaf { x = 1 } }; Leaf b = a.leaf; return b.x; }", "projected");
    }

    void loans()
    {
        valid("multiple shared loans", "i32 f() { i32 x = 1; &i32 a = &x; &i32 b = &x; return *a + *b; }");
        valid("exclusive loan reads and writes its target", "i32 f() { mut i32 x = 1; &mut i32 a = &mut x; *a = 2; return *a; }");
        invalid("shared loan forbids direct write", "i32 f() { mut i32 x = 1; &i32 a = &x; x = 2; return *a; }", "borrowed");
        invalid("exclusive loan forbids direct read", "i32 f() { mut i32 x = 1; &mut i32 a = &mut x; return x + *a; }", "exclusively borrowed");
        invalid("second exclusive loan", "i32 f() { mut i32 x = 1; &mut i32 a = &mut x; &mut i32 b = &mut x; return *a + *b; }", "borrowed");
        invalid("shared loan blocks owner move", owned("i32 f() { Owner x = Owner { value = 1 }; &Owner a = &x; Owner b = x; return (*a).value + b.value; }"), "borrowed");
        valid("scope end releases local loan", "i32 f() { mut i32 x = 1; { &i32 a = &x; i32 z = *a; } x = 2; return x; }");
        valid("last reference use ends shared loan", "i32 f() { mut i32 x = 1; &i32 a = &x; i32 old = *a; x = 2; return old + x; }");
        valid("unused reference does not reserve a place", "i32 f() { mut i32 x = 1; &i32 unused = &x; x = 2; return x; }");
        valid("call-only exclusive loans expire", "void inc(&mut i32 x) { *x += 1; return; } i32 f() { mut i32 x = 1; inc(&mut x); inc(&mut x); return x; }");
        valid("exclusive reference argument is reborrowed", "void inc(&mut i32 x) { *x += 1; return; } i32 f() { mut i32 x = 1; &mut i32 r = &mut x; inc(r); inc(r); return *r; }");
        valid("automatic method loans expire after calls", "struct C { public i32 value; public void inc(&mut C self) { (*self).value++; return; } }; i32 f() { mut C c = C { value = 1 }; c.inc(); c.inc(); return c.value; }");
        invalid("simultaneous call args overlap", "void pair(&mut i32 x, &mut i32 y) { return; } void f() { mut i32 x = 1; pair(&mut x, &mut x); return; }", "borrowed");
        valid("disjoint record fields borrow separately", "struct P { public i32 x; public i32 y; }; i32 f() { mut P p = P { x = 1, y = 2 }; &mut i32 x = &mut p.x; &mut i32 y = &mut p.y; return *x + *y; }");
        invalid("unknown array indices conservatively overlap", "i32 f(usize i, usize j) { mut i32 a[2] = [1, 2]; &mut i32 x = &mut a[i]; &mut i32 y = &mut a[j]; return *x + *y; }", "borrowed");
        valid("break ends its body loan", "i32 f() { mut i32 x = 1; while (true) { &i32 a = &x; i32 y = *a; break; } x = 2; return x; }");
    }

    void lifetimes()
    {
        invalid("return local reference", "&i32 f() { i32 x = 1; return &x; }", "local value");
        valid("return borrowed parameter", "&i32 id(&i32 x) { return x; } i32 f() { i32 x = 1; &i32 y = id(&x); return *y; }");
        invalid("call summary retains returned loan", "&i32 id(&i32 x) { return x; } i32 f() { mut i32 x = 1; &i32 y = id(&x); x = 2; return *y; }", "borrowed");
        invalid("call summary prevents caller local escape", "&i32 id(&i32 x) { return x; } &i32 f() { i32 x = 1; return id(&x); }", "local value");
        invalid("aggregate containing reference cannot escape", "struct View implements Copy { public &i32 target; }; View f() { i32 x = 1; return View { target = &x }; }", "local value");
        valid("aggregate borrowed input may be returned", "struct View implements Copy { public &i32 target; }; View id(View x) { return x; }");
        invalid("nested call summaries propagate lifetime", "&i32 one(&i32 x) { return x; } &i32 two(&i32 x) { return one(x); } &i32 bad() { i32 x = 1; return two(&x); }", "local value");
        invalid("recursive summaries retain input lifetime", "&i32 recurse(&i32 x, bool stop) { if (stop) return x; return recurse(x, true); } &i32 bad() { i32 x = 1; return recurse(&x, true); }", "local value");
        invalid("borrowed field in copied owner retains lifetime", "struct View implements Copy { public &i32 target; }; View id(View x) { return x; } View bad() { i32 x = 1; return id(View { target = &x }); }", "local value");
        invalid("borrow of by-value parameter cannot escape", owned("&Owner bad(Owner item) { return &item; }"), "local value");
        invalid("local reference assigned to outer binding", "i32 f() { &i32 view; { i32 x = 1; view = &x; } return *view; }", "outlive");
        invalid("reference-containing global storage is blocked before ownership", "i32 base = 0; mut &i32 saved = &base; void store(&i32 x) { saved = x; return; }", "AIR lowering: reference-containing global storage", false);
        valid("reference output helper has a checked storage summary", "void store(&mut &i32 out, &i32 value) { *out = value; return; }");
        valid("reference output helper accepts a same-scope referent", "void store(&mut &i32 out, &i32 value) { *out = value; return; } i32 f() { i32 base = 1; i32 next = 2; mut &i32 out = &base; store(&mut out, &next); return *out; }");
        invalid("reference output helper rejects a shorter-lived referent", "void store(&mut &i32 out, &i32 value) { *out = value; return; } i32 f() { i32 base = 1; mut &i32 out = &base; { i32 inner = 2; store(&mut out, &inner); } return *out; }", "outlive");
        invalid("method result borrows its receiver", "struct C { public i32 value; public &i32 get(&C self) { return &(*self).value; } }; i32 f() { mut C c = C { value = 1 }; &i32 r = c.get(); c.value = 2; return *r; }", "borrowed");
    }

    void globalEffects()
    {
        invalid("indirect global write conflicts with shared loan", "mut i32 total = 0; void bump() { total += 1; return; } i32 f() { &i32 r = &total; bump(); return *r; }", "borrowed");
        invalid("transitive global write conflicts with shared loan", "mut i32 total = 0; void bump() { total += 1; return; } void call() { bump(); return; } i32 f() { &i32 r = &total; call(); return *r; }", "borrowed");
        valid("shared global read permits shared loan", "i32 total = 1; i32 read() { return total; } i32 f() { &i32 r = &total; return read() + *r; }");
        invalid("global return summary retains exclusive loan", "mut i32 total = 0; &mut i32 get() { return &mut total; } i32 f() { &mut i32 r = get(); return total + *r; }", "exclusively borrowed");
    }

    void enumOwnership()
    {
        valid("enum owner moves once", "enum E { None; Count { public i32 value; }; }; i32 f() { E a = E.Count { value = 1 }; E b = a; return 0; }");
        invalid("enum owner use after move", "enum E { None; Count { public i32 value; }; }; i32 f() { E a = E.Count { value = 1 }; E b = a; switch (a) { case E.None: break; case E.Count: break; } return 0; }", "moved");
        valid("Copy enum remains readable", "enum E implements Copy { A; B; }; i32 f() { E a = E.A; E b = a; switch (a) { case E.A: break; case E.B: break; } return 0; }");
        valid("consuming enum permits one owned field extraction", owned("enum E { None; Item { public Owner data; }; }; i32 f() { E a = E.Item { data = Owner { value = 1 } }; switch (move a) { case E.Item { Owner item = data }: return item.value; break; case E.None: return 0; break; } }"));
        invalid("enum carrying local reference cannot escape", "enum E { None; View { public &i32 target; }; }; E f() { i32 x = 1; return E.View { target = &x }; }", "local value");
        invalid("enum pattern loan blocks conflicting payload write", "enum E { None; Count { public i32 value; }; }; i32 f() { mut E a = E.Count { value = 1 }; switch (a) { case E.Count { &i32 r = value }: a = E.None; return *r; break; case E.None: return 0; break; } }", "borrowed");
    }

    void genericOwnership()
    {
        valid("generic Copy specialization preserves source", "struct Pair<T> implements Copy where T implements Copy { public T value; }; i32 f() { Pair<i32> a = Pair<i32> { value = 1 }; Pair<i32> b = a; return a.value + b.value; }");
        invalid("generic bound does not make container Copy", "struct Pair<T> where T implements Copy { public T value; }; i32 f() { Pair<i32> a = Pair<i32> { value = 1 }; Pair<i32> b = a; return a.value; }", "moved");
        invalid("generic value parameter consumes owner", owned("T identity<T>(T value) { return value; } i32 f() { Owner a = Owner { value = 1 }; Owner b = identity<Owner>(a); return a.value; }"), "moved");
        invalid("generic returned reference retains caller source", "&T identity<T>(&T value) { return value; } &i32 bad() { i32 x = 1; return identity<i32>(&x); }", "local value");
    }

    // 정리 계획만 실행하는 작은 추적기는 실제 런타임 테스트와 독립적으로 경로별 소유자 수를 센다.
    // This cleanup-plan tracer counts owners on each path independently of native runtime tests.
    bool traceDrops(const aevix::AIRFunction& function, const aevix::TypeContext& types, bool branch)
    {
        std::vector<long long> values(function.valueTypes.size()), locals(function.locals.size());
        std::vector<bool> live(function.locals.size());
        std::map<long long, unsigned> drops;
        long long nextToken = 100;
        for (const auto parameter : function.parameters) { locals[parameter] = branch; live[parameter] = true; }
        std::size_t block = 0;
        for (unsigned steps = 0; steps < 200 && block < function.blocks.size(); ++steps)
        {
            const auto& current = function.blocks[block];
            for (const auto& instruction : current.instructions)
            {
                using Op = aevix::AIROpcode;
                long long result = 0;
                if (instruction.opcode == Op::Aggregate)
                {
                    result = nextToken++;
                    drops[result] = 0;
                }
                else if (instruction.opcode == Op::Constant)
                {
                    if (instruction.operation == "true") result = 1;
                    else if (instruction.operation != "false")
                    {
                        try { result = std::stoll(instruction.operation); }
                        catch (...) { return false; }
                    }
                }
                else if (instruction.opcode == Op::Read || instruction.opcode == Op::Move)
                {
                    if (instruction.place.local >= locals.size() || !live[instruction.place.local]) return false;
                    result = locals[instruction.place.local];
                    if (instruction.opcode == Op::Move) live[instruction.place.local] = false;
                }
                else if (instruction.opcode == Op::Write)
                {
                    if (instruction.place.local >= locals.size() || instruction.operands.size() != 1) return false;
                    locals[instruction.place.local] = values[instruction.operands[0]];
                    live[instruction.place.local] = true;
                }
                else if (instruction.opcode == Op::Drop)
                {
                    const auto local = instruction.place.local;
                    if (local >= locals.size()) return false;
                    if (live[local]) { ++drops[locals[local]]; live[local] = false; }
                }
                else if (instruction.opcode == Op::Binary)
                {
                    const auto a = values[instruction.operands[0]], b = values[instruction.operands[1]];
                    if (instruction.operation == "+") result = a + b;
                    else if (instruction.operation == "<") result = a < b;
                    else if (instruction.operation == "==") result = a == b;
                    else return false;
                }
                else if (instruction.opcode != Op::ScopeBegin && instruction.opcode != Op::ScopeEnd) return false;
                if (instruction.result < values.size()) values[instruction.result] = result;
            }
            const auto& end = current.terminator;
            if (end.kind == aevix::AIRTerminatorKind::Return)
            {
                for (const auto& [token, count] : drops) if (count != 1) return false;
                (void)types;
                return true;
            }
            if (end.kind == aevix::AIRTerminatorKind::Jump) block = end.target;
            else if (end.kind == aevix::AIRTerminatorKind::Branch)
                block = values[end.value] ? end.target : end.alternative;
            else return false;
        }
        return false;
    }

    void cleanup()
    {
        const std::vector<std::pair<std::string_view, std::string>> programs = {
            {"branch move drops exactly once", owned("i32 f(bool yes) { Owner a = Owner { value = 1 }; if (yes) { Owner b = a; } return 0; }")},
            {"normal nested scope drops exactly once", owned("i32 f(bool yes) { { Owner a = Owner { value = 1 }; } return 0; }")},
            {"early return drops exactly once", owned("i32 f(bool yes) { Owner a = Owner { value = 1 }; if (yes) return 1; return 0; }")},
            {"overwrite drops old value exactly once", owned("i32 f(bool yes) { mut Owner a = Owner { value = 1 }; a = Owner { value = 2 }; return 0; }")},
            {"break drops loop body owner", owned("i32 f(bool yes) { while (true) { Owner a = Owner { value = 1 }; break; } return 0; }")},
            {"continue drops each iteration owner", owned("i32 f(bool yes) { for (mut i32 i = 0; i < 3; i++) { Owner a = Owner { value = 1 }; continue; } return 0; }")}
        };
        for (const auto& [label, source] : programs)
        {
            Fixture test(source);
            const auto success = test.run();
            if (!success) test.dump(label);
            expect(success, label);
            if (!success) continue;
            const auto& function = test.air->functions.front();
            expect(traceDrops(function, test.types->types, false), label);
            expect(traceDrops(function, test.types->types, true), label);
            const auto drops = test.result.plannedDrops;
            const auto repeated = aevix::BorrowChecker(test.diagnostics).check(*test.air, *test.names, *test.types);
            expect(repeated.succeeded && repeated.plannedDrops == drops, "cleanup insertion is idempotent");
        }
    }
}

int main()
{
    moves();
    loans();
    lifetimes();
    globalEffects();
    enumOwnership();
    genericOwnership();
    cleanup();
    std::cout << checks << " ownership checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
