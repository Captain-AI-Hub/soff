set_project("soff")
set_version("0.3.1")
set_languages("cxx17")

add_requires("boost", {configs = {header_only = true}})

option("ida_sdk")
    set_default("ida-sdk-94-main/src")
    set_showmenu(true)
    set_description("Path to the IDA SDK src directory (9.3 or 9.4)")

option("ida_plugin")
    set_default(false)
    set_showmenu(true)
    set_description("Build the IDA plugin target")

target("soff_core")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_packages("boost", {public = true})
    if is_plat("linux", "macosx") then
        add_cxflags("-fPIC")
    end
    add_files(
        "src/core/*.cpp",
        "src/analysis/*.cpp",
        "src/db/*.cpp",
        "src/diff/*.cpp",
        "src/ui/*.cpp"
    )

target("soff_cli")
    set_kind("binary")
    add_deps("soff_core")
    add_files("src/cli/*.cpp")

target("soff_smoke")
    set_kind("binary")
    add_deps("soff_core")
    add_files("tests/*.cpp")

target("soff_ffi")
    set_kind("shared")
    add_deps("soff_core")
    add_files("src/ffi/*.cpp")
    if is_plat("windows") then
        set_filename("soff_ffi.dll")
    elseif is_plat("linux") then
        set_filename("libsoff_ffi.so")
        add_cxflags("-fPIC")
    elseif is_plat("macosx") then
        set_filename("libsoff_ffi.dylib")
    end

if has_config("ida_plugin") then
    local ida_sdk = path.join("$(projectdir)", get_config("ida_sdk"))

    -- SDK 9.4 renamed the prebuilt library directories (e.g. x64_linux_gcc_64
    -- became x64_linux_64); pick whichever exists in the selected SDK.
    local function ida_libdir(...)
        local names = {...}
        for _, name in ipairs(names) do
            local dir = path.join(ida_sdk, "lib", name)
            if os.isdir(dir) then
                return dir
            end
        end
        return path.join(ida_sdk, "lib", names[1])
    end

    target("soff_ida")
        set_kind("shared")
        add_deps("soff_core")
        add_files("src/plugin/*.cpp")
        add_includedirs("include", {public = true})
        add_includedirs(path.join(ida_sdk, "include"), {public = true})
        add_defines("__EA64__", "USE_DANGEROUS_FUNCTIONS")

        if is_plat("windows") then
            set_filename("soff.dll")
            add_defines("__NT__")
            add_linkdirs(ida_libdir("x64_win_64", "x64_win_vc_64"))
            add_links("ida")
            add_syslinks("user32", "advapi32", "shell32")
        elseif is_plat("linux") then
            set_filename("soff.so")
            add_defines("__LINUX__")
            add_linkdirs(ida_libdir("x64_linux_64", "x64_linux_gcc_64"))
            add_links("ida")
        elseif is_plat("macosx") then
            set_filename("soff.dylib")
            add_defines("__MAC__")
            add_cxflags("-Wno-nullability-completeness", "-Wno-nullability-extension", {force = true})
            if is_arch("arm64") then
                add_linkdirs(ida_libdir("arm64_mac_64", "arm64_mac_clang_64"))
            else
                add_linkdirs(ida_libdir("x64_mac_64", "x64_mac_clang_64"))
            end
            add_links("ida")
        end
end
