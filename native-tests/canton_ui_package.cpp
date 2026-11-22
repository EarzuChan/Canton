// UI 包挂载：目录包与 ZIP 包、清单各条规则、读取预算与摘要、资产名解码、包内图片与字体的解码。
// 加载脚本出首帧由无头回归的 showcase 用例经 Dist 包覆盖
#include <core/Font.h>
#include <core/Version.h>
#include <quickjs/QuickJsBuild.h>
#include <runtime/Digest.h>
#include <runtime/PainterResources.h>
#include <runtime/UiPackage.h>
#include <runtime/ZipArchive.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    namespace fs = std::filesystem;
    using namespace canton;

    void check(bool condition, const std::string& message) {
        if (!condition) throw std::runtime_error(message);
    }

    std::string readFile(const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("无法读取 " + path.string());
        return {std::istreambuf_iterator<char>(input), {}};
    }

    void writeFile(const fs::path& path, std::string_view bytes) {
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output) throw std::runtime_error("无法写入 " + path.string());
    }

    std::string hex(std::string_view bytes) {
        return runtime::sha256Hex({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    }

    std::string jsonText(std::string_view text) {
        std::string result = "\"";
        for (const char c : text) {
            if (c == '"' || c == '\\') result += '\\';
            result += c;
        }
        return result + '"';
    }

    // 一个包的内容：条目名到字节；清单字段可以单独改写，用来造各种不合规的包
    struct PackageSpec {
        std::map<std::string, std::string> modules{{"app.js", "export default 1\n"}};
        std::map<std::string, std::string> assets;
        std::string format = "3";
        std::string framework = jsonText(core::PackageVersion);
        std::string protocol = std::to_string(core::RuntimeVersion);
        std::string scriptFormat = "\"js\"";
        std::string engine;
        std::string entry = "\"app.js\"";
        // 只登记不落盘：挂载只校验清单，用于文件系统上放不下的条目组合
        bool writeAssets = true;
        // 改写某个条目在清单里的一行
        std::function<std::string(const std::string& name, const std::string& row)> row;

        std::string manifest() const {
            std::string rows;
            const auto add = [&](const std::string& name, const std::string& bytes, const char* kind) {
                auto line = "{\"path\":" + jsonText(name) + ",\"kind\":\"" + kind + "\",\"size\":" + std::to_string(bytes.size()) + ",\"sha256\":\"" + hex(bytes) + "\"}";
                if (row) line = row(name, line);
                rows += (rows.empty() ? "" : ",") + line;
            };
            for (const auto& [name, bytes] : modules) add(name, bytes, "module");
            for (const auto& [name, bytes] : assets) add(name, bytes, "asset");
            return "{\"format\":" + format + ",\"framework\":" + framework + ",\"protocol\":" + protocol + ",\"scriptFormat\":" + scriptFormat + (engine.empty() ? "" : ",\"engine\":" + engine) + ",\"entry\":" + entry + ",\"entries\":[" + rows + "]}";
        }

        std::map<std::string, std::string> files() const {
            auto result = modules;
            if (writeAssets) result.insert(assets.begin(), assets.end());
            result[std::string(runtime::uiManifestName)] = manifest();
            return result;
        }
    };

    // 只写存储方式的 ZIP；deflate 的读取由下面嵌入的现成 ZIP 验
    std::string storedZip(const std::map<std::string, std::string>& files) {
        std::string body, directory;
        const auto u16 = [](std::string& out, std::uint16_t value) {
            out += static_cast<char>(value & 0xff), out += static_cast<char>(value >> 8);
        };
        const auto u32 = [&](std::string& out, std::uint32_t value) {
            u16(out, static_cast<std::uint16_t>(value & 0xffff)), u16(out, static_cast<std::uint16_t>(value >> 16));
        };
        for (const auto& [name, bytes] : files) {
            const auto offset = static_cast<std::uint32_t>(body.size());
            const auto crc = runtime::crc32({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
            const auto size = static_cast<std::uint32_t>(bytes.size());
            u32(body, 0x04034b50), u16(body, 20), u16(body, 0x800), u16(body, 0), u16(body, 0), u16(body, 0), u32(body, crc), u32(body, size), u32(body, size), u16(body, static_cast<std::uint16_t>(name.size())), u16(body, 0);
            body += name + bytes;
            u32(directory, 0x02014b50), u16(directory, 20), u16(directory, 20), u16(directory, 0x800), u16(directory, 0), u16(directory, 0), u16(directory, 0), u32(directory, crc), u32(directory, size), u32(directory, size);
            u16(directory, static_cast<std::uint16_t>(name.size())), u16(directory, 0), u16(directory, 0), u16(directory, 0), u16(directory, 0), u32(directory, 0), u32(directory, offset);
            directory += name;
        }
        std::string end;
        u32(end, 0x06054b50), u16(end, 0), u16(end, 0), u16(end, static_cast<std::uint16_t>(files.size())), u16(end, static_cast<std::uint16_t>(files.size())), u32(end, static_cast<std::uint32_t>(directory.size())), u32(end, static_cast<std::uint32_t>(body.size())), u16(end, 0);
        return body + directory + end;
    }

    // Python zipfile 生成：notes/deflated.txt 是 deflate 压缩的 "canton " × 40，stored.txt 是存储的 "plain"
    constexpr std::string_view deflatedZipHex =
        "504b03041400000008001985765dd87464350e00000018010000120000006e6f7465732f6465666c617465642e7478744b4ecc2bc9cf53481ea59251"
        "2800504b030414000000000000002100cf62201905000000050000000a00000073746f7265642e747874706c61696e504b0102140014000000080019"
        "85765dd87464350e000000180100001200000000000000000000008001000000006e6f7465732f6465666c617465642e747874504b01021400140000"
        "00000000002100cf62201905000000050000000a000000000000000000000080013e00000073746f7265642e747874504b0506000000000200020078"
        "0000006b0000000000";

    std::string unhex(std::string_view text) {
        std::string result;
        for (std::size_t i = 0; i + 1 < text.size(); i += 2) result += static_cast<char>(std::stoi(std::string(text.substr(i, 2)), nullptr, 16));
        return result;
    }

    class Workspace {
       public:
        Workspace() : root_(fs::temp_directory_path() / ("canton-ui-package-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            fs::create_directories(root_);
        }

        ~Workspace() {
            std::error_code error;
            fs::remove_all(root_, error);
        }

        fs::path directory(const std::string& name, const PackageSpec& spec) const {
            const auto path = root_ / name;
            for (const auto& [entry, bytes] : spec.files()) writeFile(path / fs::path(std::u8string(entry.begin(), entry.end())), bytes);
            return path;
        }

        fs::path zip(const std::string& name, const std::map<std::string, std::string>& files) const {
            const auto path = root_ / (name + ".zip");
            writeFile(path, storedZip(files));
            return path;
        }

        fs::path file(const std::string& name, std::string_view bytes) const {
            writeFile(root_ / name, bytes);
            return root_ / name;
        }

        [[nodiscard]] const fs::path& root() const noexcept {
            return root_;
        }

       private:
        fs::path root_;
    };

    // 挂载应当失败，且报错里带着给定的说法
    void rejects(const fs::path& path, std::string_view expected) {
        try {
            (void)runtime::mountUiPackage(path);
        } catch (const std::exception& error) {
            check(std::string_view(error.what()).find(expected) != std::string_view::npos, "报错说法不对：期望含“" + std::string(expected) + "”，实际“" + error.what() + "”");
            return;
        }
        throw std::runtime_error("不合规的包被接受了：期望“" + std::string(expected) + "”");
    }

    void manifestRules(const Workspace& space) {
        const auto variant = [&](const std::string& name, const std::function<void(PackageSpec&)>& change) {
            PackageSpec spec;
            change(spec);
            return space.directory(name, spec);
        };
        rejects(variant("format", [](auto& spec) { spec.format = "2"; }), "不支持的 UI 包格式");
        rejects(variant("framework", [](auto& spec) { spec.framework = "\"0.0.9\""; }), "框架版本");
        rejects(variant("protocol", [](auto& spec) { spec.protocol = "999"; }), "协议不匹配");
        rejects(variant("script-format", [](auto& spec) { spec.scriptFormat = "\"wasm\""; }), "脚本表示无效");
        rejects(variant("js-engine", [](auto& spec) { spec.engine = "\"x\""; }), "JS 包不能声明字节码引擎");
        rejects(variant("bytecode-engine",
                        [](auto& spec) {
                            spec.modules = {{"app.cbc", "bytes"}};
                            spec.scriptFormat = "\"bytecode\"";
                            spec.engine = "\"another-engine\"";
                            spec.entry = "\"app.cbc\"";
                        }),
                "构建身份不匹配");
        rejects(variant("bytecode-js-module",
                        [](auto& spec) {
                            spec.scriptFormat = "\"bytecode\"";
                            spec.engine = jsonText(quickjs::buildIdentity());
                        }),
                "模块名不符合脚本表示");
        rejects(variant("entry-fields",
                        [](auto& spec) {
                            spec.row = [](const auto&, const std::string& row) {
                                return row.substr(0, row.size() - 1) + ",\"extra\":1}";
                            };
                        }),
                "条目格式无效");
        rejects(variant("entry-hash",
                        [](auto& spec) {
                            spec.row = [](const auto&, const std::string& row) {
                                return row.substr(0, row.size() - 67) + "\"" + std::string(64, 'A') + "\"}";
                            };
                        }),
                "条目字段无效");
        rejects(variant("reserved-name", [](auto& spec) { spec.assets = {{"con.txt", "x"}}; }), "条目字段无效");
        rejects(variant("case-conflict", [](auto& spec) { spec.assets = {{"Logo.png", "a"}, {"logo.png", "b"}}; }), "条目路径冲突");
        rejects(variant("directory-conflict",
                        [](auto& spec) {
                            spec.assets = {{"images", "a"}, {"images/a.png", "b"}};
                            spec.writeAssets = false;
                        }),
                "条目目录冲突");
        rejects(variant("entry-missing", [](auto& spec) { spec.entry = "\"main.js\""; }), "入口不是已登记模块");
        rejects(variant("entry-asset",
                        [](auto& spec) {
                            spec.assets = {{"data.js", "1"}};
                            spec.entry = "\"data.js\"";
                        }),
                "入口不是已登记模块");
        (void)space.file("not-json/canton-ui.json", "{");
        rejects(space.root() / "not-json", "运行时清单无效");
        rejects(space.root() / "missing", "不存在普通目录或 ZIP 文件");
    }

    void readsAndBudgets(const Workspace& space) {
        PackageSpec spec;
        spec.assets = {{"中文 %.json", "{\"ok\":true}"}, {"images/a b.txt", "hello"}};
        const auto path = space.directory("good", spec);
        const auto package = runtime::mountUiPackage(path);
        check(package->entry() == "app.js" && package->scriptFormat() == core::UiScriptFormat::Js, "入口或脚本表示读错");
        check(package->read("app.js", core::UiEntryKind::Module, 64) == spec.modules.at("app.js"), "模块读取错误");
        check(package->read("中文 %.json", core::UiEntryKind::Asset, 64) == "{\"ok\":true}", "非 ASCII 资产名读取错误");
        const auto fails = [&](const std::function<void()>& action, std::string_view expected) {
            try {
                action();
            } catch (const std::exception& error) {
                check(std::string_view(error.what()).find(expected) != std::string_view::npos, std::string("报错说法不对：") + error.what());
                return;
            }
            throw std::runtime_error("应当失败的读取成功了：" + std::string(expected));
        };
        fails([&] { (void)package->read("app.js", core::UiEntryKind::Asset, 64); }, "找不到已登记资产");
        fails([&] { (void)package->read("images/a b.txt", core::UiEntryKind::Asset, 4); }, "超过读取预算");
        // 挂载后文件被改：读取时按清单核对
        writeFile(path / "images" / "a b.txt", "HELLO");
        fails([&] { (void)package->read("images/a b.txt", core::UiEntryKind::Asset, 64); }, "大小或摘要不匹配");
        check(runtime::packageAssetName("/images/a%20b.txt?v=1#top") == "images/a b.txt" && runtime::packageAssetName("images/a.png") == "images/a.png", "资产 URL 解码错误");
        fails([] { (void)runtime::packageAssetName("/images%2fa.png"); }, "资产 URL 编码无效");
        fails([] { (void)runtime::packageAssetName("/../a.png"); }, "资产 URL 路径无效");
    }

    void zipPackages(const Workspace& space) {
        PackageSpec spec;
        spec.assets = {{"images/x.txt", "zip asset"}};
        const auto package = runtime::mountUiPackage(space.zip("good", spec.files()));
        check(package->read("images/x.txt", core::UiEntryKind::Asset, 64) == "zip asset", "ZIP 包资产读取错误");
        auto extra = spec.files();
        extra["stray.txt"] = "not in manifest";
        rejects(space.zip("stray", extra), "ZIP 有未登记的条目");
        auto missing = spec.files();
        missing.erase("images/x.txt");
        rejects(space.zip("missing", missing), "ZIP 索引与清单不符");
        auto unportable = spec.files();
        unportable["a:b.txt"] = "x";
        rejects(space.zip("unportable", unportable), "ZIP 条目名无效");
        rejects(space.file("broken.zip", "PK not really a zip"), "ZIP");
        // 目录包与 ZIP 包同名并存由宿主的随包位置检查拦，见 verify:host
        const runtime::ZipArchive archive(space.file("deflated.zip", unhex(deflatedZipHex)), 16, 1024);
        std::string expected;
        for (int i = 0; i < 40; ++i) expected += "canton ";
        check(archive.read("notes/deflated.txt", 1024) == expected && archive.read("stored.txt", 1024) == "plain", "deflate 或存储条目解压错误");
        bool limited = false;
        try {
            (void)archive.read("notes/deflated.txt", 16);
        } catch (const std::exception&) {
            limited = true;
        }
        check(limited, "ZIP 条目超过读取预算没有报错");
    }

    void packageResources(const Workspace& space, const fs::path& sources) {
        PackageSpec spec;
        spec.assets = {{"images/photo.png", readFile(sources / "tests/fixtures/showcase/public/images/photo.png")}, {"images/logo.svg", readFile(sources / "tests/fixtures/showcase/public/images/logo.svg")}, {"fonts/regular.ttf", readFile(sources / "tests/fixtures/fonts/CantonTestSans-Regular.ttf")}, {"data.json", "[1,2]"}};
        for (const auto& path : {space.directory("resources", spec), space.zip("resources", spec.files())}) {
            const auto package = runtime::mountUiPackage(path);
            const auto painter = runtime::packagePainterLoader(package);
            const auto load = [&](const std::string& name) {
                return painter(name, [] {}).get();
            };
            const auto bitmap = load("/images/photo.png");
            check(std::dynamic_pointer_cast<const core::BitmapContent>(bitmap.content) != nullptr, "包内 PNG 没有解成位图：" + bitmap.error);
            check(std::dynamic_pointer_cast<const core::VectorContent>(load("images/logo.svg").content) != nullptr, "包内 SVG 没有解成矢量内容");
            const auto font = std::dynamic_pointer_cast<const core::FontContent>(load("/fonts/regular.ttf").content);
            check(font && font->info.weight == 400 && !font->info.italic, "包内字体没有解出字面信息");
            const auto missing = load("/images/none.png");
            check(!missing.content && missing.error.find("images/none.png") != std::string::npos, "缺失资源的诊断没有指明条目：" + missing.error);
            check(runtime::packageAssetLoader(package)("data.json", [] {}).get() == "[1,2]", "资产加载器读取错误");
        }
    }
}

int main() {
    const char* step = "清单规则";
    try {
        const Workspace space;
        manifestRules(space);
        step = "读取与预算";
        readsAndBudgets(space);
        step = "ZIP 包";
        zipPackages(space);
        step = "包内资源";
        packageResources(space, CANTON_SOURCE_DIR);
        std::cout << "UI 包挂载、清单规则、ZIP 与包内资源通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << step << "：" << error.what() << '\n';
        return 1;
    }
}
