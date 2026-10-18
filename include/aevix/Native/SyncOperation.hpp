// ========================
//
// Created By : miruku_sora2026
//
// File name : SyncOperation.hpp
//
// Description :
//     컴파일러만 생성하는 인라인 잠금 연산의 피연산자·보호자 계약을 정의한다.
//     Define operand and capability contracts for compiler-generated inline lock operations.
//
// ========================
#pragma once
#include "aevix/Frontend/Sema/Type.hpp"
#include <array>
#include <cstddef>
#include <string_view>

namespace aevix
{
    enum class SyncKind { Mutex, RwLock, MutexGuard, ReadGuard, WriteGuard, Poisoned };
    inline constexpr TypeKind syncTypeKind(SyncKind kind)
    {
        switch (kind) {
            case SyncKind::Poisoned: return TypeKind::Nominal;
            case SyncKind::Mutex: return TypeKind::Mutex;
            case SyncKind::RwLock: return TypeKind::RwLock;
            case SyncKind::MutexGuard: return TypeKind::MutexGuard;
            case SyncKind::ReadGuard: return TypeKind::RwLockReadGuard;
            case SyncKind::WriteGuard: return TypeKind::RwLockWriteGuard;
        }
        return TypeKind::Error;
    }
    inline constexpr bool syncOwner(TypeKind kind) { return kind == TypeKind::Mutex || kind == TypeKind::RwLock; }
    inline constexpr bool syncGuard(TypeKind kind) { return kind == TypeKind::MutexGuard || kind == TypeKind::RwLockReadGuard || kind == TypeKind::RwLockWriteGuard; }
    enum class SyncAccess { None, Value, Shared, Exclusive };
    enum class SyncResult { Owner, Acquire, OptionalAcquire, Boolean, Policy, UniqueReference, IntoInner, Reference, MutableReference, Unit, Recovered };
    struct SyncOperationInfo final
    {
        std::string_view name;
        std::size_t operands;
        SyncKind kind;
        SyncAccess access;
        SyncResult result;
        SyncKind guard;
        std::string_view native;
        bool timed;
    };
    inline const SyncOperationInfo* syncOperation(std::string_view name)
    {
        using enum SyncKind;
        using enum SyncAccess;
        using enum SyncResult;
        static constexpr std::array operations = {
            SyncOperationInfo{"poisoned.into_inner",1,Poisoned,Value,Recovered,Poisoned,"",false},
            SyncOperationInfo{"mutex.new",1,Mutex,None,Owner,MutexGuard,"",false},
            SyncOperationInfo{"mutex.with_policy",2,Mutex,None,Owner,MutexGuard,"",false},
            SyncOperationInfo{"mutex.lock",1,Mutex,Shared,Acquire,MutexGuard,"mutex_lock",false},
            SyncOperationInfo{"mutex.try_lock",1,Mutex,Shared,OptionalAcquire,MutexGuard,"mutex_try_lock",false},
            SyncOperationInfo{"mutex.lock_timeout",2,Mutex,Shared,OptionalAcquire,MutexGuard,"mutex_lock_timeout",true},
            SyncOperationInfo{"mutex.is_poisoned",1,Mutex,Shared,Boolean,MutexGuard,"mutex_is_poisoned",false},
            SyncOperationInfo{"mutex.poison_policy",1,Mutex,Shared,Policy,MutexGuard,"",false},
            SyncOperationInfo{"mutex.get_mut",1,Mutex,Exclusive,UniqueReference,MutexGuard,"mutex_is_poisoned",false},
            SyncOperationInfo{"mutex.into_inner",1,Mutex,Value,IntoInner,MutexGuard,"mutex_is_poisoned",false},
            SyncOperationInfo{"mutex.data",1,MutexGuard,Shared,Reference,MutexGuard,"",false},
            SyncOperationInfo{"mutex.data_mut",1,MutexGuard,Exclusive,MutableReference,MutexGuard,"",false},
            SyncOperationInfo{"mutex.unlock",1,MutexGuard,Value,Unit,MutexGuard,"mutex_unlock",false},
            SyncOperationInfo{"mutex.mark_poisoned",1,MutexGuard,Exclusive,Unit,MutexGuard,"",false},
            SyncOperationInfo{"mutex.clear_poison",1,MutexGuard,Exclusive,Unit,MutexGuard,"mutex_clear_poison",false},
            SyncOperationInfo{"rwlock.new",1,RwLock,None,Owner,WriteGuard,"",false},
            SyncOperationInfo{"rwlock.with_policy",2,RwLock,None,Owner,WriteGuard,"",false},
            SyncOperationInfo{"rwlock.read",1,RwLock,Shared,Acquire,ReadGuard,"rwlock_read",false},
            SyncOperationInfo{"rwlock.write",1,RwLock,Shared,Acquire,WriteGuard,"rwlock_write",false},
            SyncOperationInfo{"rwlock.try_read",1,RwLock,Shared,OptionalAcquire,ReadGuard,"rwlock_try_read",false},
            SyncOperationInfo{"rwlock.try_write",1,RwLock,Shared,OptionalAcquire,WriteGuard,"rwlock_try_write",false},
            SyncOperationInfo{"rwlock.read_timeout",2,RwLock,Shared,OptionalAcquire,ReadGuard,"rwlock_read_timeout",true},
            SyncOperationInfo{"rwlock.write_timeout",2,RwLock,Shared,OptionalAcquire,WriteGuard,"rwlock_write_timeout",true},
            SyncOperationInfo{"rwlock.is_poisoned",1,RwLock,Shared,Boolean,WriteGuard,"rwlock_is_poisoned",false},
            SyncOperationInfo{"rwlock.poison_policy",1,RwLock,Shared,Policy,WriteGuard,"",false},
            SyncOperationInfo{"rwlock.get_mut",1,RwLock,Exclusive,UniqueReference,WriteGuard,"rwlock_is_poisoned",false},
            SyncOperationInfo{"rwlock.into_inner",1,RwLock,Value,IntoInner,WriteGuard,"rwlock_is_poisoned",false},
            SyncOperationInfo{"rwlock.read_data",1,ReadGuard,Shared,Reference,ReadGuard,"",false},
            SyncOperationInfo{"rwlock.write_data",1,WriteGuard,Shared,Reference,WriteGuard,"",false},
            SyncOperationInfo{"rwlock.write_data_mut",1,WriteGuard,Exclusive,MutableReference,WriteGuard,"",false},
            SyncOperationInfo{"rwlock.unlock_read",1,ReadGuard,Value,Unit,ReadGuard,"rwlock_read_unlock",false},
            SyncOperationInfo{"rwlock.unlock_write",1,WriteGuard,Value,Unit,WriteGuard,"rwlock_write_unlock",false},
            SyncOperationInfo{"rwlock.mark_poisoned",1,WriteGuard,Exclusive,Unit,WriteGuard,"",false},
            SyncOperationInfo{"rwlock.clear_poison",1,WriteGuard,Exclusive,Unit,WriteGuard,"rwlock_clear_poison",false},
            SyncOperationInfo{"mutex_guard.as_ref",1,MutexGuard,Shared,Reference,MutexGuard,"",false},
            SyncOperationInfo{"mutex_guard.as_mut",1,MutexGuard,Exclusive,MutableReference,MutexGuard,"",false},
            SyncOperationInfo{"read_guard.as_ref",1,ReadGuard,Shared,Reference,ReadGuard,"",false},
            SyncOperationInfo{"write_guard.as_ref",1,WriteGuard,Shared,Reference,WriteGuard,"",false},
            SyncOperationInfo{"write_guard.as_mut",1,WriteGuard,Exclusive,MutableReference,WriteGuard,"",false}
        };
        for (const auto& operation : operations) if (operation.name == name) return &operation;
        return nullptr;
    }
}
