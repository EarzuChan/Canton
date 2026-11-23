#include "RenderRegression.h"
#include "TextFixtures.h"

#include <d3d11/DWriteText.h>
#include <d3d11/ShaderCompiler.h>
#include <runtime/AppSession.h>
#include <runtime/DiagnosticsScene.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

// 从 .cta 夹具出发的用例：Vite 构建产物经 UI 包挂载、QuickJS、core 到离屏画面。
// 时间由用例给，不读真实时钟；分步操作经夹具里隐藏输入框的提交槽发指令
namespace render_regression {
    namespace {
        using namespace canton;

        class FixtureSession {
           public:
            FixtureSession(CaseContext& context, const std::string& fixture)
                : context_(context), fonts_(std::make_shared<core::FontRegistry>()), measurer_(fonts_.get()), app_(measurer_, fonts_, options(context)) {
                if (context.fixtures.empty()) throw std::runtime_error("夹具用例需要 --fixtures 指向构建好的夹具目录");
                app_.setWorkAvailable([this] { woken_.store(true, std::memory_order_release); });
                runtime::AppSourceConfig source;
                source.policy = runtime::LoadPolicy::OnlyDist;
                source.distOverride = context.fixtures / fixture;
                app_.configure(std::move(source));
                settle();
                if (!app_.ready()) {
                    const auto* error = app_.globalDiagnostics().error();
                    throw std::runtime_error("夹具没有成功出首帧：" + (error ? error->summary : std::string("未知原因")));
                }
            }

            // 推进到给定的虚拟时刻，按 16 毫秒一拍
            void advanceTo(double millis) {
                while (now_ + 16.0 <= millis) {
                    now_ += 16.0;
                    (void)app_.pumpFrame(now_);
                }
            }

