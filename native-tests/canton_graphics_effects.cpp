#include <core/GraphicsEffects.h>
#include <core/Layout.h>
#include <core/Modifier.h>
#include <core/Paint.h>
#include <core/SceneFramePipeline.h>
#include <d3d11/Renderer.h>
#include <d3d11/ShaderCompiler.h>

#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace canton::core;
using namespace canton::d3d11;

namespace {
    void check(bool condition, const std::string& message) {
        if (!condition) throw std::runtime_error(message);
    }

    bool near(float a, float b, float eps = 1e-3f) {
        return std::abs(a - b) <= eps;
    }

    float readFloat(const std::vector<std::uint8_t>& bytes, std::uint32_t offset) {
        float value = 0.0f;
        check(offset + sizeof(float) <= bytes.size(), "常量缓冲读取越界");
        std::memcpy(&value, bytes.data() + offset, sizeof(float));
        return value;
    }

    void testCompileDiagnosticsAndReflectPacking() {
        D3DShaderCompiler compiler;

        // 1. 语法错误精确行号诊断
        const std::string broken =
            "#include <canton.hlsl>\n"
            "float4 main(CantonInput input) : SV_Target {\n"
            "    float2 uv = input.uv;\n"
            "    float4 bad = missingSymbol + uv.x;\n"
            "    return bad;\n"
            "}\n";
        bool threw = false;
        try {
            (void)compiler.compileHlsl(broken, "main", "diag_test.hlsl");
        } catch (const std::exception& err) {
            threw = true;
            const std::string msg = err.what();
            check(msg.find("diag_test.hlsl") != std::string::npos, "编译诊断缺少文件名：" + msg);
            check(msg.find("(4,") != std::string::npos, "编译诊断缺少第 4 行行号：" + msg);
        }
        check(threw, "非法 HLSL 应当编译失败");

        // 2. D3DReflect 跨 16 字节边界、数组步长、矩阵与预乘颜色封包
        const std::string packingShader =
            "#include <canton.hlsl>\n"
            "cbuffer Params : register(b0) {\n"
            "    float scalarA;\n"
            "    float3 vecB;\n"
            "    float4 arr[2];\n"
            "    float2x2 mat;\n"
            "    float4 tint;\n"
            "    int mode;\n"
            "};\n"
            "float4 main(CantonInput input) : SV_Target {\n"
            "    float2 p = mul(input.uv, mat);\n"
            "    return tint * scalarA + float4(vecB, 1.0) * 0.01 + (arr[0] + arr[1]) * 0.01 + float4(p, (float)mode * 0.01, 0.0);\n"
            "}\n";

        const auto compiled = compiler.compileHlsl(packingShader, "main", "pack_test.hlsl");
        check(compiled.dxbc.size() > 64, "DXBC 字节码为空");
        check(compiled.reflection.buffers.size() == 1 && compiled.reflection.buffers[0].slot == 0, "未正确反射 b0 常量缓冲");

        std::vector<ShaderUniformInput> inputs;
        inputs.push_back({"scalarA", ShaderUniformInputKind::Float, {0.75f}, {}, 0});
        inputs.push_back({"vecB", ShaderUniformInputKind::Float, {1.0f, 2.0f, 3.0f}, {}, 0});
        inputs.push_back({"arr", ShaderUniformInputKind::Float, {10, 20, 30, 40, 50, 60, 70, 80}, {}, 0});
        inputs.push_back({"mat", ShaderUniformInputKind::Float, {1, 2, 3, 4}, {}, 0});
        // 0x80ff8040 -> a = 128/255, premul rgb = (255,128,64)/255 * a
        inputs.push_back({"tint", ShaderUniformInputKind::Color, {}, {}, 0x80ff8040u});
        inputs.push_back({"mode", ShaderUniformInputKind::Int, {}, {7}, 0});

        const auto packed = packRuntimeShaderUniforms(compiled.reflection, inputs);
        check(packed.size() == 1 && packed[0].slot == 0, "封包结果槽位错误");
        const auto& bytes = packed[0].bytes;

        const auto findField = [&](std::string_view name) -> const ShaderUniformField& {
            for (const auto& f : compiled.reflection.uniforms)
                if (f.name == name) return f;
            throw std::runtime_error("缺少反射字段：" + std::string(name));
        };

        const auto& fScalar = findField("scalarA");
        const auto& fVecB = findField("vecB");
        const auto& fArr = findField("arr");
        const auto& fTint = findField("tint");
        const auto& fMode = findField("mode");

        check(near(readFloat(bytes, fScalar.startOffset), 0.75f), "scalarA 封包值错误");
        check(near(readFloat(bytes, fVecB.startOffset + 0), 1.0f) && near(readFloat(bytes, fVecB.startOffset + 8), 3.0f), "vecB 封包值错误");
        check(near(readFloat(bytes, fArr.startOffset + 0), 10.0f) && near(readFloat(bytes, fArr.startOffset + 16), 50.0f) && near(readFloat(bytes, fArr.startOffset + 28), 80.0f), "数组 16 字节步长封包错误");

        const float expectedA = 128.0f / 255.0f;
        check(near(readFloat(bytes, fTint.startOffset + 0), 1.0f * expectedA, 2e-3f), "tint R 预乘错误");
        check(near(readFloat(bytes, fTint.startOffset + 4), (128.0f / 255.0f) * expectedA, 2e-3f), "tint G 预乘错误");
        check(near(readFloat(bytes, fTint.startOffset + 8), (64.0f / 255.0f) * expectedA, 2e-3f), "tint B 预乘错误");
        check(near(readFloat(bytes, fTint.startOffset + 12), expectedA, 2e-3f), "tint A 封包错误");

        std::int32_t modeVal = 0;
        std::memcpy(&modeVal, bytes.data() + fMode.startOffset, sizeof(modeVal));
        check(modeVal == 7, "int 字段封包错误");

        // 类型不匹配与维度错误必须抛异常
        bool typeMismatchThrew = false;
        try {
            std::vector<ShaderUniformInput> bad{{"scalarA", ShaderUniformInputKind::Int, {}, {1}, 0}};
            (void)packRuntimeShaderUniforms(compiled.reflection, bad);
        } catch (const std::exception&) {
            typeMismatchThrew = true;
        }
        check(typeMismatchThrew, "标量类型不匹配未拦截");

        bool dimMismatchThrew = false;
        try {
            std::vector<ShaderUniformInput> bad{{"vecB", ShaderUniformInputKind::Float, {1.0f, 2.0f}, {}, 0}};
            (void)packRuntimeShaderUniforms(compiled.reflection, bad);
        } catch (const std::exception&) {
            dimMismatchThrew = true;
        }
        check(dimMismatchThrew, "向量分量数不匹配未拦截");
    }

