// ========================
//
// Created By : miruku_sora2026
//
// File name : ModuleLoader.cpp
//
// Description :
//     검색 루트에서 연결된 .avx 모듈을 읽어 의존 그래프를 만들고 경로와 패키지 순환을 검사한다.
//     Load reachable .avx modules from search roots, build their dependency graph, and check paths and package cycles.
//
// ========================

#include "aevix/Frontend/Modules/ModuleLoader.hpp"

#include "aevix/Frontend/Lexer/Lexer.hpp"
#include "aevix/Frontend/Parser/Parser.hpp"
#include "aevix/Support/Process.hpp"
#include "aevix/Native/OsOperation.hpp"
#include "aevix/Native/TaskOperation.hpp"
#include "aevix/Frontend/Modules/AtomicModuleSource.hpp"
#include "aevix/Frontend/Modules/SyncModuleSource.hpp"
#include "aevix/Frontend/Modules/TaskModuleSource.hpp"

#include <algorithm>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <set>
#include <system_error>
#include <utility>

#ifndef AEVIX_INSTALL_LANGUAGE_DIR
#define AEVIX_INSTALL_LANGUAGE_DIR "share/aevix/library"
#endif

namespace aevix
{
    namespace
    {
        namespace fs = std::filesystem;

        bool identifier(std::string_view text)
        {
            const auto start = [](unsigned char character)
            {
                return (character >= 'a' && character <= 'z')
                    || (character >= 'A' && character <= 'Z')
                    || character == '_';
            };

            if (text.empty() || !start(static_cast<unsigned char>(text.front())))
                return false;

            return std::all_of(text.begin() + 1, text.end(), [&](unsigned char character)
            {
                return start(character) || (character >= '0' && character <= '9');
            });
        }

        bool modulePath(std::string_view name)
        {
            if (name.empty())
                return false;

            while (true)
            {
                const auto dot = name.find('.');
                if (!identifier(name.substr(0, dot)))
                    return false;
                if (dot == std::string_view::npos)
                    return true;
                name.remove_prefix(dot + 1);
            }
        }

        std::string folded(std::string value)
        {
            for (auto& character : value)
                if (character >= 'A' && character <= 'Z')
                    character = static_cast<char>(character - 'A' + 'a');
            return value;
        }

        std::optional<std::string> relativeName(fs::path relative)
        {
            if (relative.empty() || relative.is_absolute() || relative.extension() != ".avx")
                return std::nullopt;

            relative.replace_extension();
            std::string result;
            for (const auto& component : relative)
            {
                const auto part = utf8Path(component);
                if (!identifier(part))
                    return std::nullopt;
                if (!result.empty())
                    result += '.';
                result += part;
            }
            return result.empty() ? std::nullopt : std::optional<std::string>(result);
        }

        struct Root final
        {
            fs::path directory;
            std::string prefix;
        };

        struct Candidate final
        {
            std::string name;
            fs::path path;
            fs::path canonical;
            std::string package;
            std::size_t root;
        };

        struct Dependency final
        {
            ModuleId from;
            ModuleId to;
            SourceRange range;
        };

        class Loader final
        {
        public:
            Loader(SourceManager& sources, Diagnostics& diagnostics)
                : sources_(sources), diagnostics_(diagnostics)
            {
            }

