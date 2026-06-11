target("jni_lib")
    set_kind("shared")

    add_files("src/*.cpp")

    if is_plat("windows") then
        add_defines("WIN32","_WIN32", "WINDOWS", "MORPHER_RT_PLANTFORM_WINDOWS")
    elseif is_plat("linux") then
        add_defines("LINUX", "MORPHER_RT_PLANTFORM_LINUX")
    end

    local jdk_path = os.getenv("JAVA_HOME")
    if not jdk_path then
        raise("JAVA_HOME environment variable is not set.")
    end

    add_includedirs(jdk_path .. "/include")
    if is_plat("windows") then
        add_includedirs(jdk_path .. "/include/win32")
    elseif is_plat("linux") then
        add_includedirs(jdk_path .. "/include/linux")
    end

    add_links("jvm")
    add_links("kernel32", "user32", "gdi32")
    add_linkdirs(jdk_path .. "/lib")

    set_targetdir("bin")