    void testHotUpdateRetainsPreviousAndDeviceLossRecovery() {
        D3DShaderCompiler compiler;
        RuntimeEffectRegistry registry;

        const std::string redHlsl =
            "#include <canton.hlsl>\n"
            "float4 main(CantonInput input) : SV_Target { return float4(0.8, 0.2, 0.1, 1.0); }\n";
        const auto compiledRed = compiler.compileHlsl(redHlsl, "main", "hot_red.hlsl");
        RuntimeEffectProgram prog;
        prog.debugName = "hot_red.hlsl";
        prog.entryPoint = "main";
        prog.dxbc = compiledRed.dxbc;
        prog.reflection = compiledRed.reflection;
        const auto handle = registry.registerProgram(std::move(prog));
        const auto initial = registry.resolve(handle);
        check(initial && initial->revision == 1, "初始 RuntimeEffect 版本应为 1");

        // 编译失败时不更新注册表，保留旧版本
        bool compileFailed = false;
        try {
            (void)compiler.compileHlsl("broken hlsl", "main", "hot_red.hlsl");
        } catch (const std::exception&) {
            compileFailed = true;
        }
        check(compileFailed && registry.resolve(handle)->revision == 1, "热更编译失败后应保留旧版着色器");

        // 使用 WARP 验证 shaderBrush + RenderEffect.blur + RT 池复用 + 设备丢失重建
        Renderer renderer({AdapterPreference::Warp});
        auto target = renderer.createOffscreenTarget(64, 64);

        auto buildFrame = [&](const RuntimeEffectProgramRef& program) {
            PublishedFrameContent content;
            auto frag = std::make_shared<PaintFragment>();
            auto ops = std::make_shared<std::vector<DrawOp>>();

            DrawOp op;
            op.type = DrawOpType::FillRect;
            op.rect = {8, 8, 48, 48};
            op.shape = DrawShapeType::Rounded;
            op.corners = CornerRadii::uniform(10);
            op.color = 0xffffffffu;
            RuntimeShaderData shaderData;
            shaderData.program = program;
            shaderData.version = 1;
            op.shader = RuntimeShaderRef(std::move(shaderData));
            ops->push_back(std::move(op));
            frag->content = ops;
            frag->bounds = {{8, 8, 48, 48}, true, false};

            auto layer = std::make_shared<PaintLayerFragment>();
            layer->bounds = {0, 0, 64, 64};
            layer->useOffscreenGroup = true;
            layer->groupAlpha = 1.0f;
            RenderEffectData re;
            RenderEffectStep blurStep;
            blurStep.kind = RenderEffectStepKind::Blur;
            blurStep.radiusX = 4.0f;
            blurStep.radiusY = 4.0f;
            blurStep.edgeTreatment = TileMode::Clamp;
            re.steps.push_back(blurStep);
            layer->groupRenderEffect = RenderEffectRef(std::move(re));
            frag->layer = layer;
            frag->bounds = {{0, 0, 64, 64}, true, false};

            content.scenePaint = {frag, {0, 0}};
            return content;
        };

        const auto frameContent = buildFrame(initial);
        renderer.render(*target, {&frameContent, {}, 0xff000000u});
        const auto pixelsBefore = renderer.readPixels(*target);
        check(renderer.counters().userShadersCompiled >= 1, "用户着色器未计入编译计数");
        check(renderer.counters().offscreenTargetsCreated >= 1, "离屏 RT 池未创建纹理");
        check(renderer.counters().offscreenPasses >= 2, "离屏后处理 Pass 未执行");

        // 第二帧应命中离屏 RT 池复用
        const auto reusesBefore = renderer.counters().offscreenTargetReuses;
        renderer.render(*target, {&frameContent, {}, 0xff000000u});
        check(renderer.counters().offscreenTargetReuses > reusesBefore, "连续帧未复用离屏 RT 池");

        // 模拟设备丢失并重建，用户着色器必须从缓存 DXBC 自动恢复且画面逐像素一致
        const auto compiledBeforeRebuild = renderer.counters().userShadersCompiled;
        renderer.recreateDevice();
        renderer.render(*target, {&frameContent, {}, 0xff000000u});
        const auto pixelsAfter = renderer.readPixels(*target);
        check(renderer.counters().userShadersCompiled > compiledBeforeRebuild, "设备重建后用户着色器未重新创建");
        check(pixelsBefore == pixelsAfter, "设备丢失重建后着色器/离屏渲染像素不一致");
    }

