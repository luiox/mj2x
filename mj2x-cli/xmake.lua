-- mj2x-cli 子模块：命令行入口
-- include 根 = src；mj2x/ 头通过 add_deps("mj2x-core") 透传的 public includedir 解析

rule("mj2x.embed-crt-resources")
    set_extensions(".mj2xres")
    add_orders("mj2x.embed-crt-resources", "c++.build.modules.builder")
    on_preparecmd_file(function (target, batchcmds, sourcefile, opt)
        local headerdir = path.join(target:autogendir(), "rules", "mj2x", "embed-resource")
        if not os.isdir(headerdir) then
            os.mkdir(headerdir)
        end
        target:add("includedirs", headerdir)

        local crtroot = path.join(os.scriptdir(), "../mj2x-crt")
        local manifest = path.join(crtroot, "resources.manifest")
        local manifestdata = assert(io.readfile(manifest), "cannot read " .. manifest)
        local resources = {}
        for line in manifestdata:gmatch("[^\r\n]+") do
            line = line:match("^%s*(.-)%s*$")
            if #line > 0 and line:sub(1, 1) ~= "#" then
                assert(line:match("^[%w%._%-%/]+$") and not line:find("../", 1, true),
                       "invalid mj2x-crt resource path: " .. line)
                table.insert(resources, line)
            end
        end
        assert(#resources > 0, "mj2x-crt resource manifest is empty")

        local generated = {
            "// Generated from mj2x-crt/resources.manifest. Do not edit.",
            "",
        }
        for index, resource in ipairs(resources) do
            local data = assert(io.readfile(path.join(crtroot, resource), {encoding = "binary"}),
                                "cannot read mj2x-crt resource: " .. resource)
            table.insert(generated, "constexpr unsigned char kCrtResource" .. index .. "[] = {")
            local values = {}
            for byteindex = 1, #data do
                table.insert(values, string.format("0x%02X", data:byte(byteindex)))
            end
            if #values > 0 then
                for first = 1, #values, 12 do
                    local last = math.min(first + 11, #values)
                    table.insert(generated, "    " .. table.concat(values, ", ", first, last) .. ",")
                end
            end
            table.insert(generated, "};")
            table.insert(generated, "")
        end
        table.insert(generated, "constexpr EmbeddedResource kResources[] = {")
        for index, resource in ipairs(resources) do
            table.insert(generated,
                         "    {\"" .. resource .. "\", kCrtResource" .. index ..
                         ", sizeof(kCrtResource" .. index .. ")},")
        end
        table.insert(generated, "};")
        table.insert(generated, "")
        table.insert(generated,
                     "constexpr std::size_t kResourceCount = sizeof(kResources) / sizeof(kResources[0]);")
        table.insert(generated, "")

        local headerfile = path.join(headerdir, "mj2x_crt_resources.generated.hpp")
        local content = table.concat(generated, "\n")
        local existing = os.isfile(headerfile) and io.readfile(headerfile) or nil
        if existing ~= content then
            io.writefile(headerfile, content)
        end
        batchcmds:add_depfiles(sourcefile, manifest)
        for _, resource in ipairs(resources) do
            batchcmds:add_depfiles(path.join(crtroot, resource))
        end
        batchcmds:set_depmtime(os.mtime(headerfile))
        batchcmds:set_depcache(target:dependfile(headerfile))
    end)

target("mj2x-cli")
    set_kind("binary")
    set_group("mj2x")

    add_deps("mj2x-core", "libjdk_finder", "libca_process")
    add_packages("nlohmann_json")

    add_files("src/*.cpp")
    add_files("resources/mj2x_crt_sources.mj2xres", {rules = "mj2x.embed-crt-resources"})
    add_includedirs("src")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end
