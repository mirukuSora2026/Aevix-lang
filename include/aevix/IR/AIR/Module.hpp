// ========================
//
// Created By : miruku_sora2026
//
// File name : Module.hpp
//
// Description :
//     타입이 결정된 값, 장소, 명령과 제어 흐름 블록으로 구성된 AIR를 정의한다.
//     Define typed AIR values, places, instructions, and control-flow blocks.
//
// ========================

#pragma once

#include "aevix/Frontend/Sema/TypeChecker.hpp"
#include <limits>
#include <string>
#include <vector>

namespace aevix
{
    using AIRValueId = std::size_t;
    using AIRLocalId = std::size_t;
    using AIRBlockId = std::size_t;
    using AIRScopeId = std::size_t;
    inline constexpr std::size_t InvalidAIRId = std::numeric_limits<std::size_t>::max();

    enum class AIRProjectionKind { Field, Index, Dereference, EnumField };
    struct AIRProjection final
    {
        AIRProjectionKind kind = AIRProjectionKind::Field;
        std::size_t field = 0;
        AIRValueId index = InvalidAIRId;
        TypeId type = InvalidTypeId;
        std::size_t extent = 0;
        SymbolId variant = InvalidSymbolId;
    };
    struct AIRPlace final
    {
        AIRLocalId local = InvalidAIRId;
        SymbolId global = InvalidSymbolId;
        AIRValueId pointer = InvalidAIRId;
        TypeId type = InvalidTypeId;
        std::vector<AIRProjection> projections;
    };
    enum class AIROpcode
    {
        Constant, Read, Move, Write, BorrowShared, BorrowMut, ClosureConsume,
        Unary, Binary, Call, Aggregate, EnumConstruct, EnumTag, EnsurePersist, ScopeBegin, ScopeEnd, Drop,
        RangeConstruct, RangeTest, RangeNext, RangeValue, RangeWithStep, RangeCount, RangeCountFits, SliceConstruct, SliceLength,
        VectorAllocate, VectorAvailable, VectorAppend, VectorLength, VectorReserve, NumericStatus, NumericConvert, Simd,
        ArrayRepeat, TextIntrinsic, ParallelIntrinsic, SliceSubview, ComplexIntrinsic, ParseIntrinsic, SystemIntrinsic, MemoryIntrinsic, OsIntrinsic, VectorTake, VectorTruncate, VectorInsert, SharedIntrinsic, AtomicIntrinsic, SyncIntrinsic, CancellationIntrinsic, TaskIntrinsic, RangeInspect, CharScalar, BoxExtract
    };
    struct AIRInstruction final
    {
        AIROpcode opcode = AIROpcode::Constant;
        AIRValueId result = InvalidAIRId;
        TypeId type = InvalidTypeId;
        std::vector<AIRValueId> operands;
        AIRPlace place;
        SymbolId callee = InvalidSymbolId;
        std::string operation;
        std::vector<SymbolId> dynamicTargets;
        TypeId dynamicSignature = InvalidTypeId;
        // 닫힌 원자 연산의 스칼라 타입이며 소스 주소나 임의 ABI를 나타내지 않는다.
        // Scalar type of a closed atomic operation; this carries no source pointer or arbitrary ABI.
        TypeId atomicElement = InvalidTypeId;
        // 잠금의 실제 T이며 원시 상태나 위조 가능한 소스 토큰이 아니다.
        // The actual lock payload T, never raw state or a forgeable source token.
        TypeId syncElement = InvalidTypeId;
        // 작업 결과·캡처 타입과 검증된 구체 __invoke 대상이며 원시 콜백 주소가 아니다.
        // Concrete task result/capture types and verified __invoke identity, never a source callback address.
        TypeId taskElement = InvalidTypeId;
        TypeId taskCallable = InvalidTypeId;
        SymbolId taskInvoke = InvalidSymbolId;
        // FnOnce 환경 전체를 한 번에 받는 지역 목록이며 일반 필드 이동 권한이 아니다.
        // Locals receiving one complete FnOnce environment, never permission for general field moves.
        std::vector<AIRLocalId> closureLocals;
        // 추출할 Box 소유자와 실제 결과 T이며 임의 포인터나 필드 이동 권한이 아니다.
        // Exact extracting Box owner and resulting T, never a raw pointer or permission for field moves.
        TypeId boxOwner = InvalidTypeId;
        TypeId boxElement = InvalidTypeId;
        AIRScopeId scope = 0;
        const Node* source = nullptr;
        SourceRange range{};
    };
    enum class AIRTerminatorKind { Unreachable, Jump, Branch, Return, Trap, NoReturn };
    struct AIRTerminator final
    {
        AIRTerminatorKind kind = AIRTerminatorKind::Unreachable;
        AIRValueId value = InvalidAIRId;
        AIRBlockId target = InvalidAIRId;
        AIRBlockId alternative = InvalidAIRId;
        std::vector<AIRScopeId> exitedScopes;
        const Node* source = nullptr;
        SourceRange range{};
    };
    struct AIRBlock final
    {
        std::string name;
        std::vector<AIRInstruction> instructions;
        AIRTerminator terminator;
    };
    struct AIRLocal final
    {
        std::string name;
        SymbolId symbol = InvalidSymbolId;
        TypeId type = InvalidTypeId;
        bool isMutable = false;
        bool isParameter = false;
        AIRScopeId scope = 0;
        const Node* source = nullptr;
    };
    struct AIRScope final
    {
        AIRScopeId parent = InvalidAIRId;
        std::vector<AIRLocalId> locals;
    };
    struct AIRFunction final
    {
        SymbolId symbol = InvalidSymbolId;
        std::string name;
        TypeId result = InvalidTypeId;
        std::vector<AIRLocalId> parameters;
        std::vector<AIRLocal> locals;
        std::vector<AIRScope> scopes;
        std::vector<AIRBlock> blocks;
        std::vector<TypeId> valueTypes;
        const Node* source = nullptr;
        SymbolId initializesGlobal = InvalidSymbolId;
    };
    struct AIRExternal final
    {
        SymbolId symbol = InvalidSymbolId;
        std::string name;
        TypeId result = InvalidTypeId;
        std::vector<TypeId> parameters;
        bool variadic = false;
        std::string callingConvention = "C";
    };
    struct AIRGlobal final
    {
        SymbolId symbol = InvalidSymbolId;
        std::string name;
        TypeId type = InvalidTypeId;
        bool isMutable = false;
        const Node* initializer = nullptr;
        const Node* source = nullptr;
        bool isPersist = false;
        bool isLocalPersist = false;
        bool isConstant = false;
        bool isStaticText = false;
        SymbolId initializerFunction = InvalidSymbolId;
        std::string initializationOrderKey;
        std::vector<SymbolId> dependencies;
    };
    struct AIRRecord final
    {
        TypeId type = InvalidTypeId;
        std::vector<SymbolId> fields;
        std::vector<TypeId> fieldTypes;
        bool isEnum = false;
        std::vector<EnumVariantInfo> variants;
        const Node* source = nullptr;
    };
    struct AIRModule final
    {
        std::vector<AIRFunction> functions;
        std::vector<AIRGlobal> globals;
        std::vector<AIRRecord> records;
        SymbolId entry = InvalidSymbolId;
        std::vector<AIRExternal> externals;
        std::vector<SymbolId> initializationOrder;
    };
    std::string dumpAIR(const AIRModule& module, const TypeContext& types);
    bool validBoxExtractionShape(const AIRFunction& function, const AIRInstruction& instruction, const TypeContext& types);
    bool verifyAIR(const AIRModule& module, const TypeContext& types, Diagnostics& diagnostics);
}