            std::optional<ModuleGraph> run(const fs::path& entry, const ModuleOptions& options)
            {
                std::error_code error;
                const auto absoluteEntry = fs::absolute(entry, error).lexically_normal();
                if (error)
                {
                    diagnose({}, "Cannot resolve entry path: " + error.message());
                    return std::nullopt;
                }

                // 부모 경로만 정규화해 파일 별칭 자체의 모듈 이름을 보존한다.
                // Canonicalize only the parent to preserve a file alias's module name.
                const auto entryParent = fs::weakly_canonical(absoluteEntry.parent_path(), error);
                if (error)
                {
                    diagnose({}, "Cannot resolve entry directory: " + error.message());
                    return std::nullopt;
                }
                const auto logicalEntry = entryParent / absoluteEntry.filename();

                std::string text;
                if (options.entrySource) text = *options.entrySource;
                else if (!read(logicalEntry, {}, text))
                    return std::nullopt;
                const auto entrySource = sources_.add(utf8Path(logicalEntry), std::move(text));
                anchor_ = {{entrySource, 0}, {entrySource, 0}};

                auto sourceRoot = options.sourceRoot;
                if (sourceRoot.empty())
                {
                    sourceRoot = entryParent;
                    // 기본 src/ 규칙을 우선하고 독립 파일은 자기 폴더를 사용한다.
                    // Prefer the default src/ ancestor, falling back to a standalone file's directory.
                    for (auto ancestor = entryParent; !ancestor.empty();)
                    {
                        if (ancestor.filename() == "src")
                        {
                            sourceRoot = ancestor;
                            break;
                        }
                        const auto parent = ancestor.parent_path();
                        if (parent == ancestor)
                            break;
                        ancestor = parent;
                    }
                }
                if (!addRoot(sourceRoot, ""))
                    return std::nullopt;
                for (const auto& root : options.searchRoots)
                    addRoot(root.directory, root.prefix);

                inventory();

                const Candidate* selected = nullptr;
                for (const auto& candidate : candidates_)
                    if (candidate.root == 0 && candidate.path == logicalEntry)
                    {
                        selected = &candidate;
                        break;
                    }
                if (!selected)
                {
                    diagnose(anchor_, "Entry must be an exactly spelled .avx module inside the source root.");
                    return std::nullopt;
                }

                // 진입 파일도 다른 검색 루트와 같은 모듈 경로 충돌 검사를 받는다.
                // The entry also participates in module-path collision checks across roots.
                const auto* resolved = resolve(selected->name, anchor_);
                if (!resolved)
                    return std::nullopt;
                graph_.entry = registerModule(*resolved, anchor_, entrySource);
                if (graph_.entry == InvalidModuleId)
                    return std::nullopt;

                // 모듈을 먼저 등록한 뒤 확장하므로 선언 import 순환은 종료된다.
                // Register modules before expansion so declaration import cycles terminate.
                for (ModuleId index = 0; index < graph_.modules.size(); ++index)
                {
                    auto& source = *graph_.modules[index];
                    if (!source.ast)
                        continue;
                    for (const auto& child : source.ast->children)
                    {
                        if (child->kind != "Import" || child->value == "$core")
                            continue;
                        ModuleId target = InvalidModuleId;
                        if (child->value == "memory")
                        {
                            if (byName_.contains("memory"))
                            { diagnose(child->range, "Ambiguous module path 'memory': project module and builtin standard module."); continue; }
                            target = registerMemoryModule();
                        }
                        else if (child->value == "sync")
                        {
                            if (byName_.contains("sync"))
                            { diagnose(child->range, "Ambiguous module path 'sync': project module and builtin standard module."); continue; }
                            target = registerSyncModule();
                        }
                        else if (child->value == "task")
                        {
                            if (byName_.contains("task"))
                            { diagnose(child->range, "Ambiguous module path 'task': project module and builtin standard module."); continue; }
                            target = registerTaskModule();
                        }
                        else if (child->value == "atomic")
                        {
                            if (byName_.contains("atomic"))
                            { diagnose(child->range, "Ambiguous module path 'atomic': project module and builtin standard module."); continue; }
                            target = registerAtomicModule();
                        }
                        else if (child->value == "parallel" || child->value == "simd")
                        {
                            if (byName_.contains(child->value))
                            { diagnose(child->range, "Ambiguous module path '" + child->value + "': project module and builtin standard module."); continue; }
                            target = registerCpuModule(child->value);
                        }
                        else if (child->value == "system")
                        {
                            if (byName_.contains(child->value))
                            { diagnose(child->range, "Ambiguous module path 'system': project module and builtin standard module."); continue; }
                            target = registerSystemModule();
                        }
                        else if (libraryDirectory(child->value))
                        {
                            if (byName_.contains(child->value))
                            { diagnose(child->range, "Ambiguous module path '" + child->value + "': project module and builtin standard module."); continue; }
                            target = registerLibraryModule(child->value, child->range);
                        }
                        else
                        {
                            const auto* candidate = resolve(child->value, child->range);
                            if (!candidate) continue;
                            target = registerModule(*candidate, child->range);
                        }
                        if (target == InvalidModuleId)
                            continue;
                        if (std::find(source.dependencies.begin(), source.dependencies.end(), target)
                            == source.dependencies.end())
                            source.dependencies.push_back(target);
                        edges_.push_back({index, target, child->range});
                    }
                }

                checkPackageCycles();
                return std::move(graph_);
            }

        private:
            SourceManager& sources_;
            Diagnostics& diagnostics_;
            SourceRange anchor_{};
            std::vector<Root> roots_;
            std::vector<Candidate> candidates_;
            std::map<std::string, std::vector<std::size_t>> byName_;
            std::map<std::string, ModuleId> loaded_;
            std::map<fs::path, ModuleId> byFile_;
            std::vector<Dependency> edges_;
            ModuleGraph graph_;

            static const char* libraryDirectory(const std::string& name)
            {
                if (name == "numeric" || name == "complex" || name == "math" || name == "floating" || name == "integer") return "Math";
                if (name == "shape" || name == "tensor" || name == "quantized") return "AI";
                if (name == "error" || name == "optional" || name == "result" || name == "text" || name == "ascii" || name == "encoding"
                    || name == "bytes" || name == "ordering" || name == "unicode" || name == "boolean") return "Core";
                if (name == "algorithm" || name == "vector" || name == "array" || name == "slice"
                    || name == "hash" || name == "map" || name == "set"
                    || name == "ordered_map" || name == "ordered_set"
                    || name == "map_entry" || name == "range") return "Collections";
                if (name == "io" || name == "path" || name == "fs") return "IO";
                if (name == "env" || name == "time" || name == "process") return "System";
                return nullptr;
            }

            void diagnose(SourceRange range, std::string message)
            {
                diagnostics_.push_back({range, std::move(message)});
            }

