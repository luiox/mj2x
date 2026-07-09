-- mj2x-cli 子模块：命令行入口
-- include 根 = src；mj2x/ 头通过 add_deps("mj2x-core") 透传的 public includedir 解析

rule("mj2x.embed-resource")
    set_extensions(".mj2xres")
    on_load(function (target)
        local headerdir = path.join(target:autogendir(), "rules", "mj2x", "embed-resource")
        if not os.isdir(headerdir) then
            os.mkdir(headerdir)
        end
        target:add("includedirs", headerdir)
    end)
    on_preparecmd_file(function (target, batchcmds, sourcefile, opt)
        import("rules.utils.bin2c.utils", {alias = "bin2c_utils", rootdir = os.programdir()})
        local fileconfig = target:fileconfig(sourcefile)
        local resource = fileconfig and fileconfig.resource
        local header = fileconfig and fileconfig.header
        assert(resource and header, "mj2x.embed-resource requires resource/header fileconfig")
        if not path.is_absolute(resource) then
            resource = path.join(os.scriptdir(), resource)
        end
        local headerdir = path.join(target:autogendir(), "rules", "mj2x", "embed-resource")
        local headerfile = path.join(headerdir, header)
        bin2c_utils.generate_headerfile(target, batchcmds, resource, {
            progress = opt.progress,
            headerdir = headerdir,
            headerfile = headerfile,
            nozeroend = true,
        })
        batchcmds:add_depfiles(sourcefile, resource)
        batchcmds:set_depmtime(os.mtime(resource))
        batchcmds:set_depcache(target:dependfile(headerfile))
    end)

target("mj2x-cli")
    set_kind("binary")
    set_group("mj2x")

    add_deps("mj2x-core")

    add_files("src/*.cpp")
    add_files("resources/mj2x_regc_crt_h.mj2xres", {
        rules = "mj2x.embed-resource",
        resource = "../mj2x-crt/src/regc/mj2x_regc_crt.h",
        header = "mj2x_regc_crt.h.h",
    })
    add_files("resources/mj2x_regc_crt_c.mj2xres", {
        rules = "mj2x.embed-resource",
        resource = "../mj2x-crt/src/regc/mj2x_regc_crt.c",
        header = "mj2x_regc_crt.c.h",
    })
    add_files("resources/xmake_lua_in.mj2xres", {
        rules = "mj2x.embed-resource",
        resource = "../mj2x-crt/templates/xmake.lua.in",
        header = "xmake.lua.in.h",
    })
    add_files("resources/readme_md_in.mj2xres", {
        rules = "mj2x.embed-resource",
        resource = "../mj2x-crt/templates/README.md.in",
        header = "README.md.in.h",
    })
    add_includedirs("src")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end
