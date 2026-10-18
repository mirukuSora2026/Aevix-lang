// ========================
//
// Created By : miruku_sora2026
//
// File name : README.txt
//
// Description :
//     공개 라이브러리 목록·버전 주석의 의미와 재생성 절차를 설명한다.
//     Explain public library inventories, lifecycle metadata, and regeneration.
//
// ========================

Library API inventory and lifecycle
===================================

The design catalog and implementation inventory are separate views.
api_inventory.json extracts callable API table rows from the English data_type
pages, plus the system console and CPU parallel reference tables, and records
every occurrence. Exact copied signatures are merged, while
different signatures remain separate. The Array family contains historical
Vector/Slice pages; family totals therefore describe documentation locations,
not mutually exclusive runtime modules. Korean generated pages are not scanned.
GPU-only Pending, queues, asynchronous transfers, and device compilation are
recorded under excluded_gpu. CPU Tensor and Quantized contracts remain in scope.
Examples, operators, and prose-only future suggestions are not counted as APIs.

source records public library modules, types, methods, free functions, fields,
aliases, values, and public enum variants. Private parents hide their members.
Function bodies and literal strings are skipped. This scanner inventories
declarations; only the compiler can validate their semantics. An exact source
equivalent is not proof that the catalog's static-method calling form exists.
The existing v03 manifest links supported free-function equivalents to fixtures.
Its presence is not proof that the fixture ran successfully in a current build.
Compiler-injected standard modules are extracted separately from known payloads
in ModuleLoader.cpp, CoreErrorTypes.cpp, and the known AtomicModuleSource.hpp /
SyncModuleSource.hpp / TaskModuleSource.hpp headers when present. The task provider
now supplies independent cancellation and an owned-only Task/Scope execution
profile, with sealed transfer/effect checks and one native thread per child.
Its declarations do not imply complete scheduling, hierarchy, borrowed scopes,
pools, detach or async support. Suspending APIs are assigned to0.4; remaining
synchronous CPU work stays in0.3. Their exact C++ provider, virtual
source name, and physical source location are recorded. Intrinsic declaration
bodies and signatures are compiler binding surfaces, not ordinary Aevix
implementations or callable type contracts. Compiler-injected callable metadata
uses provider_declaration instead of signature, and sets signature_role to
compiler_binding_placeholder_not_call_contract. A placeholder such as void new
must never be read as the operation's actual checked return type. The rendered
page labels these records and deliberately omits their placeholder signatures.
SIMD operations without explicit Aevix method declarations are extracted from
the checker's explicit namespace dispatch and receive @api blocks in that C++
provider. The implemented scalar/lane restrictions still apply to each operation.
The new Array.from_copy(&values) intrinsic is recorded separately with its exact
borrowed fixed-array signature and Copy-element requirement. It is not the older
Array.from_copy(T, N, &fill) catalog proposal, and arrays remain move-only.

Lifecycle comments use ordinary /* ... */ blocks directly before declarations:

    /*
     * Describe the API's behavior and costs here.
     * @since 0.3
     * @deprecated_at none
     * @removed_at none
     */

@since is the first API version that supplies the declaration.
@deprecated_at is the first documented deprecation version, or none.
@removed_at is the documented removal version, or none.
none means no end version is scheduled; it is not a permanent compatibility
promise. Do not invent dates for active APIs. A real deprecation should also
carry @deprecated with the reason and replacement guidance.
@version retains its existing meaning: documentation/API metadata version.
It is not the introduction version, compiler selection, or removal schedule.
@module names a generated file-level inventory record; it is tooling metadata,
not an Aevix module declaration or a language keyword.

The lifecycle_policy.json known_since entries preserve exact declarations from
released 0.1 and 0.2 source snapshots, including compiler-generated declarations.
Box and the established Result/Optional carriers therefore retain 0.1 history;
the later numeric/text errors and SIMD/parallel/Complex additions retain 0.2.
The recorded baselines include local refs and exact commits. New declarations
default to the --since version. Existing valid lifecycle
tags always win. A changed signature has a new identity and requires review.

After source editing has stopped, run these commands from the repository root:

    python3 tools/docs/library_api apply --since 0.3 --compiler-generated
    python3 tools/docs/library_api inventory
    python3 tools/docs/library_api check
    python3 tools/docs/library_api render --output htmls/releases/api/Library_Status.html
    python3 tests/docs/library_api_tests

apply updates library/*.avx and preserves prose plus @version, @parameter,
@return and existing lifecycle dates. It does not change signatures or bodies.
Use repeated --source arguments to update selected files. Do not run it while
another task is editing the same sources. inventory/check never modify sources.
All Aevix documentation comments are English only. apply removes a redundant
Korean comment line only when a meaningful English counterpart already exists;
Korean-only or mixed-line prose is flagged for manual translation. String/raw
literals and executable tokens are never translated. check rejects remaining
Korean prose in .avx comments and injected Aevix payload comments. Ordinary C++
comments outside those payloads retain the bilingual repository convention.
The explicit --compiler-generated option also updates only the known injected
Aevix source payloads in ModuleLoader.cpp/CoreErrorTypes.cpp, the known module
source headers listed above, and the SIMD @api
and Array copy @api comment regions in TypeChecker.cpp. It preserves executable source tokens and
adds no C++ behavior. Do not use this option during compiler edits.
check rejects missing/new/removed public declarations, signature drift, missing
tags, stale lifecycle metadata, invalid version order, and catalog drift.
The generated table is suitable for the static documentation site; locale
generation and navigation integration are separate site-maintenance steps.

catalog_source_comparison.json retains per-row design signatures, conservative
source name candidates, explicit manifest-equivalent links and known contract
differences. A name match is not an exact implementation verdict, and unmatched
rows can be compiler intrinsics. It also recommends bounded pure-source batches.

task_implementation_plan.json preserves all 59 conceptual Task signatures with
structured scope, cancellation, result ownership, executor and suspension
prerequisites. It is an implementation order and rejection boundary, not proof
that the native thread foundation implements every Task catalog operation.

Compiler-owned APIs also need separate semantic tests and explicit status.
The provider list points to their source boundaries; this tool does not infer
supported overloads from arbitrary C++ text or manufacture implementation stubs.

error_reference.json records concrete qualified source error/carrier variants,
payloads, lifecycle values and exact inspection-code mappings used by the English
and Korean error reference. It remains separate from conceptual API catalog rows.

verified_checkpoint.json records the frozen138-test local checkpoint on the
39088693 binary: one uninterrupted macOS ARM64 invocation,703.69seconds, with
all526 recorded compiler/runtime/library/test/tool/Unicode-data and English
catalog inputs unchanged. The immutable log, source manifest and3797-declaration
inventory identify that snapshot. The129-test interrupted/resumed snapshot and
earlier106/106 and120/120 snapshots are archived under checkpoints/. Later
implementation and generated-document refreshes are separate changes.
Library_Status renders the checkpoint separately from newer declaration presence.
Neither a passing suite nor declaration counts complete the CPU catalog.

unicode_properties.json records the pinned Unicode17 dataset/generator and exact
reproducibility boundary. char_source_checkpoint.json maps107 catalog operations
to2 direct Char bindings and105 explicit unicode alternatives, including29
adapted error carriers. It does not claim whole-string segmentation or locale
processing. integer_source_checkpoint.json and integer_contract_audit.json keep
explicit integer helpers separate from the canonical Integer namespace.

tools/docs/error_api inventory/render/check derives the concrete CPU error table
from source and a reviewed policy. The registered drift checks reject stale API
inventories and error references. Types lacking direct Error inspection retain
that limitation instead of receiving invented code() or message() methods.