            bool read(const fs::path& path, SourceRange range, std::string& text)
            {
                std::error_code error;
                if (!fs::is_regular_file(path, error))
                {
                    diagnose(range, "Cannot open module file: " + utf8Path(path)
                        + (error ? " (" + error.message() + ")" : " (not a regular file)"));
                    return false;
                }
                std::ifstream input(path, std::ios::binary);
                if (!input)
                {
                    diagnose(range, "Cannot open module file: " + utf8Path(path));
                    return false;
                }
                try
                {
                    text.assign(std::istreambuf_iterator<char>(input), {});
                }
                catch (const std::ios_base::failure& failure)
                {
                    // 스트림 버퍼가 직접 던지는 읽기 오류도 소스 진단으로 바꾼다.
                    // Convert read errors thrown directly by the stream buffer into source diagnostics.
                    diagnose(range, "Cannot read module file: " + utf8Path(path)
                        + " (" + failure.what() + ")");
                    return false;
                }
                if (input.bad())
                {
                    diagnose(range, "Cannot read module file: " + utf8Path(path));
                    return false;
                }
                return true;
            }

            bool addRoot(const fs::path& directory, const std::string& prefix)
            {
                if (!prefix.empty() && !modulePath(prefix))
                {
                    diagnose(anchor_, "Invalid external package prefix: " + prefix);
                    return false;
                }
                std::error_code error;
                const auto canonical = fs::canonical(directory, error);
                if (error || !fs::is_directory(canonical, error))
                {
                    diagnose(anchor_, "Cannot access module source root: " + utf8Path(directory)
                        + (error ? " (" + error.message() + ")" : ""));
                    return false;
                }
                roots_.push_back({canonical, prefix});
                return true;
            }

            void inventory()
            {
                // 같은 패키지의 여러 검색 루트에도 대소문자 충돌 규칙을 적용한다.
                // Apply case-collision rules across every search root belonging to the same package.
                std::map<std::string, std::map<std::string, std::string>> packageCaseNames;
                for (std::size_t rootIndex = 0; rootIndex < roots_.size(); ++rootIndex)
                {
                    const auto& root = roots_[rootIndex];
                    auto& caseNames = packageCaseNames[root.prefix];
                    std::error_code error;
                    fs::recursive_directory_iterator current(root.directory, error), end;
                    if (error)
                    {
                        diagnose(anchor_, "Cannot inspect module source root: " + error.message());
                        continue;
                    }
                    while (current != end)
                    {
                        const auto path = current->path();
                        const auto filename = utf8Path(path.filename());
                        if (!filename.empty() && filename.front() == '.')
                        {
                            current.disable_recursion_pending();
                            current.increment(error);
                            if (error)
                            {
                                diagnose(anchor_, "Cannot traverse module source root: " + error.message());
                                break;
                            }
                            continue;
                        }
                        // 소스 후보만 검사해 관련 없는 데이터 심볼릭 링크의 대상을 따라가지 않는다.
                        // Inspect only source candidates so unrelated data symlinks are not followed.
                        const bool source = path.extension() == ".avx";
                        const bool file = source && current->is_regular_file(error);
                        if (error)
                        {
                            diagnose(anchor_, "Cannot inspect module path: " + utf8Path(path)
                                + " (" + error.message() + ")");
                            error.clear();
                        }
                        if (file)
                        {
                            const auto relative = relativeName(path.lexically_relative(root.directory));
                            if (relative)
                            {
                                const auto name = root.prefix.empty() ? *relative : root.prefix + "." + *relative;
                                const auto lower = folded(name);
                                const auto previous = caseNames.find(lower);
                                if (previous != caseNames.end() && previous->second != name)
                                    diagnose(anchor_, "Module names differ only by case: "
                                        + previous->second + " and " + name + ".");
                                else
                                    caseNames.emplace(lower, name);

                                const auto canonical = fs::canonical(path, error);
                                if (error)
                                {
                                    diagnose(anchor_, "Cannot resolve module file: " + utf8Path(path)
                                        + " (" + error.message() + ")");
                                    error.clear();
                                }
                                else
                                {
                                    auto& matches = byName_[name];
                                    const bool repeated = std::any_of(matches.begin(), matches.end(), [&](std::size_t index)
                                    {
                                        return candidates_[index].canonical == canonical;
                                    });
                                    if (!repeated)
                                    {
                                        matches.push_back(candidates_.size());
                                        candidates_.push_back({name, path, canonical, root.prefix, rootIndex});
                                    }
                                }
                            }
                        }
                        current.increment(error);
                        if (error)
                        {
                            diagnose(anchor_, "Cannot traverse module source root: " + error.message());
                            break;
                        }
                    }
                }
            }

            const Candidate* resolve(const std::string& name, SourceRange range)
            {
                if (!modulePath(name))
                {
                    diagnose(range, "Imports require a complete identifier-based module path: " + name);
                    return nullptr;
                }
                const auto found = byName_.find(name);
                if (found == byName_.end())
                {
                    const auto lower = folded(name);
                    const auto similar = std::find_if(candidates_.begin(), candidates_.end(), [&](const Candidate& candidate)
                    {
                        return folded(candidate.name) == lower;
                    });
                    diagnose(range, "Cannot find module '" + name + "'."
                        + (similar == candidates_.end() ? "" : " Exact spelling is '" + similar->name + "'."));
                    return nullptr;
                }
                if (found->second.size() != 1)
                {
                    std::string message = "Ambiguous module path '" + name + "':";
                    for (const auto index : found->second)
                        message += " " + utf8Path(candidates_[index].path);
                    diagnose(range, std::move(message));
                    return nullptr;
                }
                return &candidates_[found->second.front()];
            }

