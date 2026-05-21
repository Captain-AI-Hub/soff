#include "soff/ui/import_plan.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <unordered_map>

namespace soff::ui {

namespace {

std::string lower_copy(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

bool starts_with(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size()
        && text.substr(0, prefix.size()) == prefix;
}

bool is_auto_name(const std::string& name)
{
    const auto lowered = lower_copy(name);
    constexpr std::string_view prefixes[] = {
        "sub_",
        "nullsub_",
        "j_",
        "loc_",
        "byte_",
        "word_",
        "dword_",
        "qword_",
        "off_",
        "unk_",
    };

    return std::any_of(std::begin(prefixes), std::end(prefixes), [&](std::string_view prefix) {
        return starts_with(lowered, prefix);
    });
}

bool include_kind(db::ResultKind kind, const ImportPlanOptions& options)
{
    switch (kind) {
    case db::ResultKind::best:
        return options.include_best;
    case db::ResultKind::partial:
        return options.include_partial;
    case db::ResultKind::unreliable:
        return options.include_unreliable;
    case db::ResultKind::multimatch:
        return false;
    }
    return false;
}

void add_item(ImportPlan& plan, ImportPlanItem item)
{
    switch (item.operation) {
    case ImportOperation::rename_function:
        ++plan.function_renames;
        break;
    case ImportOperation::apply_prototype:
        ++plan.prototypes;
        break;
    case ImportOperation::set_function_comment:
        ++plan.function_comments;
        break;
    case ImportOperation::set_function_flags:
        ++plan.function_flags;
        break;
    case ImportOperation::set_instruction_comment:
        ++plan.instruction_comments;
        break;
    case ImportOperation::set_repeatable_instruction_comment:
        ++plan.repeatable_instruction_comments;
        break;
    case ImportOperation::set_forced_operand:
        ++plan.forced_operands;
        break;
    case ImportOperation::set_pseudocode_comment:
        ++plan.pseudocode_comments;
        break;
    }
    plan.items.push_back(std::move(item));
}

std::unordered_map<Address, const FunctionFeature*> function_map(const ProgramSnapshot& snapshot)
{
    std::unordered_map<Address, const FunctionFeature*> map;
    map.reserve(snapshot.functions.size());
    for (const auto& function : snapshot.functions) {
        map[function.address] = &function;
    }
    return map;
}

bool consume_char(std::string_view text, std::size_t& pos, char expected)
{
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
    if (pos >= text.size() || text[pos] != expected) {
        return false;
    }
    ++pos;
    return true;
}

bool consume_integer(std::string_view text, std::size_t& pos, int& value)
{
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
    if (pos >= text.size() || !std::isdigit(static_cast<unsigned char>(text[pos]))) {
        return false;
    }
    value = 0;
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
        value = value * 10 + (text[pos] - '0');
        ++pos;
    }
    return true;
}

bool consume_json_string(std::string_view text, std::size_t& pos, std::string& value)
{
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
    if (pos >= text.size() || text[pos] != '"') {
        return false;
    }
    ++pos;
    value.clear();
    while (pos < text.size()) {
        const char ch = text[pos++];
        if (ch == '"') {
            return true;
        }
        if (ch == '\\' && pos < text.size()) {
            value.push_back(text[pos++]);
        } else {
            value.push_back(ch);
        }
    }
    return false;
}

std::vector<std::pair<int, std::string>> parse_forced_operands(std::string_view text)
{
    std::vector<std::pair<int, std::string>> operands;
    if (text.empty()) {
        return operands;
    }

    std::size_t pos = 0;
    if (!consume_char(text, pos, '[')) {
        return operands;
    }
    while (pos < text.size()) {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
            ++pos;
        }
        if (pos < text.size() && text[pos] == ']') {
            break;
        }

        int index = 0;
        std::string name;
        if (!consume_char(text, pos, '[')
            || !consume_integer(text, pos, index)
            || !consume_char(text, pos, ',')
            || !consume_json_string(text, pos, name)
            || !consume_char(text, pos, ']')) {
            return {};
        }
        if (!name.empty()) {
            operands.emplace_back(index, std::move(name));
        }
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
            ++pos;
        }
        if (pos < text.size() && text[pos] == ',') {
            ++pos;
        }
    }
    return operands;
}

} // namespace

ImportPlan build_import_plan(const db::DiffResultSet& results, const ImportPlanOptions& options)
{
    ImportPlan plan;
    for (const auto& match : results.matches) {
        if (!include_kind(match.kind, options)) {
            ++plan.skipped_kind;
            continue;
        }
        if (match.ratio < options.minimum_ratio) {
            ++plan.skipped_ratio;
            continue;
        }
        if (match.secondary_name.empty()) {
            ++plan.skipped_empty_name;
            continue;
        }
        if (match.primary_name == match.secondary_name) {
            ++plan.skipped_same_name;
            continue;
        }
        if (options.skip_auto_names && is_auto_name(match.secondary_name)) {
            ++plan.skipped_auto_name;
            continue;
        }

        ImportPlanItem item;
        item.operation = ImportOperation::rename_function;
        item.address = match.primary;
        item.secondary_address = match.secondary;
        item.current_name = match.primary_name;
        item.imported_name = match.secondary_name;
        item.value = match.secondary_name;
        item.ratio = match.ratio;
        item.description = match.description;
        add_item(plan, std::move(item));
    }
    return plan;
}