            // 等后台资源加载时保持虚拟时刻不动
            void settle() {
                auto quietSince = std::chrono::steady_clock::now();
                const auto deadline = quietSince + std::chrono::seconds(10);
                while (std::chrono::steady_clock::now() < deadline) {
                    const auto changed = app_.pumpFrame(now_);
                    if (woken_.exchange(false, std::memory_order_acq_rel) || !changed.empty())
                        quietSince = std::chrono::steady_clock::now();
                    else if (std::chrono::steady_clock::now() - quietSince > std::chrono::milliseconds(120))
                        return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                throw std::runtime_error("夹具迟迟安定不下来");
            }

            void command(const std::string& text) {
                auto& win = windowAt(0);
                auto& tree = win.pipeline.scene().tree();
                for (const auto id : tree.nodeIds()) {
                    if (!test_support::event(tree.node(id), core::EventSlotKind::InputSubmit).valid()) continue;
                    if (!win.input.requestFocus(tree, id, win.text, {}) || !app_.charInput(win.windowId, text) || !app_.keyDown(win.windowId, {13, "Enter"})) {
                        throw std::runtime_error("夹具输入框未接收指令");
                    }
                    (void)app_.keyStateChanged(win.windowId, [](int) { return false; });
                    win.input.clearFocus(tree, win.text, {});
                    return;
                }
                throw std::runtime_error("夹具里找不到接收指令的输入框");
            }

            void resize(int width, int height) {
                app_.notifyWindowResized(windowAt(0).windowId, width, height);
            }

            [[nodiscard]] Image capture(std::size_t windowIndex = 0) {
                const auto& win = windowAt(windowIndex);
                const auto& frame = win.pipeline.publishedFrame();
                if (frame.revision == 0) throw std::runtime_error("目标窗口尚未发布首帧");
                auto target = context_;
                target.width = win.widthPx;
                target.height = win.heightPx;
                return target.render(frame.content);
            }

            void verifyGlassCursors() {
                const auto& win = windowAt(0);
                const auto [x, y, w, h] = glassBounds();
                using Icon = core::PointerIcon;
                const std::pair<core::Point, Icon> samples[]{
                    {{x + 5, y + 5}, Icon::ResizeNwse}, {{x + w - 5, y + 5}, Icon::ResizeNesw},
                    {{x + 5, y + h - 5}, Icon::ResizeNesw}, {{x + w - 5, y + h - 5}, Icon::ResizeNwse},
                    {{x + w / 2, y + 5}, Icon::ResizeVertical}, {{x + w / 2, y + h - 5}, Icon::ResizeVertical},
                    {{x + 5, y + h / 2}, Icon::ResizeHorizontal}, {{x + w - 5, y + h / 2}, Icon::ResizeHorizontal},
                    {{x + w / 2, y + h / 2}, Icon::Grab},
                };
                for (const auto& [point, expected] : samples) {
                    app_.pointerMove(win.windowId, {point, {}, now_});
                    if (app_.pointerIcon(win.windowId) != expected) throw std::runtime_error("玻璃光标与拖拽热区不符");
                }
            }

            void verifyLiquidSlider(const Image& rest) {
                const auto& win = windowAt(0);
                const auto& regions = win.pipeline.publishedFrame().content.hitTest->regions;
                const auto slider = std::find_if(regions.begin(), regions.end(), [](const auto& region) { return region.target.input == core::InputModifierKind::PointerInput; });
                if (slider == regions.end()) throw std::runtime_error("找不到玻璃滑杆热区");
                const auto [x, y, width, height] = slider->bounds;
                const core::Point center{x + width / 2, y + height / 2};
                const auto red = [&](const Image& image, core::Point point) {
                    return (image.pixels.at(static_cast<int>(point.y) * image.width + static_cast<int>(point.x)) >> 16) & 0xff;
                };
                app_.pointerDown(win.windowId, {center, {}, now_});
                advanceTo(now_ + 32);
                const auto transitioning = red(capture(), center);
                advanceTo(now_ + 224);
                const auto pressed = red(capture(), center);
                app_.pointerUp(win.windowId, {center, {}, now_});
                advanceTo(now_ + 640);
                const auto white = red(rest, center);
                if (!(white >= 250 && white > transitioning && transitioning > pressed && red(capture(), center) == white))
                    throw std::runtime_error("滑块应逐帧从白色转为玻璃，并在松手后恢复白色");

                const core::Point tap{x + width * 0.85f, center.y};
                app_.pointerDown(win.windowId, {tap, {}, now_});
                app_.pointerUp(win.windowId, {tap, {}, now_});
                advanceTo(now_ + 16);
                if (red(capture(), tap) >= 250) throw std::runtime_error("点按轨道后，滑块不可直接跳到目标位置");
                advanceTo(now_ + 640);
                if (red(capture(), tap) != white) throw std::runtime_error("点按轨道后，滑块应滑到目标位置并恢复白色");
            }

            void verifyGlassDragBounds() {
                const auto id = windowAt(0).windowId;
                const auto start = [&](core::Point point) { app_.pointerDown(id, {point, {}, now_}); };
                const auto move = [&](core::Point point) {
                    app_.pointerMove(id, {point, {}, now_});
                    advanceTo(now_ + 16);
                    return glassBounds();
                };

                const auto [x, y, w, h] = glassBounds();
                const core::Point center{x + w / 2, y + h / 2};
                start(center);
                const auto pinned = move({center.x - 320, center.y});
                if (pinned.x != 12 || move({center.x - 120, center.y}).x != pinned.x || move({center.x - 80, center.y}).x <= pinned.x)
                    throw std::runtime_error("玻璃移动到左限位后，应等鼠标回到抓取点再反向");
                app_.pointerUp(id, {{center.x - 80, center.y}, {}, now_});

                const auto bounds = glassBounds();
                const core::Point edge{bounds.x + 5, center.y};
                start(edge);
                const auto narrow = move({edge.x + 100, edge.y});
                const auto returning = move({edge.x + 60, edge.y});
                const auto wider = move({edge.x + 15, edge.y});
                if (narrow.width != 275 || returning.width != narrow.width || wider.width <= narrow.width || wider.x + wider.width != bounds.x + bounds.width)
                    throw std::runtime_error("玻璃缩到最小宽度后，应等鼠标回到左边再放宽，并固定右边");
                app_.pointerUp(id, {{edge.x + 15, edge.y}, {}, now_});
            }

            [[nodiscard]] bool hasError() const noexcept { return app_.globalDiagnostics().hasError(); }
            [[nodiscard]] double now() const noexcept { return now_; }

           private:
            [[nodiscard]] core::Rect glassBounds() {
                const auto& regions = windowAt(0).pipeline.publishedFrame().content.hitTest->regions;
                const auto drag = std::find_if(regions.begin(), regions.end(), [](const auto& region) { return region.target.input == core::InputModifierKind::Draggable; });
                if (drag == regions.end()) throw std::runtime_error("玻璃面板没有可拖动热区");
                return drag->bounds;
            }

            [[nodiscard]] runtime::WindowSurfaceState& windowAt(std::size_t index) {
                const auto ids = app_.windowIds();
                if (index >= ids.size()) throw std::runtime_error("目标窗口索引越界");
                return *app_.window(ids[index]);
            }

            runtime::AppSessionOptions options(const CaseContext& context) {
                runtime::AppSessionOptions result;
                result.diagnosticsFontFamily = {test_support::testFontFamily};
                result.shaderCompiler = std::make_shared<d3d11::D3DShaderCompiler>();
                if (context.width > 0 && context.height > 0) {
                    result.platform.onWindowCreated = [this, width = context.width, height = context.height](core::WindowId id, const core::WindowConfigSpec&) {
                        if (app_.windowIds().front() == id) app_.notifyWindowResized(id, width, height);
                    };
                }
                return result;
            }

            CaseContext& context_;
            std::shared_ptr<core::FontRegistry> fonts_;
            d3d11::DWriteTextMeasurer measurer_;
            std::atomic<bool> woken_{false};
            runtime::AppSession app_;
            double now_ = 0;
        };