            ModuleId registerLibraryModule(const std::string& name, SourceRange range)
            {
                if (const auto found = loaded_.find(name); found != loaded_.end()) return found->second;
                std::string executableError;
                const auto executable = currentExecutable(executableError);
                if (executable.empty())
                { diagnose(range, "Cannot locate standard library relative to aevixc: " + executableError); return InvalidModuleId; }
                const auto directory = libraryDirectory(name);
                if (!directory) { diagnose(range, "Unknown standard library module: " + name); return InvalidModuleId; }
                const auto development = fs::path("share") / "aevix" / "library" / directory / (name + ".avx");
                auto path = executable.parent_path() / development;
                std::error_code error;
                if (!fs::is_regular_file(path, error))
                {
                    path = executable.parent_path().parent_path() / AEVIX_INSTALL_LANGUAGE_DIR / directory / (name + ".avx");
                    if (!fs::is_regular_file(path, error))
                        path = executable.parent_path().parent_path() / development;
                }
                std::string text;
                if (!read(path, range, text)) return InvalidModuleId;
                const auto source = sources_.add(utf8Path(path), std::move(text));
                auto tokens = Lexer(sources_.get(source), diagnostics_).tokenize();
                auto ast = Parser(std::move(tokens), diagnostics_).parseModule();
                if (!ast) return InvalidModuleId;
                if (name == "io" || name == "env" || name == "time" || name == "process" || name == "path" || name == "fs")
                    for (auto& declaration : ast->children)
                        if (declaration->kind == "Function" && declaration->value.starts_with("__")
                            && osOperation(std::string_view(declaration->value).substr(2))
                            && osOperation(std::string_view(declaration->value).substr(2))->module == name)
                            for (const auto& kind : {"CompilerOs", "CompilerIntrinsic"})
                            {
                                auto marker = makeNode(kind, declaration->value.substr(2), declaration->range);
                                declaration->children.push_back(std::move(marker));
                            }
                if (name == "numeric")
                    for (auto& declaration : ast->children)
                        if (declaration->kind == "Interface" && declaration->value == "Floating")
                        {
                            auto marker = std::make_unique<Node>(); marker->kind = "CompilerNumericContract";
                            marker->value = "float"; marker->range = declaration->range;
                            declaration->children.push_back(std::move(marker));
                        }
                if (name == "complex")
                    for (auto& declaration : ast->children)
                        if (declaration->kind == "Struct" && declaration->value == "Complex")
                            for (auto& method : declaration->children)
                                if (method->kind == "Function" && (method->value == "magnitude" || method->value == "divide_components"))
                                {
                                    auto marker = std::make_unique<Node>(); marker->kind = "CompilerComplexMath";
                                    marker->value = method->value; marker->range = method->range;
                                    method->children.push_back(std::move(marker));
                                }
                const auto id = graph_.modules.size();
                auto module = std::make_unique<SourceModule>();
                module->name = name; module->package = "$" + name; module->path = std::move(path);
                module->source = source; module->ast = std::move(ast);
                graph_.modules.push_back(std::move(module)); loaded_[name] = id;
                return id;
            }