ImportPlan build_import_plan(
    const db::DiffResultSet& results,
    const ProgramSnapshot& primary,
    const ProgramSnapshot& secondary,
    const ImportPlanOptions& options)
{
    ImportPlan plan;
    const auto primary_functions = function_map(primary);
    const auto secondary_functions = function_map(secondary);

    for (const auto& match : results.matches) {
        if (!include_kind(match.kind, options)) {
            ++plan.skipped_kind;
            continue;
        }
        if (match.ratio < options.minimum_ratio) {
            ++plan.skipped_ratio;
            continue;
        }

        const auto primary_it = primary_functions.find(match.primary);
        const auto secondary_it = secondary_functions.find(match.secondary);
        if (primary_it == primary_functions.end() || secondary_it == secondary_functions.end()) {
            ++plan.skipped_missing_source;
            continue;
        }
        const auto& primary_function = *primary_it->second;
        const auto& secondary_function = *secondary_it->second;

        if (options.import_function_names) {
            if (secondary_function.name.empty()) {
                ++plan.skipped_empty_name;
            } else if (primary_function.name == secondary_function.name) {
                ++plan.skipped_same_name;
            } else if (options.skip_auto_names && is_auto_name(secondary_function.name)) {
                ++plan.skipped_auto_name;
            } else {
                ImportPlanItem item;
                item.operation = ImportOperation::rename_function;
                item.address = primary_function.address;
                item.secondary_address = secondary_function.address;
                item.current_name = primary_function.name;
                item.imported_name = secondary_function.name;
                item.value = secondary_function.name;
                item.ratio = match.ratio;
                item.description = match.description;
                add_item(plan, std::move(item));
            }
        }

        if (options.import_prototypes && !secondary_function.prototype.empty()
            && primary_function.prototype != secondary_function.prototype) {
            ImportPlanItem item;
            item.operation = ImportOperation::apply_prototype;
            item.address = primary_function.address;
            item.secondary_address = secondary_function.address;
            item.current_name = primary_function.name;
            item.imported_name = secondary_function.name;
            item.value = secondary_function.prototype;
            item.ratio = match.ratio;
            item.description = match.description;
            add_item(plan, std::move(item));
        }

        if (options.import_function_comments && !secondary_function.comment.empty()
            && primary_function.comment != secondary_function.comment) {
            ImportPlanItem item;
            item.operation = ImportOperation::set_function_comment;
            item.address = primary_function.address;
            item.secondary_address = secondary_function.address;
            item.current_name = primary_function.name;
            item.imported_name = secondary_function.name;
            item.value = secondary_function.comment;
            item.ratio = match.ratio;
            item.description = match.description;
            add_item(plan, std::move(item));
        }

        if (options.import_function_flags && secondary_function.function_flags != 0
            && primary_function.function_flags != secondary_function.function_flags) {
            ImportPlanItem item;
            item.operation = ImportOperation::set_function_flags;
            item.address = primary_function.address;
            item.secondary_address = secondary_function.address;
            item.current_name = primary_function.name;
            item.imported_name = secondary_function.name;
            item.numeric_value = secondary_function.function_flags;
            item.ratio = match.ratio;
            item.description = match.description;
            add_item(plan, std::move(item));
        }

        const auto aligned_instructions = std::min(
            primary_function.instruction_details.size(),
            secondary_function.instruction_details.size());
        for (std::size_t index = 0; index < aligned_instructions; ++index) {
            const auto& primary_instruction = primary_function.instruction_details[index];
            const auto& secondary_instruction = secondary_function.instruction_details[index];
            if (options.import_instruction_comments && !secondary_instruction.comment1.empty()
                && primary_instruction.comment1 != secondary_instruction.comment1) {
                ImportPlanItem item;
                item.operation = ImportOperation::set_instruction_comment;
                item.address = primary_instruction.address;
                item.secondary_address = secondary_instruction.address;
                item.current_name = primary_function.name;
                item.imported_name = secondary_function.name;
                item.value = secondary_instruction.comment1;
                item.ratio = match.ratio;
                item.description = match.description;
                add_item(plan, std::move(item));
            }
            if (options.import_instruction_comments && !secondary_instruction.comment2.empty()
                && primary_instruction.comment2 != secondary_instruction.comment2) {
                ImportPlanItem item;
                item.operation = ImportOperation::set_repeatable_instruction_comment;
                item.address = primary_instruction.address;
                item.secondary_address = secondary_instruction.address;
                item.current_name = primary_function.name;
                item.imported_name = secondary_function.name;
                item.value = secondary_instruction.comment2;
                item.ratio = match.ratio;
                item.description = match.description;
                add_item(plan, std::move(item));
            }
            if (options.import_pseudocode_comments && !secondary_instruction.pseudocomment.empty()
                && primary_instruction.pseudocomment != secondary_instruction.pseudocomment) {
                ImportPlanItem item;
                item.operation = ImportOperation::set_pseudocode_comment;
                item.address = primary_instruction.address;
                item.secondary_address = secondary_instruction.address;
                item.current_name = primary_function.name;
                item.imported_name = secondary_function.name;
                item.value = secondary_instruction.pseudocomment;
                item.numeric_value = secondary_instruction.pseudoitp;
                item.ratio = match.ratio;
                item.description = match.description;
                add_item(plan, std::move(item));
            }
            if (options.import_forced_operands && !secondary_instruction.operand_names.empty()
                && primary_instruction.operand_names != secondary_instruction.operand_names) {
                for (const auto& [operand_index, operand_name] : parse_forced_operands(secondary_instruction.operand_names)) {
                    ImportPlanItem item;
                    item.operation = ImportOperation::set_forced_operand;
                    item.address = primary_instruction.address;
                    item.secondary_address = secondary_instruction.address;
                    item.current_name = primary_function.name;
                    item.imported_name = secondary_function.name;
                    item.value = operand_name;
                    item.numeric_value = static_cast<std::uint64_t>(operand_index);
                    item.ratio = match.ratio;
                    item.description = match.description;
                    add_item(plan, std::move(item));
                }
            }
        }
    }
    return plan;
}

} // namespace soff::ui