        // 切到某一页，等它安定后截图
        Case page(std::string name, std::string instruction, int width = 560, int height = 400, double settleMillis = 64) {
            return {std::move(name), width, height, [instruction, settleMillis](CaseContext& context) {
                        FixtureSession session(context, "showcase");
                        session.command(instruction);
                        session.advanceTo(session.now() + settleMillis);
                        session.settle();
                        return session.capture();
                    }};
        }
    }

    namespace {
        // 诊断层：提示条、来源角标与帧率徽标。指令直接取自运行时的诊断场景，文字换成测试字体
        Image diagnosticsLayers(CaseContext& context) {
            const runtime::DiagnosticsScene scene;
            const core::Rect bounds{0, 0, static_cast<float>(context.width), static_cast<float>(context.height)};
            std::vector<DrawOp> ops;
            const auto append = [&](std::vector<DrawOp> part) {
                for (auto& op : part) {
                    if (op.type == core::DrawOpType::DrawText) {
                        op.textStyle.fontFamily = {test_support::testFontFamily};
                        op.textLayout.reset();
                    }
                    ops.push_back(std::move(op));
                }
            };
            append(scene.buildFpsBadge(bounds, 60, context.text));
            append(scene.buildBadge(bounds, {"LIVE", 0xff34d399u}, runtime::DiagnosticVisibility::Always, context.text));
            append(scene.buildToasts(bounds, {{core::LogLevel::Info, "Live App started", "http localhost 9178"}, {core::LogLevel::Warn, "Live unavailable", "fell back to Dist"}, {core::LogLevel::Error, "Hot update failed", ""}}, runtime::DiagnosticVisibility::Always, 1.0f, &context.text));
            return context.renderOps(std::move(ops));
        }
    }

    void addDiagnosticsCases(std::vector<Case>& cases) {
        cases.push_back({"diagnostics-layers", 420, 240, diagnosticsLayers});
    }