    void testRenderLayerCycleDetection() {
        // 1. 直接自引用环：A 录制时绘制 A
        {
            auto frag = std::make_shared<PaintFragment>();
            auto layer = std::make_shared<PaintLayerFragment>();
            layer->drawCommands = std::make_shared<DrawCommands>();

            DrawOp beginOp;
            beginOp.type = DrawOpType::BeginRecordLayer;
            beginOp.layerHandle = {10, 1};
            beginOp.rect = {0, 0, 64, 64};

            DrawOp drawOp;
            drawOp.type = DrawOpType::DrawLayer;
            RenderLayerSnapshot snap;
            snap.handle = {10, 1};
            drawOp.layerSnapshot = RenderLayerSnapshotRef(std::move(snap));

            DrawOp endOp;
            endOp.type = DrawOpType::EndRecordLayer;

            layer->before = {beginOp, drawOp, endOp};
            frag->layer = layer;

            bool threw = false;
            try {
                validateRenderLayerCycles({frag, {0, 0}});
            } catch (const std::exception& err) {
                threw = true;
                check(std::string(err.what()).find("环") != std::string::npos, "直接环错误信息不清晰");
            }
            check(threw, "未检出 RenderLayer 直接自引用环");
        }

        // 2. 间接环：A -> B -> A
        {
            auto frag = std::make_shared<PaintFragment>();
            auto layer = std::make_shared<PaintLayerFragment>();
            layer->drawCommands = std::make_shared<DrawCommands>();

            auto makeBegin = [](std::uint64_t id) {
                DrawOp op;
                op.type = DrawOpType::BeginRecordLayer;
                op.layerHandle = {id, 1};
                op.rect = {0, 0, 32, 32};
                return op;
            };
            auto makeDraw = [](std::uint64_t id) {
                DrawOp op;
                op.type = DrawOpType::DrawLayer;
                RenderLayerSnapshot snap;
                snap.handle = {id, 1};
                op.layerSnapshot = RenderLayerSnapshotRef(std::move(snap));
                return op;
            };
            DrawOp endOp;
            endOp.type = DrawOpType::EndRecordLayer;

            layer->before = {makeBegin(1), makeDraw(2), endOp, makeBegin(2), makeDraw(1), endOp};
            frag->layer = layer;

            bool threw = false;
            try {
                validateRenderLayerCycles({frag, {0, 0}});
            } catch (const std::exception&) {
                threw = true;
            }
            check(threw, "未检出 RenderLayer 间接引用环");
        }
    }

