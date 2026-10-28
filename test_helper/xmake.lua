target("test_helper")
    set_kind("static")
    set_group("libs/test")
    -- 通用机制委托 ca::test；libjdk_finder 为 luiox-repo 直连包：
    -- jdk_fixture.hpp 经本目录 include 的消费方依赖其传递可见性。
    -- test_helper.hpp include libca/test/test.hpp；public 供测试消费方传递。
    add_packages("libjdk_finder", "libca", "libjbytecode", "libjbc", {public = true})
    add_files("test_helper.cpp", "test_helper_dom_sync.cpp")
    add_includedirs(".", {public = true})
    -- 批 6d：test_helper 的 runWithDomSync/classFromTree/treeFromDom 依赖
    -- mj2x-core 的快照刷新与 CodeConverter（符号由消费方链接的 mj2x-core 提供）。
    add_includedirs("../mj2x-core/src", {public = true})
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end
