target("mj2x-crt")
    set_kind("static")
    set_group("mj2x/mj2x-crt")
    set_default(false)
    set_languages("c99")

    add_files("src/regc/*.c")
    add_headerfiles("src/regc/*.h")
    add_includedirs("src", {public = true})

    local jdk_path = os.getenv("JAVA_HOME")
    if jdk_path then
        add_includedirs(jdk_path .. "/include")
        if is_plat("windows") then
            add_includedirs(jdk_path .. "/include/win32")
        elseif is_plat("linux") then
            add_includedirs(jdk_path .. "/include/linux")
        elseif is_plat("macosx") then
            add_includedirs(jdk_path .. "/include/darwin")
        end
    end

    if is_plat("windows") then
        add_cflags("/TC", {tools = "cl"})
        add_cflags("/utf-8", {tools = "cl"})
    end