    void testGroupOpacityVsModulateAlpha() {
        Renderer renderer({AdapterPreference::Warp});
        auto target = renderer.createOffscreenTarget(64, 64);

        auto renderCase = [&](bool offscreen) {
            PublishedFrameContent content;
            auto frag = std::make_shared<PaintFragment>();
            auto ops = std::make_shared<std::vector<DrawOp>>();

            DrawOp red;
            red.type = DrawOpType::FillRect;
            red.rect = {4, 4, 40, 40};
            red.color = 0xffff0000u;
            DrawOp blue;
            blue.type = DrawOpType::FillRect;
            blue.rect = {16, 16, 40, 40};
            blue.color = 0xff0000ffu;
            ops->push_back(red);
            ops->push_back(blue);
            frag->content = ops;
            frag->bounds = {{4, 4, 52, 52}, true, false};

            auto layer = std::make_shared<PaintLayerFragment>();
            layer->bounds = {0, 0, 64, 64};
            layer->useOffscreenGroup = offscreen;
            layer->groupAlpha = 0.5f;
            layer->contentAlpha = offscreen ? 1.0f : 0.5f;
            frag->layer = layer;

            content.scenePaint = {frag, {0, 0}};
            renderer.render(*target, {&content, {}, 0xff000000u});
            return renderer.readPixels(*target);
        };

        const auto offPixels = renderCase(true);
        const auto modPixels = renderCase(false);
        // 重叠区中心像素 (28, 28)
        const std::uint32_t offP = offPixels[28 * 64 + 28];
        const std::uint32_t modP = modPixels[28 * 64 + 28];
        const int offR = static_cast<int>((offP >> 16) & 0xffu);
        const int offB = static_cast<int>(offP & 0xffu);
        const int modR = static_cast<int>((modP >> 16) & 0xffu);
        const int modB = static_cast<int>(modP & 0xffu);

        // Offscreen：先不透明蓝盖红，再整组 0.5 叠黑底 -> R=0, B≈128
        check(offR <= 1 && std::abs(offB - 128) <= 2, "Offscreen 组透明度重叠区不应渗出底层红色");
        // ModulateAlpha：先 0.5 红，再 0.5 蓝 -> R≈64, B≈128
        check(std::abs(modR - 64) <= 2 && std::abs(modB - 128) <= 2, "ModulateAlpha 逐图元透明度计算异常");
    }

