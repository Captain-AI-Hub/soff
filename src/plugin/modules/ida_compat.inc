// Compatibility shims between IDA SDK 9.3 and 9.4.
//
// SDK 9.4 deprecates the func_t*-based accessors (get_func, getn_func,
// get_next_func, func_item_iterator_t, qflow_chart_t, decompile_func,
// get_func_cmt, set_func_cmt, update_func, get_sourcefile, gen_microcode)
// in favor of ea-based APIs. All plugin call sites go through these helpers
// so both SDKs build warning-free.

#if defined(IDA_SDK_VERSION) && IDA_SDK_VERSION >= 940
#define SOFF_IDA_SDK_94 1
#else
#define SOFF_IDA_SDK_94 0
#endif

// Everything the exporter needs from a function, without holding a func_t*.
struct FuncInfo
{
    ea_t start_ea = BADADDR;
    ea_t end_ea = BADADDR;
    uint64_t flags = 0;

    asize_t size() const { return end_ea >= start_ea ? end_ea - start_ea : 0; }
};

// Entry-chunk descriptor for the function containing ea.
bool soff_get_func_info(ea_t ea, FuncInfo& out)
{
#if SOFF_IDA_SDK_94
    func_entry_info_t info;
    if (!get_func_entry_info(&info, ea)) {
        return false;
    }
    out.start_ea = info.start_ea;
    out.end_ea = info.end_ea;
    out.flags = info.get_flags();
    return true;
#else
    func_t* function = get_func(ea);
    if (function == nullptr) {
        return false;
    }
    out.start_ea = function->start_ea;
    out.end_ea = function->end_ea;
    out.flags = function->flags;
    return true;
#endif
}

// Start address of the function containing ea, or BADADDR.
ea_t soff_get_func_start(ea_t ea)
{
#if SOFF_IDA_SDK_94
    return get_func_start(ea);
#else
    func_t* function = get_func(ea);
    return function != nullptr ? function->start_ea : BADADDR;
#endif
}

ea_t soff_func_ea_by_num(std::size_t n)
{
#if SOFF_IDA_SDK_94
    return get_func_ea_by_num(n);
#else
    func_t* function = getn_func(n);
    return function != nullptr ? function->start_ea : BADADDR;
#endif
}

ea_t soff_next_func_ea(ea_t ea)
{
#if SOFF_IDA_SDK_94
    return get_next_func_ea(ea);
#else
    func_t* function = get_next_func(ea);
    return function != nullptr ? function->start_ea : BADADDR;
#endif
}

ssize_t soff_get_func_cmt(qstring* out, ea_t func_ea, bool repeatable)
{
#if SOFF_IDA_SDK_94
    return get_func_cmt_ea(out, func_ea, repeatable);
#else
    func_t* function = get_func(func_ea);
    return function != nullptr ? get_func_cmt(out, function, repeatable) : -1;
#endif
}

bool soff_set_func_cmt(ea_t func_ea, const char* comment, bool repeatable)
{
#if SOFF_IDA_SDK_94
    return set_func_cmt_ea(func_ea, comment, repeatable);
#else
    func_t* function = get_func(func_ea);
    return function != nullptr && set_func_cmt(function, comment, repeatable);
#endif
}

// update_func() replacement; only ever used to persist a flags change.
bool soff_update_func_flags(ea_t func_ea, uint64_t flags)
{
#if SOFF_IDA_SDK_94
    func_entry_info_t info;
    if (!get_func_entry_info(&info, func_ea)) {
        return false;
    }
    info.set_flags(flags);
    return set_func_entry_info(&info);
#else
    func_t* function = get_func(func_ea);
    if (function == nullptr) {
        return false;
    }
    function->flags = static_cast<decltype(function->flags)>(flags);
    return update_func(function);
#endif
}

std::string soff_get_sourcefile(ea_t ea)
{
#if SOFF_IDA_SDK_94
    qstring out;
    if (!get_sourcefile_by_ea(&out, ea)) {
        return "";
    }
    return to_string(out);
#else
    const char* source = get_sourcefile(ea);
    return source != nullptr ? source : "";
#endif
}

cfuncptr_t soff_decompile(ea_t func_ea, hexrays_failure_t* failure, int flags)
{
#if SOFF_IDA_SDK_94
    return decompile_function(func_ea, failure, flags);
#else
    func_t* function = get_func(func_ea);
    if (function == nullptr) {
        return cfuncptr_t(nullptr);
    }
    return decompile_func(function, failure, flags);
#endif
}

mba_t* soff_gen_microcode(ea_t func_ea, hexrays_failure_t* failure)
{
#if SOFF_IDA_SDK_94
    decomp_ranges_t ranges(func_ea);
    return gen_microcode(ranges, failure, nullptr, DECOMP_WARNINGS, MMAT_GENERATED);
#else
    func_t* function = get_func(func_ea);
    if (function == nullptr) {
        return nullptr;
    }
    mba_ranges_t ranges(function);
    return gen_microcode(ranges, failure, nullptr, DECOMP_WARNINGS, MMAT_GENERATED);
#endif
}

#if SOFF_IDA_SDK_94
using SoffFlowChart = qflow_chart_ea_t;
using SoffItemIterator = function_item_iterator_t;
#else
using SoffFlowChart = qflow_chart_t;
using SoffItemIterator = func_item_iterator_t;
#endif

SoffFlowChart soff_make_flow_chart(const FuncInfo& function)
{
#if SOFF_IDA_SDK_94
    return SoffFlowChart("", function.start_ea, function.start_ea, function.end_ea, 0);
#else
    return SoffFlowChart("", get_func(function.start_ea), function.start_ea, function.end_ea, 0);
#endif
}

SoffItemIterator soff_make_item_iterator(const FuncInfo& function)
{
#if SOFF_IDA_SDK_94
    return SoffItemIterator(function.start_ea);
#else
    return SoffItemIterator(get_func(function.start_ea));
#endif
}

// hook_to_notification_point has no replacement as of SDK 9.4 (IDAPython's own
// binding still forwards to it); suppress the deprecation warning locally
// instead of pretending a successor API exists.
bool soff_hook_ui_notifications(hook_cb_t* callback, void* user_data)
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    const bool hooked = hook_to_notification_point(HT_UI, callback, user_data);
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    return hooked;
}

void soff_unhook_ui_notifications(hook_cb_t* callback, void* user_data)
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    unhook_from_notification_point(HT_UI, callback, user_data);
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
}
