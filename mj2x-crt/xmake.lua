target("mj2x-crt")
    set_kind("static")
    set_group("mj2x/mj2x-crt")
    set_default(false)
    set_languages("c99", "cxx17")

    add_files("src/**.c")
    if #os.files("src/**.cpp") > 0 then
        add_files("src/**.cpp")
    end
    add_headerfiles("src/**.h")
    add_includedirs("src", {public = true})

    local jdk_path = os.getenv("JAVA_HOME")
    if jdk_path then
        add_includedirs(jdk_path .. "/include", {public = true})
        if is_plat("windows") then
            add_includedirs(jdk_path .. "/include/win32", {public = true})
        elseif is_plat("linux") then
            add_includedirs(jdk_path .. "/include/linux", {public = true})
        elseif is_plat("macosx") then
            add_includedirs(jdk_path .. "/include/darwin", {public = true})
        end
    end

    if is_plat("windows") then
        add_cflags("/TC", {tools = "cl"})
        add_cflags("/utf-8", {tools = "cl"})
        add_cxxflags("/utf-8", {tools = "cl"})
    else
        add_syslinks("pthread")
    end

target("mj2x_crt_test")
    set_kind("binary")
    set_group("mj2x/mj2x-crt/test")
    set_default(false)
    set_languages("c99", "cxx17")

    add_deps("mj2x-crt")
    add_files("test/*.c")
    add_files("test/*.cpp")
    add_includedirs("src")
    set_rundir("$(projectdir)")

    if is_plat("windows") then
        add_cflags("/TC", {tools = "cl"})
        add_cflags("/utf-8", {tools = "cl"})
        add_cxxflags("/utf-8", {tools = "cl"})
    else
        add_syslinks("pthread")
    end