    void testOnGloballyPositionedTimingAndRollback() {
        NativeScene scene;
        PublishedFrame published;
        SceneFramePipeline pipeline;
        auto& tree = scene.tree();
        tree.apply({CreateNodeMutation{1, NodeType::Layout}, CreateNodeMutation{2, NodeType::Layout}, InsertChildMutation{1, 2, 0}});
        tree.mutableNode(1).measurePolicy = BoxMeasurePolicy{};
        tree.mutableNode(2).measurePolicy = BoxMeasurePolicy{};

        LayoutModifierSemantics sz;
        sz.kind = LayoutModifierKind::Size;
        sz.width = 80;
        sz.height = 40;
        OffsetModifier off;
        off.x = 12;
        off.y = 20;
        PositionedModifier posMod;
        posMod.callback = {2, EventSlotKind::GloballyPositioned, "onGloballyPositioned", 1, 1};
        tree.setModifierChain(2, {{off}, {sz}, {posMod}});

        std::vector<std::string> order;
        int positionedCalls = 0;
        PositionedUpdate lastUpdate;

        FramePreparation prep;
        prep.positioned = [&](NativeScene&, const std::vector<PositionedUpdate>& updates) {
            order.push_back("positioned");
            positionedCalls += static_cast<int>(updates.size());
            if (!updates.empty()) lastUpdate = updates.back();
        };
        prep.draw = [&](NativeScene&) {
            order.push_back("draw");
        };

        // 1. 首帧必发且在 draw 之前
        (void)pipeline.prepare(scene, 1, {0, 400, 0, 300}, nullptr, true, published, {}, 0.0, prep);
        pipeline.commit(scene, published);
        check(positionedCalls == 1, "首帧应派发一次 onGloballyPositioned");
        check(order.size() >= 1 && order[0] == "positioned", "onGloballyPositioned 必须在 draw 之前派发");
        check(near(static_cast<float>(lastUpdate.geometry.localToRoot.tx), 12.0f) && near(static_cast<float>(lastUpdate.geometry.localToRoot.ty), 20.0f) && near(lastUpdate.geometry.size.width, 80.0f), "几何坐标错误");

        // 2. 几何未变不重发
        positionedCalls = 0;
        (void)pipeline.prepare(scene, 1, {0, 400, 0, 300}, nullptr, true, published, {}, 16.0, prep);
        pipeline.commit(scene, published);
        check(positionedCalls == 0, "几何未变化不应重复派发 onGloballyPositioned");

        // 3. 候选帧修改位置后回滚，下一次真正提交必须重新派发
        MutationTransaction moveTx;
        off.x = 50;
        moveTx.operations.push_back(TreeMutation{SetModifierMutation{2, {{off}, {sz}, {posMod}}}});
        positionedCalls = 0;
        (void)pipeline.prepare(scene, 1, {0, 400, 0, 300}, &moveTx, true, published, {}, 32.0, prep);
        check(positionedCalls == 1 && near(static_cast<float>(lastUpdate.geometry.localToRoot.tx), 50.0f), "候选帧位置变化应派发");
        pipeline.discard(scene);

        // 回滚后 lastPositioned 回到 (12, 20)，再次 prepare (50, 20) 必须再次派发
        positionedCalls = 0;
        (void)pipeline.prepare(scene, 1, {0, 400, 0, 300}, &moveTx, true, published, {}, 48.0, prep);
        pipeline.commit(scene, published);
        check(positionedCalls == 1 && near(static_cast<float>(lastUpdate.geometry.localToRoot.tx), 50.0f), "候选回滚后下帧必须重新派发位置变更");
    }
}

int main() {
    try {
        testCompileDiagnosticsAndReflectPacking();
        testHotUpdateRetainsPreviousAndDeviceLossRecovery();
        testRenderLayerCycleDetection();
        testGroupOpacityVsModulateAlpha();
        testOnGloballyPositionedTimingAndRollback();
        std::cout << "canton_graphics_effects passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
