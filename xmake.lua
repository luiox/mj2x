-- mj2x - Morpher Java Bytecode to X
-- 聚合入口：各子模块（core/cli/runtime/unittest）拆到独立目录，对齐
-- libs/libjbytecode/<module>/ 的子库模式。每个子模块自带 xmake.lua。

-- 测试类 target 保 assert（R8 评审 N1 处方：测试 target 去 NDEBUG）。
--
-- 背景：release 模式下 xmake 内置 mode.release rule 会向每个 target 注入
-- -DNDEBUG——注意不是 defines 而是 cxflags（xmake rules/mode/xmake.lua 的
-- on_config 里 target:add("cxflags", "-DNDEBUG")，v3.0.9 实查），标准 C
-- assert 因此整体剥除：mj2x_rt_test 在 release 下 "all passed, exit 0"
-- 实际零向量验证（绿验空转，#1058 评审实锤）。产品 target 的 NDEBUG 必须保留
-- （release 优化语义不动），只有测试类 target 在这里摘掉它。
--
-- 为什么挂 on_config 而不是 remove_defines：NDEBUG 不在 defines 列表里，
-- remove_defines 无效；且 target:remove("cxflags", ...) 不是注册 API
-- （v3.0.9 的 target remove API 只覆盖 files 类，实测 raise unknown api）。
-- xmake 派发顺序为「各 rule on_config → target 自身 on_config → 各 rule
-- after_config」，故 target 级 on_config 晚于 mode.release 注入，在此
-- get + set 过滤即可摘掉刚注入的值（target:get 只取 target 自身值，不混入
-- dep/package 来源，set 不会产生重复/串源）。
-- 覆盖面：release/releasedbg/minsizerel/profile 四种模式 xmake 均注入 NDEBUG，
-- 一并移除（debug 等其余模式本来就不注入，过滤为无害空操作）。
function mj2x_test_keep_asserts()
    on_config(function (target)
        if is_mode("release", "releasedbg", "minsizerel", "profile") then
            for _, name in ipairs({"cxflags", "cflags", "cxxflags"}) do
                local values = table.wrap(target:get(name))
                if #values > 0 then
                    local filtered = {}
                    for _, value in ipairs(values) do
                        if value ~= "-DNDEBUG" then
                            table.insert(filtered, value)
                        end
                    end
                    target:set(name, table.unpack(filtered))
                end
            end
            -- 兜底：部分 xmake 版本走 defines 注入（值不带 -D 前缀）。
            local defines = table.wrap(target:get("defines"))
            if #defines > 0 then
                local filtered = {}
                for _, value in ipairs(defines) do
                    if value ~= "NDEBUG" then
                        table.insert(filtered, value)
                    end
                end
                target:set("defines", table.unpack(filtered))
            end
        end
    end)
end

includes("mj2x-core")
includes("mj2x-cli")
includes("mj2x-rt")
includes("unittest")
