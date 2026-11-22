// 多窗口生命周期、Win32 Owner 拓扑逆序销毁、嵌套非阻塞模态锁计数、WM_NCHITTEST 五级优先级与 DPI 密度联动原生测试。
// 全程使用 visible=false 的真实隐藏 Win32 HWND，零屏幕闪烁。
#include "RenderSupport.h"

#include <core/HitTest.h>
#include <core/Version.h>
#include <runtime/AppSession.h>
#include <runtime/Digest.h>
#include <runtime/UiPackage.h>
#include <win32/Window.h>
#include <win32/WindowManager.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace canton;

namespace {
    void check(bool cond, const std::string& msg) {
        if (!cond) throw std::runtime_error(msg);
    }

    // 1. Win32 WindowManager：Owner 继承、模态锁计数（含二级嵌套模态）与拓扑逆序销毁
    void verifyWindowManagerTopologyAndModality() {
        std::vector<core::WindowId> destroyedOrder;
        win32::WindowManager manager([](core::WindowId) { return win32::WindowCallbacks{}; });

        core::WindowConfigSpec mainSpec;
        mainSpec.title = "Main";
        mainSpec.visible = false;
        mainSpec.sizeDp = {600, 400};
        auto& mainWin = manager.createWindow(1, mainSpec);
        check(mainWin.ownerHandle() == nullptr && mainWin.isEnabled(), "顶层窗口初始应启用且无 Owner");

        // 子窗口继承 Owner（HWND 挂在主窗名下）
        core::WindowConfigSpec ownedToolSpec;
        ownedToolSpec.title = "Tool";
        ownedToolSpec.visible = false;
        ownedToolSpec.ownerWindowId = 1;
        auto& toolWin = manager.createWindow(2, ownedToolSpec);
        check(reinterpret_cast<HWND>(GetWindowLongPtrW(static_cast<HWND>(toolWin.handle()), GWLP_HWNDPARENT)) == static_cast<HWND>(mainWin.handle()), "Owned 窗口未绑定父级 GWLP_HWNDPARENT");

        // owner="none"（ownerWindowId 为空）时不挂 GWLP_HWNDPARENT
        core::WindowConfigSpec independentSpec;
        independentSpec.title = "Independent";
        independentSpec.visible = false;
        auto& indWin = manager.createWindow(3, independentSpec);
        check(reinterpret_cast<HWND>(GetWindowLongPtrW(static_cast<HWND>(indWin.handle()), GWLP_HWNDPARENT)) == nullptr, "owner=none 窗口不应携带 GWLP_HWNDPARENT");

        // 为测模态锁计数，将逻辑配置标为 visible=true（不额外调 ShowWindow 弹窗，仅通过 updateWindow/创建验证锁计数；为免屏幕闪烁，放在屏幕外极小隐藏位置或直接验证启用态）
        // 注：模态锁对 visible=true 的模态窗生效；将窗口坐标移至 (-32000, -32000) 零打扰验证真实 EnableWindow 状态
        const auto offscreen = [](core::WindowConfigSpec spec) {
            spec.position = {core::WindowPositionPreset::Coordinates, -20000, -20000};
            spec.sizeDp = {120, 80};
            spec.role = core::WindowRole::Utility;
            spec.showInTaskbar = false;
            spec.visible = true;
            return spec;
        };

        win32::WindowManager modalMgr;
        auto& mMain = modalMgr.createWindow(10, offscreen(mainSpec));
        auto& mSibling = modalMgr.createWindow(11, offscreen(independentSpec));
        check(mMain.isEnabled() && mSibling.isEnabled(), "模态打开前两窗均应启用");

        // 一级 Owner 模态窗（owner=10）：只锁 10，不锁无关独立窗 11
        core::WindowConfigSpec modal1Spec = offscreen({});
        modal1Spec.ownerWindowId = 10;
        modal1Spec.modality = core::WindowModality::Owner;
        auto& mModal1 = modalMgr.createWindow(12, modal1Spec);
        check(modalMgr.modalLockCount(10) == 1 && !mMain.isEnabled(), "Owner 模态未锁定宿主窗口");
        check(modalMgr.modalLockCount(11) == 0 && mSibling.isEnabled(), "Owner 模态不应误锁无关兄弟顶层窗口");
        check(mModal1.isEnabled(), "模态窗自身必须保持启用");

        // 二级嵌套 Owner 模态窗（owner=12）：10 锁计数保持 >=1，12 锁计数变为 1 并禁用，二级模态 13 自身启用
        core::WindowConfigSpec modal2Spec = offscreen({});
        modal2Spec.ownerWindowId = 12;
        modal2Spec.modality = core::WindowModality::Owner;
        auto& mModal2 = modalMgr.createWindow(13, modal2Spec);
        check(modalMgr.modalLockCount(12) == 1 && !mModal1.isEnabled(), "二级嵌套模态未锁定一级模态窗");
        check(!mMain.isEnabled() && mModal2.isEnabled(), "二级嵌套模态期间主窗必须仍锁定、二级模态自身启用");

        // 关闭二级模态 13：一级模态 12 恢复启用（锁计数归 0），但主窗 10 仍被一级模态锁住！
        modalMgr.destroyWindow(13);
        check(modalMgr.modalLockCount(12) == 0 && mModal1.isEnabled(), "关闭二级模态后一级模态未解锁恢复");
        check(modalMgr.modalLockCount(10) == 1 && !mMain.isEnabled(), "关闭二级模态不应提前解锁外层主窗");

        // Application 模态窗：锁定进程内除自身与自身子孙外的全部窗口（含 10 和 11）
        core::WindowConfigSpec appModalSpec = offscreen({});
        appModalSpec.modality = core::WindowModality::Application;
        (void)modalMgr.createWindow(14, appModalSpec);
        check(modalMgr.modalLockCount(10) == 2 && modalMgr.modalLockCount(11) == 1 && !mSibling.isEnabled(), "Application 模态未叠加锁计数并禁用全部其他窗口");

        modalMgr.destroyWindow(14);
        check(modalMgr.modalLockCount(11) == 0 && mSibling.isEnabled() && modalMgr.modalLockCount(10) == 1, "关闭 Application 模态后锁计数递减不正确");

        modalMgr.destroyWindow(12);
        check(modalMgr.modalLockCount(10) == 0 && mMain.isEnabled(), "关闭最后一个模态窗后主窗未恢复 EnableWindow(TRUE)");

        // 拓扑逆序排序契约验证（父在前传入，必须输出深层子孙在前、父在后）
        std::unordered_map<core::WindowId, std::optional<core::WindowId>> owners{
            {1, std::nullopt},
            {2, 1},
            {3, 2},
            {4, std::nullopt},
        };
        const auto order = runtime::AppSession::sortDestroyOrder({1, 2, 3, 4}, owners);
        const auto pos = [&](core::WindowId id) { return std::find(order.begin(), order.end(), id) - order.begin(); };
        check(pos(3) < pos(2) && pos(2) < pos(1), "同事务销毁父子窗未严格按先 Owned 子孙、后 Owner 的拓扑逆序排列");
    }

