// ========================
//
// Created By : miruku_sora2026
//
// File name : PersistRuntime.hpp
//
// Description :
//     AOT와 JIT가 공유하는 persist 초기화·종료용 C 호출 규약을 정의한다.
//     Define the shared C ABI for persist initialization and shutdown in AOT and JIT.
//
// ========================
#pragma once

extern "C"
{
    using AevixPersistCallback = void (*)();

    // 슬롯은 0으로 초기화한 포인터 저장소이며 같은 모듈의 모든 호출이 공유한다.
    // Slots are zero-initialized pointer storage shared by all calls in the same module.
    void aevix_persist_ensure(void** domainSlot, void** guardSlot, const char* name,
                              AevixPersistCallback initialize, AevixPersistCallback cleanup);

    // 진행 중 초기화를 기다린 다음 완료 역순으로 정리하며 재호출은 아무 일도 하지 않는다.
    // Wait for active initialization, then clean up in reverse completion order; repeated calls do nothing.
    void aevix_persist_shutdown(void** domainSlot);
}
