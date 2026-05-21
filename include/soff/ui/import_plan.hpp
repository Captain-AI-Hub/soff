#pragma once

#include "soff/analysis/model.hpp"
#include "soff/db/result_repository.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace soff::ui {

enum class ImportOperation
{
    rename_function,
    apply_prototype,
    set_function_comment,
    set_function_flags,
    set_instruction_comment,
    set_repeatable_instruction_comment,
    set_forced_operand,
    set_pseudocode_comment,
};

struct ImportPlanOptions
{
    bool include_best = true;
    bool include_partial = true;
    bool include_unreliable = false;
    bool skip_auto_names = true;
    bool import_function_names = true;
    bool import_prototypes = true;
    bool import_function_comments = true;
    bool import_function_flags = false;
    bool import_instruction_comments = true;
    bool import_forced_operands = true;
    bool import_pseudocode_comments = true;
    double minimum_ratio = 0.5;
};

struct ImportPlanItem
{
    ImportOperation operation = ImportOperation::rename_function;
    Address address = 0;
    Address secondary_address = 0;
    std::string current_name;
    std::string imported_name;
    std::string value;
    std::uint64_t numeric_value = 0;
    double ratio = 0.0;
    std::string description;
};

struct ImportPlan
{
    std::vector<ImportPlanItem> items;
    std::size_t skipped_same_name = 0;
    std::size_t skipped_auto_name = 0;
    std::size_t skipped_kind = 0;
    std::size_t skipped_ratio = 0;
    std::size_t skipped_empty_name = 0;
    std::size_t skipped_missing_source = 0;
    std::size_t function_renames = 0;
    std::size_t prototypes = 0;
    std::size_t function_comments = 0;
    std::size_t function_flags = 0;
    std::size_t instruction_comments = 0;
    std::size_t repeatable_instruction_comments = 0;
    std::size_t forced_operands = 0;
    std::size_t pseudocode_comments = 0;
};

ImportPlan build_import_plan(
    const db::DiffResultSet& results,
    const ImportPlanOptions& options = {});

ImportPlan build_import_plan(
    const db::DiffResultSet& results,
    const ProgramSnapshot& primary,
    const ProgramSnapshot& secondary,
    const ImportPlanOptions& options = {});

} // namespace soff::ui
