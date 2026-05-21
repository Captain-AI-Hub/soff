set_project("soff")
set_version("0.1.0")
set_languages("cxx17")

add_requires("boost", {configs = {header_only = true}})

option("ida_sdk")
    set_default("ida-sdk-93-main/src")
    set_showmenu(true)
    set_description("Path to the IDA 9.3 SDK src directory")

option("ida_plugin")
    set_default(false)
    set_showmenu(true)
    set_description("Build the IDA plugin target")

target("soff_core")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_packages("boost", {public = true})
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

if has_config("ida_plugin") then
    local ida_sdk = path.join("$(projectdir)", get_config("ida_sdk"))

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
            add_linkdirs(path.join(ida_sdk, "lib", "x64_win_vc_64"))
            add_links("ida")
            add_syslinks("user32", "advapi32", "shell32", "crypt32")
        elseif is_plat("linux") then
            set_filename("soff.so")
            add_defines("__LINUX__")
            add_linkdirs(path.join(ida_sdk, "lib", "x64_linux_gcc_64"))
            add_links("ida")
        elseif is_plat("macosx") then
            set_filename("soff.dylib")
            add_defines("__MAC__")
            add_linkdirs(path.join(ida_sdk, "lib", "x64_mac_clang_64"))
            add_links("ida")
        end
end