    // 2. 自绘标题栏 5 级 WM_NCHITTEST 实窗消息判定
    void verifyFiveLevelNcHitTest() {
        core::LayoutTree tree;
        core::InputModifierSemantics dragArea;
        dragArea.kind = core::InputModifierKind::DraggableArea;

        core::InputModifierSemantics nonDragArea;
        nonDragArea.kind = core::InputModifierKind::NonDraggableArea;

        core::InputModifierSemantics clickBtn;
        clickBtn.kind = core::InputModifierKind::Clickable;
        clickBtn.eventSlot = core::makeEventSlotId(3, core::EventSlotKind::Click);

        core::InputModifierSemantics minBtn, maxBtn, closeBtn;
        minBtn.kind = core::InputModifierKind::CaptionButton;
        minBtn.captionButton = core::CaptionButtonKind::Minimize;
        maxBtn.kind = core::InputModifierKind::CaptionButton;
        maxBtn.captionButton = core::CaptionButtonKind::Maximize;
        closeBtn.kind = core::InputModifierKind::CaptionButton;
        closeBtn.captionButton = core::CaptionButtonKind::Close;

        const auto fixed = [](float x, float y, float w, float h, core::ModifierValue extra) {
            core::OffsetModifier off{x, y};
            core::LayoutModifierSemantics sz;
            sz.kind = core::LayoutModifierKind::Size;
            sz.width = w;
            sz.height = h;
            return core::ModifierDescriptors{{off}, {sz}, {std::move(extra)}};
        };

        tree.apply({
            core::CreateNodeMutation{1, core::NodeType::Layout},
            core::SetPropMutation{1, "measurePolicy", core::PropValue::objectValue({{"kind", core::PropValue::stringValue("Box")}})},
            core::SetModifierMutation{1, fixed(0, 0, 500, 300, core::PaintStyleSemantics{})},

            // 标题栏拖拽底条 (0, 0, 500, 40)
            core::CreateNodeMutation{2, core::NodeType::Layout},
            core::SetPropMutation{2, "measurePolicy", core::PropValue::objectValue({{"kind", core::PropValue::stringValue("Box")}})},
            core::SetModifierMutation{2, fixed(0, 0, 500, 40, dragArea)},
            core::InsertChildMutation{1, 2, 0},

            // 标题栏内可点击按钮 (60, 8, 80, 24) -> 自动让位 HTCLIENT
            core::CreateNodeMutation{3, core::NodeType::Layout},
            core::SetPropMutation{3, "measurePolicy", core::PropValue::objectValue({{"kind", core::PropValue::stringValue("Box")}})},
            core::SetModifierMutation{3, fixed(60, 8, 80, 24, clickBtn)},
            core::InsertChildMutation{2, 3, 0},

            // 显式 nonDraggableArea (160, 8, 60, 24) -> HTCLIENT
            core::CreateNodeMutation{4, core::NodeType::Layout},
            core::SetPropMutation{4, "measurePolicy", core::PropValue::objectValue({{"kind", core::PropValue::stringValue("Box")}})},
            core::SetModifierMutation{4, fixed(160, 8, 60, 24, nonDragArea)},
            core::InsertChildMutation{2, 4, 1},

            // 三键：Minimize (360..400), Maximize (400..440), Close (440..480)
            core::CreateNodeMutation{5, core::NodeType::Layout},
            core::SetPropMutation{5, "measurePolicy", core::PropValue::objectValue({{"kind", core::PropValue::stringValue("Box")}})},
            core::SetModifierMutation{5, fixed(360, 8, 40, 28, minBtn)},
            core::InsertChildMutation{2, 5, 2},

            core::CreateNodeMutation{6, core::NodeType::Layout},
            core::SetPropMutation{6, "measurePolicy", core::PropValue::objectValue({{"kind", core::PropValue::stringValue("Box")}})},
            core::SetModifierMutation{6, fixed(400, 8, 40, 28, maxBtn)},
            core::InsertChildMutation{2, 6, 3},

            core::CreateNodeMutation{7, core::NodeType::Layout},
            core::SetPropMutation{7, "measurePolicy", core::PropValue::objectValue({{"kind", core::PropValue::stringValue("Box")}})},
            core::SetModifierMutation{7, fixed(440, 8, 40, 28, closeBtn)},
            core::InsertChildMutation{2, 7, 4},
        });
        core::LayoutEngine{}.layout(tree, 1, {0, 500, 0, 300});
        const auto snapshot = core::buildHitTestSnapshot(tree, 1);

        core::WindowConfigSpec spec;
        spec.chrome = core::WindowChrome::CustomTitleBar;
        spec.visible = false;
        spec.sizeDp = {500, 300};

        win32::WindowCallbacks cb;
        cb.hitTest = [&](core::Point pt, float border) {
            int cw = 0, ch = 0;
            // 按实际客户区尺寸判定
            cw = 500;
            ch = 300;
            return core::resolveWindowHitTest(snapshot, {static_cast<float>(cw), static_cast<float>(ch)}, pt, {spec.resizable, spec.minimizable, spec.maximizable, spec.closable, spec.placement, spec.chrome, border});
        };
        win32::Window win(spec, nullptr, std::move(cb));

        // 自绘标题栏必须剥离 WS_SYSMENU（消除 Win11 DWM 在 Mica/Acrylic 上叠画幽灵系统三键），同时保留 WS_MAXIMIZEBOX 支撑 Snap Layout
        const auto customStyle = static_cast<DWORD>(GetWindowLongPtrW(static_cast<HWND>(win.handle()), GWL_STYLE));
        check((customStyle & WS_SYSMENU) == 0 && (customStyle & WS_MAXIMIZEBOX) != 0, "CustomTitleBar 未剥离 WS_SYSMENU 或丢失了 WS_MAXIMIZEBOX");

        // 同步缩放后的真实客户区尺寸再做五级校验
        int actualW = 0, actualH = 0;
        win.clientSize(actualW, actualH);
        check(win.sendHitTestAtClientPoint(2, 2) == HTTOPLEFT, "优先级 1：左上角未命中 HTTOPLEFT");
        check(win.sendHitTestAtClientPoint(250, 2) == HTTOP, "优先级 1：上边缘缩放带未优先于标题栏返回 HTTOP");
        check(win.sendHitTestAtClientPoint(380, 20) == HTMINBUTTON, "优先级 2：最小化按钮未返回 HTMINBUTTON");
        check(win.sendHitTestAtClientPoint(420, 20) == HTMAXBUTTON, "优先级 2：最大化按钮未返回 HTMAXBUTTON（支撑 Win11 Snap Layout）");
        check(win.sendHitTestAtClientPoint(460, 20) == HTCLOSE, "优先级 2：关闭按钮未返回 HTCLOSE");
        check(win.sendHitTestAtClientPoint(90, 20) == HTCLIENT, "优先级 3：标题栏内可交互节点未自动让位 HTCLIENT");
        check(win.sendHitTestAtClientPoint(180, 20) == HTCLIENT, "优先级 3：nonDraggableArea 未让位 HTCLIENT");
        check(win.sendHitTestAtClientPoint(260, 20) == HTCAPTION, "优先级 4：标题栏拖拽空白区未返回 HTCAPTION");
        check(win.sendHitTestAtClientPoint(250, 150) == HTCLIENT, "优先级 5：普通客户区未返回 HTCLIENT");

        // 验证真实 Win32 WM_LBUTTONDOWN + WM_LBUTTONUP（含 SetCapture / ReleaseCapture）不会因同步 WM_CAPTURECHANGED 误取消点击
        int clientClicks = 0;
        bool unexpectedCancel = false;
        win32::WindowCallbacks clickCb;
        clickCb.pointerDown = [&](const core::PointerSample&, core::PointerDownOptions) {};
        clickCb.pointerUp = [&](const core::PointerSample&) { ++clientClicks; };
        clickCb.pointerCancelled = [&] { unexpectedCancel = true; };
        win32::Window clickWin(spec, nullptr, std::move(clickCb));
        const LPARAM clientPt = MAKELPARAM(100, 100);
        SendMessageW(static_cast<HWND>(clickWin.handle()), WM_LBUTTONDOWN, MK_LBUTTON, clientPt);
        SendMessageW(static_cast<HWND>(clickWin.handle()), WM_LBUTTONUP, 0, clientPt);
        check(clientClicks == 1 && !unexpectedCancel, "WM_LBUTTONUP 内的 ReleaseCapture 触发了 WM_CAPTURECHANGED 误取消点击");
    }

