#include "RenderRegression.h"
#include "TextFixtures.h"

#include <d3d11/DWriteText.h>
#include <runtime/SceneHost.h>

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
            FixtureSession(CaseContext& context, const std::string& fixture) : context_(context), fonts_(std::make_shared<core::FontRegistry>()), measurer_(fonts_.get()), scene_(measurer_, fonts_, options()) {
                if (context.fixtures.empty()) throw std::runtime_error("夹具用例需要 --fixtures 指向构建好的夹具目录");
                scene_.setWorkAvailable([this] { woken_.store(true, std::memory_order_release); });
                scene_.resize(context.width, context.height);
                runtime::AppSourceConfig source;
                source.policy = runtime::LoadPolicy::OnlyDist;
                source.distOverride = context.fixtures / fixture;
                scene_.configure(std::move(source));
                settle();
                if (!scene_.ready()) {
                    const auto* error = scene_.diagnostics().error();
                    throw std::runtime_error("夹具没有成功出首帧：" + (error ? error->summary : std::string("未知原因")));
                }
            }

            // 推进到给定的虚拟时刻，按 16 毫秒一拍
            void advanceTo(double millis) {
                while (now_ + 16.0 <= millis) {
                    now_ += 16.0;
                    (void)scene_.pumpFrame(now_);
                }
            }

            // 资源在后台线程加载：停在当前虚拟时刻反复泵帧，直到一段真实时间内没有新工作
            void settle() {
                auto quietSince = std::chrono::steady_clock::now();
                const auto deadline = quietSince + std::chrono::seconds(10);
                while (std::chrono::steady_clock::now() < deadline) {
                    const auto revision = scene_.publishedFrame().revision;
                    (void)scene_.pumpFrame(now_);
                    if (woken_.exchange(false, std::memory_order_acq_rel) || scene_.publishedFrame().revision != revision)
                        quietSince = std::chrono::steady_clock::now();
                    else if (std::chrono::steady_clock::now() - quietSince > std::chrono::milliseconds(120))
                        return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                throw std::runtime_error("夹具迟迟安定不下来");
            }

            void command(const std::string& text) {
                const auto& tree = scene_.runtime().scene().tree();
                for (const auto id : tree.nodeIds()) {
                    const auto slot = test_support::event(tree.node(id), core::EventSlotKind::InputSubmit);
                    if (!slot.valid()) continue;
                    scene_.runtime().enqueueStringEvent(slot, text);
                    return;
                }
                throw std::runtime_error("夹具里找不到接收指令的输入框");
            }

            void resize(int width, int height) {
                context_.width = width;
                context_.height = height;
                scene_.resize(width, height);
            }

            [[nodiscard]] Image capture() {
                return context_.render(scene_.publishedFrame().content);
            }

            [[nodiscard]] runtime::SceneHost& scene() noexcept {
                return scene_;
            }

            [[nodiscard]] double now() const noexcept {
                return now_;
            }

           private:
            static runtime::SceneHostOptions options() {
                runtime::SceneHostOptions result;
                // 画面只留应用内容；错误屏用精简样式，不带因机器而异的路径
                result.diagnostics.badge = runtime::DiagnosticVisibility::Hidden;
                result.diagnostics.toasts = runtime::DiagnosticVisibility::Hidden;
                result.detailedErrorScreen = false;
                result.diagnosticsFontFamily = {test_support::testFontFamily};
                return result;
            }

            CaseContext& context_;
            std::shared_ptr<core::FontRegistry> fonts_;
            d3d11::DWriteTextMeasurer measurer_;
            runtime::SceneHost scene_;
            std::atomic<bool> woken_{false};
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
            append(scene.buildBadge(bounds, {"LIVE", 0xff22c55eu}, runtime::DiagnosticVisibility::Always, context.text));
            append(scene.buildToasts(bounds, {{core::LogLevel::Info, "Live App started", "http localhost 9178"}, {core::LogLevel::Warn, "Live unavailable", "fell back to Dist"}, {core::LogLevel::Error, "Hot update failed", ""}}, runtime::DiagnosticVisibility::Always));
            return context.renderOps(std::move(ops));
        }
    }

    void addDiagnosticsCases(std::vector<Case>& cases) {
        cases.push_back({"diagnostics-layers", 420, 240, diagnosticsLayers});
    }

    void addFixtureCases(std::vector<Case>& cases) {
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
                             if (!session.scene().diagnostics().hasError()) throw std::runtime_error("脚本抛错后没有进入错误屏");
                             return session.capture();
                         }});
    }
}