            ModuleId registerSystemModule()
            {
                if (const auto found = loaded_.find("system"); found != loaded_.end()) return found->second;
                const auto source = sources_.add("<aevix-system>", R"AEVIXDOC(/*
 * Lifecycle of the system public module.
 * @module system
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */

public import error { IoError, ReadError, FormatError };
/*
 * Version history for the system.LogLevel public API.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public enum LogLevel implements Copy { /*
 * Version history for the system.LogLevel.info public API.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
info; /*
 * Version history for the system.LogLevel.warn public API.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
warn; /*
 * Version history for the system.LogLevel.error public API.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
error; };
/*
 * Version history for the system.info public API.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public const LogLevel info = LogLevel.info;
/*
 * Version history for the system.warn public API.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public const LogLevel warn = LogLevel.warn;
/*
 * Version history for the system.error public API.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public const LogLevel error = LogLevel.error;
/*
 * Version history for the system.print public API.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public Result<unit, IoError> print<T>(T value) {}
/*
 * Version history for the system.println public API.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public Result<unit, IoError> println<T>(T value) {}
/*
 * Version history for the system.printf public API.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public Result<unit, IoError> printf<T>(string_view format, T values[]) {}
/*
 * Version history for the system.flush public API.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public Result<unit, IoError> flush() {}
/*
 * Version history for the system.log public API.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public Result<unit, IoError> log(LogLevel level, string_view message, string_view code = "") {}
/*
 * Version history for the system.input public API.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public Result<Optional<T>, ReadError> input<T = string>() {}
)AEVIXDOC");
                auto tokens = Lexer(sources_.get(source), diagnostics_).tokenize();
                auto ast = Parser(std::move(tokens), diagnostics_).parseModule();
                if (!ast) return InvalidModuleId;
                for (auto& declaration : ast->children)
                {
                    if (declaration->kind == "Function")
                        for (const auto& kind : {"CompilerSystem", "CompilerIntrinsic"})
                        {
                            auto marker = std::make_unique<Node>(); marker->kind = kind;
                            marker->value = declaration->value; marker->range = declaration->range;
                            declaration->children.push_back(std::move(marker));
                        }
                    if (declaration->kind == "Variable")
                        for (const auto& kind : {"CompilerSystemLevel", "CompilerIntrinsic"})
                        {
                            auto marker = std::make_unique<Node>(); marker->kind = kind;
                            marker->value = declaration->value; marker->range = declaration->range;
                            declaration->children.push_back(std::move(marker));
                        }
                }
                const auto id = graph_.modules.size();
                auto module = std::make_unique<SourceModule>();
                module->name = "system"; module->package = "$system"; module->path = "<aevix-system>";
                module->source = source; module->ast = std::move(ast);
                graph_.modules.push_back(std::move(module)); loaded_["system"] = id;
                return id;
            }

            ModuleId registerCpuModule(const std::string& name)
            {
                if (const auto found = loaded_.find(name); found != loaded_.end()) return found->second;
                const auto source = sources_.add("<aevix-" + name + ">", name == "simd"
                    ? R"AEVIXDOC(/*
 * Lifecycle of the simd public module.
 * @module simd
 * @since 0.2
 * @deprecated_at none
 * @removed_at none
 */
/*
 * Version history for the simd.Simd public API.
 * @since 0.2
 * @deprecated_at none
 * @removed_at none
 */
public struct Simd {}; /*
 * Version history for the simd.SimdMask public API.
 * @since 0.2
 * @deprecated_at none
 * @removed_at none
 */
public struct SimdMask {};)AEVIXDOC"
                    : R"AEVIXDOC(/*
 * Lifecycle of the parallel public module.
 * @module parallel
 * @since 0.2
 * @deprecated_at none
 * @removed_at none
 */
/*
 * Version history for the parallel.for_each public API.
 * @since 0.2
 * @deprecated_at none
 * @removed_at none
 */
public void for_each() {} /*
 * Version history for the parallel.worker_count public API.
 * @since 0.2
 * @deprecated_at none
 * @removed_at none
 */
public usize worker_count() { return 0; })AEVIXDOC");
                auto tokens = Lexer(sources_.get(source), diagnostics_).tokenize();
                auto ast = Parser(std::move(tokens), diagnostics_).parseModule();
                if (!ast) return InvalidModuleId;
                for (auto& declaration : ast->children)
                {
                    auto marker = std::make_unique<Node>();
                    marker->kind = name == "simd" ? "CompilerSimd" : "CompilerParallel";
                    marker->value = declaration->value; marker->range = declaration->range;
                    declaration->children.push_back(std::move(marker));
                    if (name == "parallel")
                    {
                        auto intrinsic = std::make_unique<Node>(); intrinsic->kind = "CompilerIntrinsic";
                        intrinsic->range = declaration->range; declaration->children.push_back(std::move(intrinsic));
                    }
                }
                const auto id = graph_.modules.size();
                auto module = std::make_unique<SourceModule>();
                module->name = name; module->package = "$" + name; module->path = "<aevix-" + name + ">";
                module->source = source; module->ast = std::move(ast);
                graph_.modules.push_back(std::move(module)); loaded_[name] = id;
                return id;
            }

            ModuleId registerSyncModule()
            {
                if (const auto found = loaded_.find("sync"); found != loaded_.end()) return found->second;
                const auto source = sources_.add("<aevix-sync>", std::string(SyncModuleSource));
                auto tokens = Lexer(sources_.get(source), diagnostics_).tokenize();
                auto ast = Parser(std::move(tokens), diagnostics_).parseModule();
                if (!ast) return InvalidModuleId;
                const auto mark = [](Node& target, std::string kind)
                { auto marker = std::make_unique<Node>(); marker->kind = std::move(kind); marker->range = target.range; target.children.push_back(std::move(marker)); };
                for (auto& declaration : ast->children)
                    if (declaration->kind == "Struct")
                    {
                        const auto& name = declaration->value;
                        const auto marker = name == "Mutex" ? "CompilerMutex" : name == "RwLock" ? "CompilerRwLock"
                            : name == "MutexGuard" ? "CompilerMutexGuard" : name == "RwLockReadGuard" ? "CompilerReadGuard"
                            : name == "RwLockWriteGuard" ? "CompilerWriteGuard" : "";
                        if (name == "Poisoned")
                        {
                            mark(*declaration, "CompilerPoisoned");
                            for (auto& method : declaration->children)
                                if (method->kind == "Function" && method->value == "into_inner") mark(*method, "CompilerIntrinsic");
                            continue;
                        }
                        if (!*marker) continue;
                        mark(*declaration, marker);
                        for (auto& method : declaration->children) if (method->kind == "Function") mark(*method, "CompilerIntrinsic");
                    }
                const auto id = graph_.modules.size(); auto module = std::make_unique<SourceModule>();
                module->name = "sync"; module->package = "$sync"; module->path = "<aevix-sync>"; module->source = source; module->ast = std::move(ast);
                graph_.modules.push_back(std::move(module)); loaded_["sync"] = id; return id;
            }