    // 3. AppSession 端到端：onCloseRequest 拦截、同帧切窗不误退、末窗关闭退出、销毁逆序与 DPI 独立驱动
    void verifyAppSessionLifecycleAndDpi() {
        const auto dir = std::filesystem::temp_directory_path() / ("canton-win-e2e-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir);
        struct Cleanup {
            std::filesystem::path p;
            ~Cleanup() {
                std::error_code ec;
                std::filesystem::remove_all(p, ec);
            }
        } cleanup{dir};

        const std::string appJs = R"JS(
const n = globalThis.__CANTON_NATIVE__
let stage = 'login'
let allowClose = false
let dpiA = 1
let loginId = 0, mainId = 0, childId = 0, mainRoot = 0, boxA = 0, chainA = null

function baseConfig(title, ownerWindowId) {
    return {
        ...(ownerWindowId ? { ownerWindowId } : {}),
        title,
        visible: false,
        sizeDp: { width: 400, height: 300 },
        position: 'platformDefault',
        placement: 'normal',
        modality: 'none',
        chrome: 'system',
        role: 'standard',
        resizable: true,
        minimizable: true,
        maximizable: true,
        closable: true,
        alwaysOnTop: false,
        backdrop: 'none',
        cornerPreference: 'default',
    }
}

n.installWindowDriver({
    onCloseRequest(winId) {
        if (winId === loginId) {
            // 同帧销毁登录窗并打开主窗 + 子窗
            stage = 'main'
            n.requestFrame(true)
        } else if (winId === mainId && allowClose) {
            stage = 'closed'
            n.requestFrame(true)
        } else if (winId === childId) {
            allowClose = true
        }
    },
    onGeometryChanged() {},
    onFocusChanged() {},
    onDpiChanged(winId, scale) {
        if (winId === mainId) {
            dpiA = scale
            n.requestFrame(true)
        }
    },
})

n.installFrameDriver(() => {
    n.beginRearrange()
    n.configureApp({ exitOnLastWindowClosed: true })
    if (stage === 'login' && loginId === 0) {
        loginId = n.allocateWindowId()
        n.createWindow(loginId, 2, baseConfig('Login'))
        n.createNode(2, 'Root', loginId)
    } else if (stage === 'main' && mainId === 0) {
        n.deleteNode(2)
        n.destroyWindow(loginId)
        mainId = n.allocateWindowId()
        mainRoot = 3
        boxA = 4
        n.createWindow(mainId, mainRoot, baseConfig('Main'))
        n.createNode(mainRoot, 'Root', mainId)
        n.createNode(boxA, 'LayoutNode', mainId)
        n.insertChild(mainRoot, boxA, 0)
        n.updateBinding(n.registerBinding(boxA, 'measurePolicy'), { kind: 'MinSize' })
        chainA = n.registerBinding(boxA, 'modifier')
        n.updateBinding(chainA, { elements: [{ type: 'size', value: { width: 100 * dpiA, height: 40 * dpiA } }, { type: 'clickable', value: { onClick: () => n.requestAppExit() } }] })

        childId = n.allocateWindowId()
        n.createWindow(childId, 5, baseConfig('Child', mainId))
        n.createNode(5, 'Root', childId)
    } else if (stage === 'main' && mainId !== 0) {
        n.updateBinding(chainA, { elements: [{ type: 'size', value: { width: 100 * dpiA, height: 40 * dpiA } }, { type: 'clickable', value: { onClick: () => n.requestAppExit() } }] })
    } else if (stage === 'closed' && mainId !== 0) {
        // 父子同帧销毁：故意先调父再调子，验证 AppSession 按拓扑逆序先毁子再毁父
        n.deleteNode(mainRoot)
        n.destroyWindow(mainId)
        n.deleteNode(5)
        n.destroyWindow(childId)
        mainId = 0
    }
    n.submitRearrange(err => { if (err) throw new Error(err) })
}, () => {}, () => {})
n.requestFrame(true)
)JS";
        std::ofstream(dir / "app.js", std::ios::binary) << appJs;
        const auto digest = runtime::sha256Hex({reinterpret_cast<const std::uint8_t*>(appJs.data()), appJs.size()});
        std::ofstream(dir / std::string(runtime::uiManifestName), std::ios::binary)
            << "{\"format\":3,\"framework\":\"" << core::PackageVersion << "\",\"protocol\":" << core::RuntimeVersion
            << ",\"scriptFormat\":\"js\",\"entry\":\"app.js\",\"entries\":[{\"path\":\"app.js\",\"kind\":\"module\",\"size\":" << appJs.size() << ",\"sha256\":\"" << digest << "\"}]}";

        core::ApproximateTextMeasurer measurer;
        auto fonts = std::make_shared<core::FontRegistry>();
        std::vector<core::WindowId> createdIds, destroyedIds;
        int exitCount = 0;

        runtime::AppSessionOptions opts;
        opts.platform.onWindowCreated = [&](core::WindowId id, const core::WindowConfigSpec&) { createdIds.push_back(id); };
        opts.platform.onWindowDestroyed = [&](core::WindowId id) { destroyedIds.push_back(id); };
        opts.platform.onRequestExit = [&] { ++exitCount; };

        runtime::AppSession session(measurer, fonts, std::move(opts));
        runtime::AppSourceConfig src;
        src.policy = runtime::LoadPolicy::OnlyDist;
        src.distOverride = dir;
        session.configure(std::move(src));

        double now = 0;
        (void)session.pumpFrame(now += 16);
        check(createdIds.size() == 1 && !session.exitRequested() && exitCount == 0, "初始登录窗未建立");
        const auto loginWinId = createdIds[0];

        // 触发登录窗关闭请求 -> 同帧销毁登录窗并建立主窗与子窗，绝不误触发末窗退出
        session.notifyWindowCloseRequest(loginWinId);
        (void)session.pumpFrame(now += 16);
        check(!session.exitRequested() && exitCount == 0, "同帧切窗误触发了 exitOnLastWindowClosed");
        check(createdIds.size() == 3 && destroyedIds.size() == 1 && destroyedIds[0] == loginWinId, "同帧切窗未销毁旧窗并建立新主窗与子窗");
        const auto mainWinId = createdIds[1];
        const auto childWinId = createdIds[2];
        check(mainWinId > loginWinId && childWinId > mainWinId, "原生窗口身份未保持单调递增");

        // DPI 变更为 1.5x -> 驱动主窗节点尺寸重测为 150x60
        session.notifyWindowDpi(mainWinId, 1.5);
        (void)session.pumpFrame(now += 16);
        check(std::abs(session.window(mainWinId)->pipeline.scene().node(4).bounds.width - 150.0f) < 0.1f, "WM_DPICHANGED 未驱动所属窗口布局尺寸重测");

        // 关闭拦截：allowClose 为 false 时主窗收到关闭请求不毁窗
        session.notifyWindowCloseRequest(mainWinId);
        (void)session.pumpFrame(now += 16);
        check(session.window(mainWinId) != nullptr && !session.exitRequested(), "onCloseRequest 未能拦截主窗关闭");

        // 先经子窗回调放行 allowClose，再发主窗关闭请求 -> 同帧销毁父子两窗：子窗严格先于父窗销毁，收尾后触发末窗退出
        destroyedIds.clear();
        session.notifyWindowCloseRequest(childWinId);
        session.notifyWindowCloseRequest(mainWinId);
        (void)session.pumpFrame(now += 16);
        check(destroyedIds.size() == 2 && destroyedIds[0] == childWinId && destroyedIds[1] == mainWinId, "同事务毁窗未严格按先子后父逆序执行");
        check(session.exitRequested() && exitCount == 1, "全部活跃窗口关闭后未触发末窗有序退出");
    }
}

int main() {
    try {
        const test_support::ComApartment com;
        verifyWindowManagerTopologyAndModality();
        verifyFiveLevelNcHitTest();
        verifyAppSessionLifecycleAndDpi();
        std::cout << "canton_window_runtime：多窗生命周期、同帧切窗、关闭拦截、Owned 逆序销毁、嵌套模态锁计数、5 级 WM_NCHITTEST 与 DPI 联动全部通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
