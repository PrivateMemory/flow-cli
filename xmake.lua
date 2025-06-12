
add_rules("mode.debug", "mode.release")

local luau_dir = path.join("C:\\Users\\Kumusta\\Documents\\GitHub\\luau\\")

package("luau")

    set_homepage("https://luau-lang.org/")
    set_description("A fast, small, safe, gradually typed embeddable scripting language derived from Lua.")
    set_license("MIT")
    set_sourcedir(luau_dir)

    add_configs("extern_c", { description = "Use extern C for all APIs.", default = false, type = "boolean" })
    add_configs("build_web", { description = "Build web module.", default = false, type = "boolean" })

    add_deps("cmake")

    on_install("linux", "windows", "mingw|x86_64", "macosx", function(package)
        local configs = {}
        table.insert(configs, "-DCMAKE_BUILD_TYPE=" .. "Debug")
        table.insert(configs, "-DBUILD_SHARED_LIBS=" .. (package:config("shared") and "ON" or "OFF"))
        table.insert(configs, "-DLUAU_BUILD_TESTS=OFF")
        table.insert(configs, "-DLUAU_BUILD_WEB=" .. (package:config("build_web") and "ON" or "OFF"))
        table.insert(configs, "-DLUAU_EXTERN_C=" .. (package:config("extern_c") and "ON" or "OFF"))
        import("package.tools.cmake").install(package, configs, { buildir = "build" })
        -- need to link with Ast last
        local libs = { "Luau.VM", "Luau.Compiler", "Luau.Ast" }
        for _, link in ipairs(libs) do
            package:add("links", link)
        end
    end)
package_end()

add_requires("luau")
target("LuauExampleApp") --rename this to whatever you like
    set_kind("binary") -- exe
    set_languages("cxx20") -- cpp20
    add_files("src/**.cpp") -- add all cpp files in directories and subdirectories of src
    -- add useful luau include directories
    add_includedirs(luau_dir.."\\Compiler\\include\\", luau_dir.."\\Common\\include", luau_dir.."\\Ast\\include", luau_dir.."\\VM\\src", luau_dir.."\\VM\\include", luau_dir.."\\CodeGen\\include", luau_dir.."\\Flow\\include")
    add_packages("luau")
    set_symbols("debug")