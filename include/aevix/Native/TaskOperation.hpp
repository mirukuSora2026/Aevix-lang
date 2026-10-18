// ========================
//
// Created By : miruku_sora2026
//
// File name : TaskOperation.hpp
//
// Description :
//     작업·스코프와 독립 취소 소유자에 허용된 닫힌 연산과 정확한 역할을 정의한다.
//     Define closed Task/Scope and independent-cancellation operations with exact owner roles.
//
// ========================
#pragma once
#include "aevix/Frontend/Sema/Type.hpp"
#include <array>
#include <cstddef>
#include <string_view>

namespace aevix
{
    enum class CancellationResult { Source, Token, Request, Boolean };
    struct CancellationOperationInfo final
    {
        std::string_view name;
        std::size_t operands;
        TypeKind input;
        CancellationResult result;
        bool fallible;
        std::string_view native;
    };
    inline constexpr bool cancellationOwner(TypeKind kind)
    { return kind == TypeKind::CancellationSource || kind == TypeKind::CancellationToken; }
    inline const CancellationOperationInfo* cancellationOperation(std::string_view name)
    {
        using enum CancellationResult;
        static constexpr std::array operations = {
            CancellationOperationInfo{"source_new",0,TypeKind::Error,Source,true,"aevix_cancel_source_new"},
            CancellationOperationInfo{"source_clone",1,TypeKind::CancellationSource,Source,true,"aevix_cancel_source_clone"},
            CancellationOperationInfo{"token_from_source",1,TypeKind::CancellationSource,Token,true,"aevix_cancel_token_from_source"},
            CancellationOperationInfo{"token_clone",1,TypeKind::CancellationToken,Token,true,"aevix_cancel_token_clone"},
            CancellationOperationInfo{"source_request",1,TypeKind::CancellationSource,Request,true,"aevix_cancel_source_request"},
            CancellationOperationInfo{"token_requested",1,TypeKind::CancellationToken,Boolean,false,"aevix_cancel_token_requested"}
        };
        for (const auto& operation : operations) if (operation.name == name) return &operation;
        return nullptr;
    }
    inline constexpr bool taskOwner(TypeKind kind)
    { return kind == TypeKind::Task || kind == TypeKind::TaskScope; }
    enum class TaskAccess { None, ScopeShared, ScopeValue, TaskValue, TaskShared, TaskExclusive, CancellationShared };
    enum class TaskResult { Scope, Spawn, Join, TryTake, Discard, Status, Wait, Request, Close, Boolean, Completion, CheckCancel, Executor, CloseAdmission, ActiveCount, ScopeState, Token, Identity };
    inline constexpr bool taskFallible(TaskResult result)
    { return result != TaskResult::Status && result != TaskResult::Boolean && result != TaskResult::Executor && result != TaskResult::ActiveCount && result != TaskResult::ScopeState && result != TaskResult::Identity; }
    enum class TaskPreparation { None, Ready };
    struct TaskOperationInfo final
    {
        std::string_view name;
        std::size_t operands;
        TaskAccess access;
        TaskResult result;
        bool worker;
        TaskPreparation preparation = TaskPreparation::None;
    };
    inline const TaskOperationInfo* taskOperation(std::string_view name)
    {
        using enum TaskAccess;
        using enum TaskResult;
        static constexpr std::array operations = {
            TaskOperationInfo{"scope_open",2,None,Scope,false},
            TaskOperationInfo{"scope_close",1,ScopeValue,Close,false},
            TaskOperationInfo{"spawn_owned",2,ScopeShared,Spawn,true},
            TaskOperationInfo{"spawn",2,ScopeShared,Spawn,true},
            TaskOperationInfo{"ready",2,ScopeShared,Spawn,false,TaskPreparation::Ready},
            TaskOperationInfo{"join",1,TaskValue,Join,false},
            TaskOperationInfo{"try_take",1,TaskExclusive,TryTake,false},
            TaskOperationInfo{"discard_result",1,TaskValue,Discard,false},
            TaskOperationInfo{"status",1,TaskShared,Status,false},
            TaskOperationInfo{"wait_timeout",2,TaskShared,Wait,false},
            TaskOperationInfo{"request_cancel",1,TaskShared,Request,false},
            TaskOperationInfo{"is_finished",1,TaskShared,Boolean,false},
            TaskOperationInfo{"is_cancel_requested",1,TaskShared,Boolean,false},
            TaskOperationInfo{"cancellation",1,TaskShared,Token,false},
            TaskOperationInfo{"wait",1,TaskShared,Completion,false},
            TaskOperationInfo{"cancel_and_join",1,TaskValue,Join,false},
            TaskOperationInfo{"check_cancel",1,CancellationShared,CheckCancel,false},
            TaskOperationInfo{"executor",1,TaskShared,Executor,false},
            TaskOperationInfo{"id",1,TaskShared,Identity,false},
            TaskOperationInfo{"close_admission",1,ScopeShared,CloseAdmission,false},
            TaskOperationInfo{"active_count",1,ScopeShared,ActiveCount,false},
            TaskOperationInfo{"scope_status",1,ScopeShared,ScopeState,false}
        };
        for (const auto& operation : operations) if (operation.name == name) return &operation;
        return nullptr;
    }

}
