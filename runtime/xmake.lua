target("mj2x-runtime-core")
    set_kind("static")
    set_group("mj2x/runtime")

    add_files("src/crypto/*.cpp")
    add_files("src/kernel/*.cpp")
    add_files("src/antidebug/*.cpp")
    add_files("src/antijvmti/*.cpp")
    add_files("src/regc/*.cpp")
    add_files("src/stringpool/*.cpp")
    add_files("src/intrinsic/*.cpp")
    add_includedirs("src", {public = true})

    if is_plat("windows") then
        add_defines("WIN32", "_WIN32", "WINDOWS", "MORPHER_RT_PLANTFORM_WINDOWS")
        add_syslinks("kernel32")
        add_cxflags("/utf-8", {tools = "cl"})
    elseif is_plat("linux") then
        add_defines("LINUX", "MORPHER_RT_PLANTFORM_LINUX")
    end

target("jni_lib")
    set_kind("shared")
    set_group("mj2x/runtime")
    set_default(false)

    add_deps("mj2x-runtime-core")
    add_files("src/runtime_entry.cpp")
    add_includedirs("src")

    local jdk_path = os.getenv("JAVA_HOME")
    if jdk_path then
        add_includedirs(jdk_path .. "/include")
        if is_plat("windows") then
            add_includedirs(jdk_path .. "/include/win32")
            add_linkdirs(jdk_path .. "/lib")
        elseif is_plat("linux") then
            add_includedirs(jdk_path .. "/include/linux")
            add_linkdirs(jdk_path .. "/lib/server")
        end
        add_links("jvm")
    end

    if is_plat("windows") then
        add_syslinks("kernel32", "user32", "gdi32")
        add_cxflags("/utf-8", {tools = "cl"})
    end

    set_targetdir("bin")

target("mj2x_runtime_test")
    set_kind("binary")
    set_group("mj2x/runtime/test")
    set_default(false)

    add_deps("mj2x-runtime-core")
    add_packages("gtest")
    add_files("test/main.cpp")
    add_files("test/*_test.cpp")
    add_includedirs("src")
    set_rundir("$(projectdir)")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
        add_defines("_CRT_SECURE_NO_WARNINGS")
    end