    void addFixtureCases(std::vector<Case>& cases) {
        cases.push_back({"render-windows", 0, 0, [](CaseContext& context) {
                             FixtureSession session(context, "render-windows");
                             const auto main = session.capture();
                             const auto second = session.capture(1);
                             if (main.width != 80 || main.height != 64 || main.pixels.at(0) != 0xff102030u ||
                                 second.width != 48 || second.height != 32 || second.pixels.at(0) != 0xff4070a0u) {
                                 throw std::runtime_error("离屏取图未命中指定窗口的视口或内容");
                             }
                             return second;
                         }, false});
        cases.push_back(page("showcase-layout", "layout"));
        cases.push_back(page("showcase-text", "text"));
        cases.push_back(page("showcase-image", "image"));
        cases.push_back(page("showcase-scroll", "scroll:180"));
        cases.push_back(page("showcase-lazy", "lazy:1234"));
        cases.push_back(page("showcase-canvas", "canvas"));
        // 视口变了要重新布局：同一页先按小尺寸出帧，再放大
        cases.push_back({"showcase-layout-resized", 560, 400, [](CaseContext& context) {
                             FixtureSession session(context, "showcase");
                             session.resize(760, 480);
                             session.advanceTo(session.now() + 64);
                             session.settle();
                             return session.capture();
                         }});
        // 动画在指定时刻取帧：起点、走到一半、终点
        const auto animation = [](std::string name, double elapsed) {
            return Case{std::move(name), 560, 400, [elapsed](CaseContext& context) {
                            FixtureSession session(context, "showcase");
                            session.command("animation:0");
                            session.advanceTo(session.now() + 64);
                            session.settle();
                            if (elapsed > 0) {
                                session.command("animation:1");
                                session.advanceTo(session.now() + elapsed);
                            }
                            return session.capture();
                        }};
        };
        cases.push_back(animation("showcase-animation-start", 0));
        cases.push_back(animation("showcase-animation-halfway", 224));
        cases.push_back(animation("showcase-animation-end", 640));
        // Lazy 项动画：删、插、挪之后在起点、一半和终点取帧
        const auto items = [](std::string name, double elapsed) {
            return Case{std::move(name), 560, 400, [elapsed](CaseContext& context) {
                            FixtureSession session(context, "showcase");
                            session.command("items:0");
                            session.advanceTo(session.now() + 64);
                            session.settle();
                            session.command("items:1");
                            session.advanceTo(session.now() + 16 + elapsed);
                            return session.capture();
                        }};
        };
        cases.push_back(items("showcase-items-start", 0));
        cases.push_back(items("showcase-items-halfway", 200));
        cases.push_back(items("showcase-items-end", 640));
        // 脚本抛错后进错误屏
        cases.push_back({"showcase-error-screen", 560, 400, [](CaseContext& context) {
                             FixtureSession session(context, "showcase");
                             session.command("crash");
                             session.advanceTo(session.now() + 64);
                             if (!session.hasError()) throw std::runtime_error("脚本抛错后没有进入错误屏");
                             return session.capture();
                         }});
        // 着色器与液态玻璃夹具（消费 @canton/kyant-glass、.hlsl 预编译 DXBC 与离屏合成）
        const auto shaderPage = [](std::string name, std::string instruction) {
            return Case{std::move(name), 640, 420, [instruction](CaseContext& context) {
                            FixtureSession session(context, "shader-effects");
                            session.command(instruction);
                            session.advanceTo(session.now() + 64);
                            session.settle();
                            auto image = session.capture();
                            if (instruction == "glass:1") {
                                session.verifyGlassCursors();
                                session.verifyLiquidSlider(image);
                                session.verifyGlassDragBounds();
                            }
                            return image;
                        }};
        };
        cases.push_back(shaderPage("shader-effects-glass", "glass:1"));
        cases.push_back(shaderPage("shader-effects-brushes", "brushes"));
        cases.push_back(shaderPage("shader-effects-compositing", "compositing"));
    }
}
