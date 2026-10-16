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

#include <algorithm>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <set>
#include <system_error>
#include <utility>

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
                        const bool file = current->is_regular_file(error);
                        if (error)
                        {
                            diagnose(anchor_, "Cannot inspect module path: " + utf8Path(path)
                                + " (" + error.message() + ")");
                            error.clear();
                        }
                        if (file && path.extension() == ".avx")
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

            ModuleId registerMemoryModule()
            {
                if (const auto found = loaded_.find("memory"); found != loaded_.end()) return found->second;
                const auto source = sources_.add("<aevix-memory>", R"(
public struct Box<T> {
    T payload;
    public void new(T value) {}
    public void into_dyn(T value) {}
    public void as_ref(&Self owner) {}
    public void as_mut(&mut Self owner) {}
};
)");
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
                    if (declaration->kind == "Struct" && declaration->value == "Box")
                    {
                        mark(*declaration, "CompilerBox");
                        for (auto& method : declaration->children) if (method->kind == "Function") mark(*method, "CompilerIntrinsic");
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
