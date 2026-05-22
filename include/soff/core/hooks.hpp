#pragma once

#include "soff/analysis/model.hpp"
#include "soff/diff/heuristics.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace soff {

struct MatchContext
{
    Address primary = 0;
    std::string primary_name;
    Address secondary = 0;
    std::string secondary_name;
    std::string description;
    double ratio = 0.0;
    int primary_nodes = 0;
    int secondary_nodes = 0;
};

struct MatchDecision
{
    bool accept = true;
    double adjusted_ratio = 0.0;
};

class DiffHooks
{
public:
    virtual ~DiffHooks() = default;

    virtual std::vector<diff::HeuristicDefinition> get_heuristics(
        diff::HeuristicCategory category,
        const std::vector<diff::HeuristicDefinition>& heuristics)
    {
        return heuristics;
    }

    virtual std::optional<std::string> on_launch_heuristic(
        std::string_view name,
        std::string_view sql)
    {
        (void)name;
        return std::string(sql);
    }

    virtual std::string get_queries_postfix(
        diff::HeuristicCategory category,
        std::string_view current_postfix)
    {
        (void)category;
        return std::string(current_postfix);
    }

    virtual MatchDecision on_match(const MatchContext& context)
    {
        return {true, context.ratio};
    }

    virtual void on_finish() {}
};

/// Export phase hooks - called during IDB export
class ExportHooks
{
public:
    virtual ~ExportHooks() = default;

    /// Called before exporting a function. Return false to skip this function.
    virtual bool before_export_function(Address ea, std::string_view name)
    {
        (void)ea;
        (void)name;
        return true;
    }

    /// Called after a function is exported. Can modify the feature before it's saved.
    virtual void after_export_function(FunctionFeature& feature)
    {
        (void)feature;
    }

    /// Called if the export crashes or is cancelled.
    virtual void on_export_crash(Address last_ea, std::string_view last_name)
    {
        (void)last_ea;
        (void)last_name;
    }
};

} // namespace soff