            ModuleId registerTaskModule()
            {
                if (const auto found = loaded_.find("task"); found != loaded_.end()) return found->second;
                const auto source = sources_.add("<aevix-task>", std::string(TaskModuleSource));
                auto tokens = Lexer(sources_.get(source), diagnostics_).tokenize();
                auto ast = Parser(std::move(tokens), diagnostics_).parseModule();
                if (!ast) return InvalidModuleId;
                const auto mark = [](Node& target, std::string kind, std::string value = {})
                { auto marker = std::make_unique<Node>(); marker->kind = std::move(kind); marker->value = std::move(value); marker->range = target.range; target.children.push_back(std::move(marker)); };
                for (auto& declaration : ast->children)
                {
                    if (declaration->kind != "Struct") continue;
                    const auto owner = declaration->value;
                    if (owner == "CancellationSource") mark(*declaration, "CompilerCancellationSource");
                    else if (owner == "CancellationToken") mark(*declaration, "CompilerCancellationToken");
                    else if (owner == "TaskScope") { mark(*declaration,"CompilerTaskScope"); continue; }
                    else if (owner == "TaskId") { mark(*declaration,"CompilerTaskId"); continue; }
                    else if (owner == "Task") mark(*declaration,"CompilerTask");
                    else continue;
                    for (auto& method : declaration->children)
                    {
                        if (method->kind != "Function") continue;
                        std::string operation;
                        if (owner == "CancellationSource")
                            operation = method->value == "new" ? "source_new" : method->value == "clone" ? "source_clone"
                                : method->value == "token" ? "token_from_source" : method->value == "request" ? "source_request" : "";
                        else if (owner == "CancellationToken") operation = method->value == "clone" ? "token_clone" : method->value == "requested" ? "token_requested" : "";
                        else operation = method->value == "cancel_source" ? "source_new" : method->value == "cancel_token" ? "token_from_source"
                            : method->value == "cancel_clone" ? "token_clone" : method->value == "cancel_request" ? "source_request" : method->value == "cancel_requested" ? "token_requested" : "";
                        if (!operation.empty()) { mark(*method, "CompilerCancellation", operation); mark(*method, "CompilerIntrinsic"); }
                        else if (owner == "Task")
                        {
                            if (method->value == "deadline_after")
                            {
                                mark(*method,"CompilerTaskSourceAlias","deadline_after"); mark(*method,"CompilerIntrinsic");
                                continue;
                            }
                            const auto action = method->value == "open_scope" ? "scope_open" : method->value == "close_scope" ? "scope_close" : method->value;
                            if (taskOperation(action)) { mark(*method,"CompilerTaskOperation",action); mark(*method,"CompilerIntrinsic"); }
                        }
                    }
                }
                const auto id = graph_.modules.size(); auto module = std::make_unique<SourceModule>();
                module->name = "task"; module->package = "$task"; module->path = "<aevix-task>"; module->source = source; module->ast = std::move(ast);
                graph_.modules.push_back(std::move(module)); loaded_["task"] = id; return id;
            }

            ModuleId registerAtomicModule()
            {
                if (const auto found = loaded_.find("atomic"); found != loaded_.end()) return found->second;
                const auto source = sources_.add("<aevix-atomic>", std::string(AtomicModuleSource));
                auto tokens = Lexer(sources_.get(source), diagnostics_).tokenize();
                auto ast = Parser(std::move(tokens), diagnostics_).parseModule();
                if (!ast) return InvalidModuleId;
                const auto mark = [](Node& target, std::string kind)
                { auto marker = std::make_unique<Node>(); marker->kind = std::move(kind); marker->range = target.range; target.children.push_back(std::move(marker)); };
                for (auto& declaration : ast->children)
                    if (declaration->kind == "Struct" && declaration->value == "Atomic")
                    {
                        mark(*declaration, "CompilerAtomic");
                        for (auto& method : declaration->children) if (method->kind == "Function") mark(*method, "CompilerIntrinsic");
                    }
                const auto id = graph_.modules.size(); auto module = std::make_unique<SourceModule>();
                module->name = "atomic"; module->package = "$atomic"; module->path = "<aevix-atomic>"; module->source = source; module->ast = std::move(ast);
                graph_.modules.push_back(std::move(module)); loaded_["atomic"] = id; return id;
            }

            ModuleId registerMemoryModule()
            {
                if (const auto found = loaded_.find("memory"); found != loaded_.end()) return found->second;
                const auto source = sources_.add("<aevix-memory>", R"AEVIXDOC(/*
 * Lifecycle of the memory public module.
 * @module memory
 * @since 0.1
 * @deprecated_at none
 * @removed_at none
 */

/*
 * Version history for the memory.Box public API.
 * @since 0.1
 * @deprecated_at none
 * @removed_at none
 */
public struct Box<T> {
    T payload;
    /*
     * Version history for the memory.Box.new public API.
     * @since 0.1
     * @deprecated_at none
     * @removed_at none
     */
    public void new(T value) {}
    /*
     * Consume one concrete Box and move its payload out, releasing only the outer allocation.
     * @since 0.3
     * @deprecated_at none
     * @removed_at none
     */
    public void into_inner(Self owner) {}
    /*
     * Version history for the memory.Box.into_dyn public API.
     * @since 0.1
     * @deprecated_at none
     * @removed_at none
     */
    public void into_dyn(T value) {}
    /*
     * Version history for the memory.Box.as_ref public API.
     * @since 0.1
     * @deprecated_at none
     * @removed_at none
     */
    public void as_ref(&Self owner) {}
    /*
     * Version history for the memory.Box.as_mut public API.
     * @since 0.1
     * @deprecated_at none
     * @removed_at none
     */
    public void as_mut(&mut Self owner) {}
};
/*
 * Moves a replacement into initialized storage and returns its previous value without dropping either.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public T replace<T>(&mut T target, T replacement) { return replacement; }
/*
 * Exchanges initialized exclusive values without copying, allocating, or invoking Drop.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public unit swap<T>(&mut T left, &mut T right) { return Unit.value(); }

/*
 * Checked reference-count growth failure; count arithmetic never wraps.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public enum RefCountError implements Copy {

/*
 * The requested additional ownership count would overflow.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
    CountOverflow;
};

/*
 * Strong shared payload owner; construction and count clones are explicit.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public struct Shared<T> {
    private T value;

/*
 * Compiler-managed Shared.new; lifetime and ownership effects are checked before execution.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
    public void new(T value) {}

/*
 * Compiler-managed Shared.clone; lifetime and ownership effects are checked before execution.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
    public void clone(&Shared<T> owner) {}

/*
 * Compiler-managed Shared.as_ref; lifetime and ownership effects are checked before execution.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
    public void as_ref(&Shared<T> owner) {}

/*
 * Compiler-managed Shared.downgrade; lifetime and ownership effects are checked before execution.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
    public void downgrade(&Shared<T> owner) {}

/*
 * Compiler-managed Shared.get_mut; lifetime and ownership effects are checked before execution.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
    public void get_mut(&mut Shared<T> owner) {}

/*
 * Compiler-managed Shared.try_unwrap; lifetime and ownership effects are checked before execution.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
    public void try_unwrap(Shared<T> owner) {}

/*
 * Compiler-managed Shared.strong_count; lifetime and ownership effects are checked before execution.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
    public void strong_count(&Shared<T> owner) {}

/*
 * Compiler-managed Shared.weak_count; lifetime and ownership effects are checked before execution.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
    public void weak_count(&Shared<T> owner) {}

/*
 * Compiler-managed Shared.ptr_equal; lifetime and ownership effects are checked before execution.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
    public void ptr_equal(&Shared<T> left, &Shared<T> right) {}
};

/*
 * Weak allocation identity; retaining metadata never resurrects a destroyed payload.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public struct Weak<T> {
    private T value;

/*
 * Compiler-managed Weak.new; lifetime and ownership effects are checked before execution.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
    public void new() {}

/*
 * Compiler-managed Weak.clone; lifetime and ownership effects are checked before execution.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
    public void clone(&Weak<T> owner) {}

/*
 * Compiler-managed Weak.upgrade; lifetime and ownership effects are checked before execution.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
    public void upgrade(&Weak<T> owner) {}

/*
 * Compiler-managed Weak.strong_count; lifetime and ownership effects are checked before execution.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
    public void strong_count(&Weak<T> owner) {}

/*
 * Compiler-managed Weak.weak_count; lifetime and ownership effects are checked before execution.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
    public void weak_count(&Weak<T> owner) {}

/*
 * Compiler-managed Weak.ptr_equal; lifetime and ownership effects are checked before execution.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
    public void ptr_equal(&Weak<T> left, &Weak<T> right) {}

/*
 * Compiler-managed Weak.is_empty; lifetime and ownership effects are checked before execution.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
    public void is_empty(&Weak<T> owner) {}

/*
 * Compiler-managed Weak.is_expired; lifetime and ownership effects are checked before execution.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
    public void is_expired(&Weak<T> owner) {}

/*
 * Compiler-managed Weak.clear; lifetime and ownership effects are checked before execution.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
    public void clear(&mut Weak<T> owner) {}
};
)AEVIXDOC");
                auto tokens = Lexer(sources_.get(source), diagnostics_).tokenize();
                auto ast = Parser(std::move(tokens), diagnostics_).parseModule();
                if (!ast) return InvalidModuleId;
                const auto mark = [](Node& owner, std::string kind)
                { auto marker = std::make_unique<Node>(); marker->kind = std::move(kind); marker->range = owner.range; owner.children.push_back(std::move(marker)); };
                auto reexport = std::make_unique<Node>(); reexport->kind = "Import"; reexport->value = "$core"; reexport->range = ast->range;
                for (const auto& [kind, value] : std::vector<std::pair<std::string, std::string>>{{"ImportName", "InputError"}, {"Modifier", "public"}, {"CompilerImport", ""}})
                { auto item = std::make_unique<Node>(); item->kind = kind; item->value = value; item->range = ast->range; reexport->children.push_back(std::move(item)); }
                ast->children.push_back(std::move(reexport));
                for (auto& declaration : ast->children)
                {
                    if (declaration->kind == "Struct" && declaration->value == "Box")
                    {
                        mark(*declaration, "CompilerBox");
                        for (auto& method : declaration->children) if (method->kind == "Function") mark(*method, "CompilerIntrinsic");
                    }
                    if (declaration->kind == "Struct" && (declaration->value == "Shared" || declaration->value == "Weak"))
                    {
                        mark(*declaration, declaration->value == "Shared" ? "CompilerShared" : "CompilerWeak");
                        for (auto& method : declaration->children) if (method->kind == "Function") mark(*method, "CompilerIntrinsic");
                    }
                    if (declaration->kind == "Function" && (declaration->value == "replace" || declaration->value == "swap"))
                    { mark(*declaration, "CompilerMemory"); mark(*declaration, "CompilerIntrinsic"); }
                }
                const auto id = graph_.modules.size();
                auto module = std::make_unique<SourceModule>();
                module->name = "memory"; module->package = "$memory"; module->path = "<aevix-memory>"; module->source = source; module->ast = std::move(ast);
                graph_.modules.push_back(std::move(module)); loaded_["memory"] = id;
                return id;
            }

            ModuleId registerModule(const Candidate& candidate, SourceRange range, SourceId existingSource = InvalidSourceId)
            {
                const auto named = loaded_.find(candidate.name);
                if (named != loaded_.end())
                    return named->second;
                const auto physical = byFile_.find(candidate.canonical);
                if (physical != byFile_.end())
                {
                    diagnose(range, "One source file cannot have multiple module names: '"
                        + graph_.modules[physical->second]->name + "' and '" + candidate.name + "'.");
                    return InvalidModuleId;
                }

                auto source = existingSource;
                if (source == InvalidSourceId)
                {
                    std::string text;
                    if (!read(candidate.path, range, text))
                        return InvalidModuleId;
                    source = sources_.add(utf8Path(candidate.path), std::move(text));
                }
                const auto id = graph_.modules.size();
                auto module = std::make_unique<SourceModule>();
                module->name = candidate.name;
                module->path = candidate.path;
                module->package = candidate.package;
                module->source = source;
                graph_.modules.push_back(std::move(module));
                loaded_.emplace(candidate.name, id);
                byFile_.emplace(candidate.canonical, id);

                const auto before = diagnostics_.size();
                auto tokens = Lexer(sources_.get(source), diagnostics_).tokenize();
                if (diagnostics_.size() == before)
                    graph_.modules[id]->ast = Parser(std::move(tokens), diagnostics_).parseModule();
                return id;
            }

            void checkPackageCycles()
            {
                std::map<std::string, std::size_t> packages;
                for (const auto& module : graph_.modules)
                    if (!packages.contains(module->package))
                        packages.emplace(module->package, packages.size());
                std::vector<std::string> names(packages.size());
                for (const auto& [name, index] : packages)
                    names[index] = name.empty() ? "<project>" : name;

                struct Edge final
                {
                    std::size_t target;
                    SourceRange range;
                };
                std::vector<std::vector<Edge>> adjacency(packages.size());
                std::set<std::pair<std::size_t, std::size_t>> recorded;
                for (const auto& edge : edges_)
                {
                    const auto from = packages.at(graph_.modules[edge.from]->package);
                    const auto to = packages.at(graph_.modules[edge.to]->package);
                    if (from != to && recorded.emplace(from, to).second)
                        adjacency[from].push_back({to, edge.range});
                }

                // 패키지 그래프는 모듈 순환과 별개로 검사하며 재귀 호출을 피한다.
                // Check package cycles independently of module cycles without recursive calls.
                std::vector<unsigned char> state(packages.size(), 0);
                struct Frame final { std::size_t package; std::size_t next; };
                std::vector<Frame> stack;
                for (std::size_t initial = 0; initial < packages.size(); ++initial)
                {
                    if (state[initial] != 0)
                        continue;
                    state[initial] = 1;
                    stack.push_back({initial, 0});
                    while (!stack.empty())
                    {
                        auto& frame = stack.back();
                        if (frame.next == adjacency[frame.package].size())
                        {
                            state[frame.package] = 2;
                            stack.pop_back();
                            continue;
                        }
                        const auto edge = adjacency[frame.package][frame.next++];
                        if (state[edge.target] == 0)
                        {
                            state[edge.target] = 1;
                            stack.push_back({edge.target, 0});
                        }
                        else if (state[edge.target] == 1)
                        {
                            std::string message = "Package dependency cycle is not allowed: ";
                            const auto begin = std::find_if(stack.begin(), stack.end(), [&](const Frame& value)
                            {
                                return value.package == edge.target;
                            });
                            for (auto it = begin; it != stack.end(); ++it)
                                message += names[it->package] + " -> ";
                            diagnose(edge.range, message + names[edge.target] + ".");
                        }
                    }
                }
            }
        };
    }

    ModuleId ModuleGraph::find(std::string_view name) const noexcept
    {
        for (ModuleId id = 0; id < modules.size(); ++id)
            if (modules[id]->name == name)
                return id;
        return InvalidModuleId;
    }

    ModuleLoader::ModuleLoader(SourceManager& sources, Diagnostics& diagnostics)
        : sources_(sources), diagnostics_(diagnostics)
    {
    }

    std::optional<ModuleGraph> ModuleLoader::load(const std::filesystem::path& entry, const ModuleOptions& options)
    {
        return Loader(sources_, diagnostics_).run(entry, options);
    }
}
