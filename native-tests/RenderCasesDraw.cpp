#include "RenderRegression.h"

#include <core/Modifier.h>
#include <d3d11/ShaderCompiler.h>
#include <runtime/PainterResources.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winerror.h>
#undef DrawText

#include <cmath>

// 直接喂绘制指令的用例：只验渲染器，不经脚本和布局
namespace render_regression {
    namespace {
        using namespace canton::core;

        DrawOp fill(Rect rect, std::uint32_t color, DrawShapeType shape = DrawShapeType::Rectangle, CornerRadii corners = {}) {
            DrawOp op;
            op.type = DrawOpType::FillRect;
            op.rect = rect;
            op.color = color;
            op.shape = shape;
            op.corners = corners;
            return op;
        }

        DrawOp stroke(Rect rect, std::uint32_t color, float width, DrawShapeType shape = DrawShapeType::Rectangle, CornerRadii corners = {}) {
            auto op = fill(rect, color, shape, corners);
            op.type = DrawOpType::StrokeRect;
            op.strokeWidth = width;
            return op;
        }

        // border 修饰器的描边：落在轮廓内侧
        DrawOp border(Rect rect, std::uint32_t color, float width, DrawShapeType shape = DrawShapeType::Rectangle, CornerRadii corners = {}) {
            auto op = stroke(rect, color, width, shape, corners);
            op.strokeInside = true;
            return op;
        }

        DrawOp line(Point from, Point to, std::uint32_t color, float width, StrokeCap cap = StrokeCap::Butt) {
            DrawOp op;
            op.type = DrawOpType::DrawLine;
            op.rect = {from.x, from.y, 0, 0};
            op.lineEnd = to;
            op.color = color;
            op.strokeWidth = width;
            op.cap = cap;
            return op;
        }

        DrawOp label(Rect rect, std::string value, std::uint32_t color = 0xffe5e7ebu, float size = 12) {
            DrawOp op;
            op.type = DrawOpType::DrawText;
            op.rect = rect;
            op.text = std::move(value);
            op.color = color;
            op.textStyle.fontSize = size;
            op.textStyle.fontFamily = {test_support::testFontFamily};
            op.maxLines = 1;
            return op;
        }

        DrawOp withGradient(DrawOp op, Gradient gradient) {
            op.gradient = GradientRef(std::move(gradient));
            op.color = 0xffffffffu;
            return op;
        }

        Gradient linear(Point start, Point end, std::vector<GradientStop> stops) {
            Gradient gradient;
            gradient.kind = GradientKind::Linear;
            gradient.start = start;
            gradient.end = end;
            gradient.stops = std::move(stops);
            return gradient;
        }

        DrawOp pushClip(Rect rect, DrawShapeType shape = DrawShapeType::Rectangle, CornerRadii corners = {}) {
            auto op = fill(rect, 0, shape, corners);
            op.type = DrawOpType::PushClip;
            return op;
        }

        DrawOp pop(DrawOpType type) {
            DrawOp op;
            op.type = type;
            return op;
        }

        DrawOp pushTransform(Rect rect, float rotation, float scaleX = 1, float scaleY = 1, float translationX = 0, float translationY = 0, float originX = 0.5f, float originY = 0.5f) {
            DrawOp op;
            op.type = DrawOpType::PushTransform;
            op.rect = rect;
            op.rotationZ = rotation;
            op.scaleX = scaleX;
            op.scaleY = scaleY;
            op.translationX = translationX;
            op.translationY = translationY;
            op.transformOriginX = originX;
            op.transformOriginY = originY;
            return op;
        }

        Rect expandedBy(Rect rect, float amount) {
            return {rect.x - amount, rect.y - amount, rect.width + 2 * amount, rect.height + 2 * amount};
        }

        const std::vector<GradientStop> sunset{{0.0f, 0xfff97316u}, {0.5f, 0xffec4899u}, {1.0f, 0xff6366f1u}};

        Image shapes(CaseContext& context) {
            std::vector<DrawOp> ops;
            // 第一行：矩形、统一圆角、分角圆角、圆、椭圆
            ops.push_back(fill({20, 20, 80, 60}, 0xff3b82f6u));
            ops.push_back(fill({120, 20, 80, 60}, 0xff22c55eu, DrawShapeType::Rounded, CornerRadii::uniform(16)));
            ops.push_back(fill({220, 20, 80, 60}, 0xfff59e0bu, DrawShapeType::Rounded, {0, 30, 8, 20}));
            ops.push_back(fill({320, 20, 60, 60}, 0xffef4444u, DrawShapeType::Circle));
            ops.push_back(fill({400, 20, 100, 60}, 0xffa855f7u, DrawShapeType::Circle));
            // 第二行：Canvas 的形状描边，一律以轮廓为中线；矩形外角是尖角
            ops.push_back(stroke({20, 110, 80, 60}, 0xff93c5fdu, 6));
            ops.push_back(stroke({120, 110, 80, 60}, 0xff86efacu, 6, DrawShapeType::Rounded, CornerRadii::uniform(16)));
            ops.push_back(stroke({220, 110, 80, 60}, 0xfffcd34du, 2, DrawShapeType::Rounded, {0, 30, 8, 20}));
            ops.push_back(stroke({320, 110, 60, 60}, 0xfffca5a5u, 8, DrawShapeType::Circle));
            ops.push_back(stroke({400, 110, 100, 60}, 0xffd8b4feu, 1, DrawShapeType::Circle));
            // 第三行：线段与三种端点；半像素位置与半透明叠加
            ops.push_back(line({30, 210}, {110, 210}, 0xffe5e7ebu, 12, StrokeCap::Butt));
            ops.push_back(line({30, 235}, {110, 235}, 0xffe5e7ebu, 12, StrokeCap::Round));
            ops.push_back(line({30, 260}, {110, 260}, 0xffe5e7ebu, 12, StrokeCap::Square));
            ops.push_back(line({140, 200}, {200, 270}, 0xff38bdf8u, 1));
            ops.push_back(line({160, 200}, {220, 270}, 0xff38bdf8u, 4, StrokeCap::Round));
            ops.push_back(fill({250.5f, 200.5f, 40, 40}, 0xffffffffu));
            ops.push_back(fill({300.25f, 200.25f, 40.5f, 40.5f}, 0xffffffffu, DrawShapeType::Rounded, CornerRadii::uniform(6)));
            ops.push_back(fill({370, 200, 60, 60}, 0x80ef4444u, DrawShapeType::Circle));
            ops.push_back(fill({400, 200, 60, 60}, 0x8022c55eu, DrawShapeType::Circle));
            ops.push_back(fill({385, 230, 60, 60}, 0x803b82f6u, DrawShapeType::Circle));
            // 第四行：border 修饰器，一律落在轮廓内侧；细线框标出外轮廓
            const Rect borders[]{{20, 300, 80, 50}, {120, 300, 80, 50}, {220, 300, 80, 50}, {320, 300, 50, 50}};
            for (const auto& rect : borders) ops.push_back(stroke(rect, 0xff475569u, 1));
            ops.push_back(border(borders[0], 0xff93c5fdu, 6));
            ops.push_back(border(borders[1], 0xff86efacu, 6, DrawShapeType::Rounded, CornerRadii::uniform(16)));
            ops.push_back(border(borders[2], 0xfffcd34du, 4, DrawShapeType::Rounded, {0, 24, 8, 16}));
            ops.push_back(border(borders[3], 0xfffca5a5u, 8, DrawShapeType::Circle));
            return context.renderOps(std::move(ops));
        }

        Image gradients(CaseContext& context) {
            std::vector<DrawOp> ops;
            ops.push_back(withGradient(fill({20, 20, 140, 80}, 0), linear({0, 0}, {1, 0}, sunset)));
            ops.push_back(withGradient(fill({180, 20, 140, 80}, 0, DrawShapeType::Rounded, CornerRadii::uniform(20)), linear({0, 0}, {1, 1}, sunset)));
            ops.push_back(withGradient(fill({340, 20, 140, 80}, 0), linear({0, 0}, {0, 1}, {{0.0f, 0xffffffffu}, {0.5f, 0x00ffffffu}, {0.5f, 0xff000000u}, {1.0f, 0xff22c55eu}})));
            Gradient radial;
            radial.kind = GradientKind::Radial;
            radial.stops = sunset;
            ops.push_back(withGradient(fill({20, 120, 140, 100}, 0), radial));
            radial.center = {0.25f, 0.25f};
            radial.radius = 0.5f;
            ops.push_back(withGradient(fill({180, 120, 100, 100}, 0, DrawShapeType::Circle), radial));
            Gradient sweep;
            sweep.kind = GradientKind::Sweep;
            sweep.stops = {{0.0f, 0xffef4444u}, {0.33f, 0xff22c55eu}, {0.66f, 0xff3b82f6u}, {1.0f, 0xffef4444u}};
            ops.push_back(withGradient(fill({300, 120, 100, 100}, 0, DrawShapeType::Circle), sweep));
            sweep.startAngle = 90;
            ops.push_back(withGradient(stroke({420, 130, 80, 80}, 0, 12, DrawShapeType::Circle), sweep));
            // 渐变描边、渐变线段，以及半透明的渐变
            ops.push_back(withGradient(stroke({20, 240, 140, 60}, 0, 8, DrawShapeType::Rounded, CornerRadii::uniform(12)), linear({0, 0}, {1, 0}, sunset)));
            ops.push_back(withGradient(line({190, 250}, {310, 290}, 0, 10, StrokeCap::Round), linear({0, 0}, {1, 1}, sunset)));
            auto faded = withGradient(fill({340, 240, 140, 60}, 0), linear({0, 0}, {1, 0}, sunset));
            faded.color = 0x66ffffffu;
            ops.push_back(faded);
            return context.renderOps(std::move(ops));
        }
    }

    namespace {
        // 带阴影的一层：阴影先画，随后是这一层自己的指令
        PlacedPaintFragment effectLayer(Point offset, Size size, std::vector<EffectModifier> effects, std::vector<DrawOp> ops) {
            PlacedPaintFragment current;
            {
                PaintFragment content;
                content.content = std::make_shared<const std::vector<DrawOp>>(std::move(ops));
                content.bounds = {{}, false, false};
                current = {std::make_shared<const PaintFragment>(std::move(content)), {}};
            }
            // 修饰链靠前的在外层，倒着包
            for (auto it = effects.rbegin(); it != effects.rend(); ++it) {
                auto layer = std::make_shared<PaintLayerFragment>();
                layer->value = *it;
                layer->bounds = {0, 0, size.width, size.height};
                PaintFragment wrapper;
                wrapper.layer = layer;
                wrapper.children.push_back(current);
                wrapper.bounds = {{}, false, false};
                current = {std::make_shared<const PaintFragment>(std::move(wrapper)), {}};
            }
            current.offset = offset;
            return current;
        }

        EffectModifier shadow(EffectKind kind, ShapeKind shape, CornerRadii corners, float radius, float spread, Point offset, std::uint32_t color) {
            EffectModifier effect;
            effect.kind = kind;
            effect.shape.shapeType = shape;
            effect.shape.corners = corners;
            effect.radius = radius;
            effect.spread = spread;
            effect.offset = offset;
            effect.color = color;
            return effect;
        }

        Image shadows(CaseContext& context) {
            PaintFragment root;
            root.bounds = {{}, false, false};
            root.content = std::make_shared<const std::vector<DrawOp>>(std::vector<DrawOp>{fill({0, 0, 520, 320}, 0xfff3f4f6u)});
            const auto card = [&](Point at, Size size, ShapeKind shape, CornerRadii corners, std::vector<EffectModifier> effects, std::uint32_t color = 0xffffffffu) {
                const auto drawShape = shape == ShapeKind::Circle ? DrawShapeType::Circle : shape == ShapeKind::Rounded ? DrawShapeType::Rounded : DrawShapeType::Rectangle;
                root.children.push_back(effectLayer(at, size, std::move(effects), {fill({0, 0, size.width, size.height}, color, drawShape, corners)}));
            };
            const auto round = CornerRadii::uniform(16);
            // 外阴影：不同模糊半径、扩展与偏移
            card({30, 30}, {100, 70}, ShapeKind::Rectangle, {}, {shadow(EffectKind::DropShadow, ShapeKind::Rectangle, {}, 8, 0, {0, 4}, 0x66000000u)});
            card({170, 30}, {100, 70}, ShapeKind::Rounded, round, {shadow(EffectKind::DropShadow, ShapeKind::Rounded, round, 24, 0, {0, 10}, 0x80000000u)});
            card({310, 30}, {100, 70}, ShapeKind::Rounded, {0, 30, 8, 20}, {shadow(EffectKind::DropShadow, ShapeKind::Rounded, {0, 30, 8, 20}, 12, 6, {6, 6}, 0x803b82f6u)});
            card({440, 30}, {60, 60}, ShapeKind::Circle, {}, {shadow(EffectKind::DropShadow, ShapeKind::Circle, {}, 16, 2, {0, 6}, 0x99ef4444u)});
            // 清晰的外阴影（半径为 0）与大模糊小形状
            card({30, 140}, {100, 70}, ShapeKind::Rounded, round, {shadow(EffectKind::DropShadow, ShapeKind::Rounded, round, 0, 0, {8, 8}, 0xff111827u)});
            card({190, 160}, {30, 30}, ShapeKind::Rounded, CornerRadii::uniform(6), {shadow(EffectKind::DropShadow, ShapeKind::Rounded, CornerRadii::uniform(6), 40, 0, {0, 0}, 0xff000000u)});
            // 内阴影：画在背景之上
            const auto inner = [&](Point at, Size size, ShapeKind shape, CornerRadii corners, float radius, float spread, Point offset) {
                const auto drawShape = shape == ShapeKind::Circle ? DrawShapeType::Circle : shape == ShapeKind::Rounded ? DrawShapeType::Rounded : DrawShapeType::Rectangle;
                PaintFragment background;
                background.bounds = {{}, false, false};
                background.content = std::make_shared<const std::vector<DrawOp>>(std::vector<DrawOp>{fill({0, 0, size.width, size.height}, 0xffe5e7ebu, drawShape, corners)});
                background.children.push_back(effectLayer({}, size, {shadow(EffectKind::InnerShadow, shape, corners, radius, spread, offset, 0xb3000000u)}, {}));
                root.children.push_back({std::make_shared<const PaintFragment>(std::move(background)), at});
            };
            inner({280, 140}, {100, 70}, ShapeKind::Rounded, round, 12, 0, {0, 4});
            inner({410, 140}, {80, 80}, ShapeKind::Circle, {}, 16, 4, {4, 4});
            inner({30, 240}, {100, 60}, ShapeKind::Rectangle, {}, 6, 0, {0, 0});
            // 两层外阴影叠加
            card({190, 240}, {120, 60}, ShapeKind::Rounded, round, {shadow(EffectKind::DropShadow, ShapeKind::Rounded, round, 30, 0, {0, 16}, 0x40000000u), shadow(EffectKind::DropShadow, ShapeKind::Rounded, round, 4, 0, {0, 2}, 0x40000000u)});
            PublishedFrameContent content;
            content.scenePaint = {std::make_shared<const PaintFragment>(std::move(root)), {}};
            return context.render(content);
        }
    }

    namespace {
        PathRef pathOf(std::vector<PathVerb> verbs, std::vector<float> values, bool evenOdd = false) {
            PathData data;
            data.verbs = std::move(verbs);
            data.values = std::move(values);
            data.evenOdd = evenOdd;
            // 外包盒取全部端点与控制点；弧取所在矩形
            float left = 0, top = 0, right = 0, bottom = 0;
            bool any = false;
            const auto include = [&](float x, float y) {
                left = any ? std::min(left, x) : x, top = any ? std::min(top, y) : y;
                right = any ? std::max(right, x) : x, bottom = any ? std::max(bottom, y) : y;
                any = true;
            };
            std::size_t index = 0;
            for (const auto verb : data.verbs) {
                if (verb == PathVerb::ArcTo) {
                    include(data.values[index], data.values[index + 1]);
                    include(data.values[index] + data.values[index + 2], data.values[index + 1] + data.values[index + 3]);
                } else
                    for (int i = 0; i + 1 < pathVerbArity(verb); i += 2) include(data.values[index + static_cast<std::size_t>(i)], data.values[index + static_cast<std::size_t>(i) + 1]);
                index += static_cast<std::size_t>(pathVerbArity(verb));
            }
            data.bounds = {left, top, right - left, bottom - top};
            return PathRef(std::move(data));
        }

        // 五角星：五个外顶点隔一个连一条线，中间自相交
        PathRef star(float radius) {
            std::vector<PathVerb> verbs;
            std::vector<float> values;
            for (int i = 0; i < 5; ++i) {
                const auto angle = (-90.0f + 144.0f * static_cast<float>(i)) * 3.14159265f / 180.0f;
                verbs.push_back(i == 0 ? PathVerb::MoveTo : PathVerb::LineTo);
                values.push_back(radius + radius * std::cos(angle));
                values.push_back(radius + radius * std::sin(angle));
            }
            verbs.push_back(PathVerb::Close);
            return pathOf(std::move(verbs), std::move(values));
        }

        DrawOp drawPath(PathRef path, Point at, std::uint32_t color, float strokeWidth = 0, StrokeCap cap = StrokeCap::Butt, StrokeJoin join = StrokeJoin::Miter) {
            DrawOp op;
            op.type = DrawOpType::DrawPath;
            op.shape = DrawShapeType::Path;
            op.rect = {at.x, at.y, path->bounds.width, path->bounds.height};
            op.path = std::move(path);
            op.color = color;
            op.strokeWidth = strokeWidth;
            op.cap = cap;
            op.join = join;
            return op;
        }

        Image paths(CaseContext& context) {
            std::vector<DrawOp> ops;
            // 非零与奇偶两种填充规则
            auto nonZero = star(45);
            auto evenOddData = *nonZero;
            evenOddData.evenOdd = true;
            ops.push_back(drawPath(nonZero, {20, 20}, 0xfffbbf24u));
            ops.push_back(drawPath(PathRef(evenOddData), {130, 20}, 0xfffbbf24u));
            // 二次、三次曲线与弧
            const auto blob = pathOf({PathVerb::MoveTo, PathVerb::CubicTo, PathVerb::QuadTo, PathVerb::LineTo, PathVerb::Close}, {0, 40, 10, -20, 90, -10, 100, 40, 80, 100, 40, 80, 10, 90});
            ops.push_back(withGradient(drawPath(blob, {250, 20}, 0), linear({0, 0}, {1, 1}, sunset)));
            const auto pie = pathOf({PathVerb::MoveTo, PathVerb::ArcTo, PathVerb::Close}, {45, 45, 0, 0, 90, 90, -30, 270, 0});
            ops.push_back(drawPath(pie, {380, 20}, 0xff34d399u));
            // 描边：三种线接
            const auto zigzag = pathOf({PathVerb::MoveTo, PathVerb::LineTo, PathVerb::LineTo, PathVerb::LineTo}, {0, 50, 30, 0, 60, 50, 90, 0});
            ops.push_back(drawPath(zigzag, {20, 140}, 0xff60a5fau, 12, StrokeCap::Butt, StrokeJoin::Miter));
            ops.push_back(drawPath(zigzag, {140, 140}, 0xff60a5fau, 12, StrokeCap::Round, StrokeJoin::Round));
            ops.push_back(drawPath(zigzag, {260, 140}, 0xff60a5fau, 12, StrokeCap::Square, StrokeJoin::Bevel));
            // 闭合路径描边、细线与曲线描边
            ops.push_back(drawPath(star(40), {380, 130}, 0xfff472b6u, 3, StrokeCap::Butt, StrokeJoin::Miter));
            const auto wave = pathOf({PathVerb::MoveTo, PathVerb::CubicTo, PathVerb::CubicTo}, {0, 30, 30, -20, 60, 80, 90, 30, 120, -20, 150, 80, 180, 30});
            ops.push_back(drawPath(wave, {20, 230}, 0xffffffffu, 1));
            ops.push_back(withGradient(drawPath(wave, {230, 230}, 0, 8, StrokeCap::Round, StrokeJoin::Round), linear({0, 0}, {1, 0}, sunset)));
            const auto ring = pathOf({PathVerb::ArcTo}, {0, 0, 60, 60, 135, 270, 1});
            ops.push_back(drawPath(ring, {440, 240}, 0xffa78bfau, 8, StrokeCap::Round));
            return context.renderOps(std::move(ops));
        }
    }

    namespace {
        Image clipsAndTransforms(CaseContext& context) {
            std::vector<DrawOp> ops;
            const auto stripes = [&](Rect area) {
                for (int i = 0; i < 6; ++i) ops.push_back(fill({area.x - 20 + static_cast<float>(i) * 24, area.y - 20, 12, area.height + 40}, i % 2 ? 0xff38bdf8u : 0xfff472b6u));
            };
            const auto clipped = [&](Rect area, DrawShapeType shape, CornerRadii corners = {}) {
                ops.push_back(pushClip(area, shape, corners));
                ops.push_back(fill(expandedBy(area, 30), 0xff334155u));
                stripes(area);
                ops.push_back(pop(DrawOpType::PopClip));
            };
            // 第一行：矩形、圆角、椭圆、路径裁剪
            clipped({20, 20, 100, 70}, DrawShapeType::Rectangle);
            clipped({140, 20, 100, 70}, DrawShapeType::Rounded, CornerRadii::uniform(24));
            clipped({260, 20, 100, 70}, DrawShapeType::Circle);
            {
                auto clip = pushClip({390, 10, 0, 0}, DrawShapeType::Path);
                clip.path = star(50);
                clip.rect = {390, 10, clip.path->bounds.width, clip.path->bounds.height};
                ops.push_back(clip);
                ops.push_back(fill({380, 0, 130, 120}, 0xff334155u));
                stripes({400, 20, 100, 80});
                ops.push_back(pop(DrawOpType::PopClip));
            }
            // 第二行：嵌套圆角裁剪（走掩码）、裁剪里再旋转、旋转后的矩形裁剪
            ops.push_back(pushClip({20, 120, 120, 80}, DrawShapeType::Rounded, CornerRadii::uniform(30)));
            ops.push_back(fill({20, 120, 120, 80}, 0xff334155u));
            ops.push_back(pushClip({50, 110, 120, 80}, DrawShapeType::Circle));
            stripes({20, 120, 140, 80});
            ops.push_back(pop(DrawOpType::PopClip));
            ops.push_back(pop(DrawOpType::PopClip));

            ops.push_back(pushTransform({180, 120, 100, 80}, 20));
            ops.push_back(pushClip({180, 120, 100, 80}));
            ops.push_back(fill({160, 100, 140, 120}, 0xff334155u));
            stripes({180, 120, 100, 80});
            ops.push_back(pop(DrawOpType::PopClip));
            ops.push_back(pop(DrawOpType::PopTransform));

            // 旋转、缩放、平移与变换原点
            ops.push_back(pushTransform({330, 130, 60, 60}, 45));
            ops.push_back(fill({330, 130, 60, 60}, 0xff22c55eu, DrawShapeType::Rounded, CornerRadii::uniform(10)));
            ops.push_back(pop(DrawOpType::PopTransform));
            ops.push_back(pushTransform({420, 130, 60, 60}, 0, 1.4f, 0.6f));
            ops.push_back(fill({420, 130, 60, 60}, 0xfff59e0bu, DrawShapeType::Circle));
            ops.push_back(pop(DrawOpType::PopTransform));
            // 第三行：绕左上角旋转；嵌套变换；缩放下的描边与线
            ops.push_back(fill({20, 230, 60, 40}, 0x55ffffffu));
            ops.push_back(pushTransform({20, 230, 60, 40}, 15, 1, 1, 0, 0, 0, 0));
            ops.push_back(stroke({20, 230, 60, 40}, 0xffef4444u, 3));
            ops.push_back(pop(DrawOpType::PopTransform));
            ops.push_back(pushTransform({130, 220, 80, 80}, 0, 0.5f, 0.5f, 20, 0));
            ops.push_back(pushTransform({130, 220, 80, 80}, 30));
            ops.push_back(withGradient(fill({130, 220, 80, 80}, 0), linear({0, 0}, {1, 1}, sunset)));
            ops.push_back(drawPath(star(40), {130, 220}, 0xffffffffu, 4));
            ops.push_back(pop(DrawOpType::PopTransform));
            ops.push_back(pop(DrawOpType::PopTransform));
            ops.push_back(pushTransform({260, 230, 100, 50}, -10, 2, 2));
            ops.push_back(stroke({290, 245, 40, 20}, 0xffa78bfau, 2, DrawShapeType::Rounded, CornerRadii::uniform(6)));
            ops.push_back(line({290, 255}, {330, 255}, 0xffffffffu, 1));
            ops.push_back(pop(DrawOpType::PopTransform));
            // 压扁成线的层什么都不画
            ops.push_back(pushTransform({400, 230, 80, 60}, 0, 0, 1));
            ops.push_back(fill({400, 230, 80, 60}, 0xffff0000u));
            ops.push_back(pop(DrawOpType::PopTransform));
            ops.push_back(stroke({400, 230, 80, 60}, 0xff64748bu, 1));
            return context.renderOps(std::move(ops));
        }
    }

    namespace {
        // 程序生成的测试位图：左上到右下的色彩渐变，叠棋盘格和一圈边，方向与裁切一眼可辨
        std::shared_ptr<const BitmapContent> testBitmap(int width, int height) {
            auto bitmap = std::make_shared<BitmapContent>();
            bitmap->width = width;
            bitmap->height = height;
            bitmap->intrinsicSize = Size{static_cast<float>(width), static_cast<float>(height)};
            bitmap->pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    const auto edge = x < 2 || y < 2 || x >= width - 2 || y >= height - 2;
                    const auto dark = ((x / 8) + (y / 8)) % 2 == 0;
                    const auto red = static_cast<std::uint32_t>(40 + 215 * x / std::max(width - 1, 1)), green = static_cast<std::uint32_t>(40 + 215 * y / std::max(height - 1, 1));
                    const auto blue = dark ? 90u : 220u;
                    bitmap->pixels[static_cast<std::size_t>(y * width + x)] = edge ? 0xffffffffu : 0xff000000u | (red << 16) | (green << 8) | blue;
                }
            return bitmap;
        }

        // 不透明度随形状变化的图标：圆形实心、四周透明，用来验着色
        std::shared_ptr<const BitmapContent> testIcon(int size) {
            auto bitmap = std::make_shared<BitmapContent>();
            bitmap->width = bitmap->height = size;
            bitmap->intrinsicSize = Size{static_cast<float>(size), static_cast<float>(size)};
            bitmap->pixels.resize(static_cast<std::size_t>(size) * static_cast<std::size_t>(size));
            const auto center = static_cast<float>(size) * 0.5f;
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x) {
                    const auto distance = std::hypot(static_cast<float>(x) + 0.5f - center, static_cast<float>(y) + 0.5f - center);
                    const auto alpha = static_cast<std::uint32_t>(std::clamp(center - 1.0f - distance, 0.0f, 1.0f) * 255.0f + 0.5f);
                    // 预乘：白色乘以 alpha
                    bitmap->pixels[static_cast<std::size_t>(y * size + x)] = (alpha << 24) | (alpha << 16) | (alpha << 8) | alpha;
                }
            return bitmap;
        }

        DrawOp painter(Rect rect, std::shared_ptr<const PainterContent> content, ContentScale scale = ContentScale::Fit, ImageAlignment alignment = ImageAlignment::Center) {
            DrawOp op;
            op.type = DrawOpType::DrawPainter;
            op.rect = rect;
            op.color = 0xffffffffu;
            op.painter.identity = 1;
            op.painter.generation = 1;
            op.painter.content = std::move(content);
            op.contentScale = scale;
            op.alignment = alignment;
            return op;
        }

        Image images(CaseContext& context) {
            std::vector<DrawOp> ops;
            const auto bitmap = testBitmap(64, 40);
            const auto framed = [&](DrawOp op) {
                ops.push_back(fill(op.rect, 0xff334155u));
                ops.push_back(std::move(op));
            };
            // 第一行：七种内容缩放
            const ContentScale scales[]{ContentScale::Fit, ContentScale::Crop, ContentScale::FillBounds, ContentScale::Inside, ContentScale::Unscaled, ContentScale::FillWidth, ContentScale::FillHeight};
            for (int i = 0; i < 7; ++i) framed(painter({10 + static_cast<float>(i) * 72, 10, 64, 64}, bitmap, scales[i]));
            // 第二行：对齐、不透明度、半像素位置、放大后的线性过滤
            framed(painter({10, 90, 90, 60}, bitmap, ContentScale::Unscaled, ImageAlignment::TopStart));
            framed(painter({110, 90, 90, 60}, bitmap, ContentScale::Unscaled, ImageAlignment::BottomEnd));
            auto faded = painter({210, 90, 64, 40}, bitmap, ContentScale::FillBounds);
            faded.color = 0x80ffffffu;
            framed(faded);
            framed(painter({284.5f, 90.5f, 64, 40}, bitmap, ContentScale::FillBounds));
            framed(painter({360, 90, 150, 60}, testBitmap(20, 8), ContentScale::FillBounds));
            // 第三行：平铺、九宫格、着色、filmstrip 选帧
            auto tiled = painter({10, 170, 150, 70}, testBitmap(24, 24));
            tiled.repeat = ImageRepeat::Tile;
            framed(tiled);
            auto nine = painter({170, 170, 150, 70}, testBitmap(24, 24));
            nine.repeat = ImageRepeat::NinePatch;
            nine.slices = {8, 8, 8, 8};
            framed(nine);
            auto tinted = painter({330, 170, 60, 60}, testIcon(48), ContentScale::FillBounds);
            tinted.hasTint = true;
            tinted.color = 0xfffbbf24u;
            framed(tinted);
            framed(painter({400, 170, 60, 60}, testIcon(48), ContentScale::FillBounds));
            for (std::uint32_t frame = 0; frame < 4; ++frame) {
                auto strip = painter({10 + static_cast<float>(frame) * 50, 255, 40, 50}, testBitmap(64, 20), ContentScale::FillBounds);
                strip.painter.frames = 4;
                strip.painter.frame = frame;
                strip.painter.horizontal = true;
                framed(strip);
            }
            // 像素缓冲：非预乘 ARGB，带渐变的 Alpha；旋转后的图片
            auto buffer = std::make_shared<PixelBufferContent>();
            buffer->width = 32, buffer->height = 32;
            buffer->intrinsicSize = Size{32, 32};
            buffer->pixels.resize(32 * 32);
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 32; ++x) buffer->pixels[static_cast<std::size_t>(y * 32 + x)] = (static_cast<std::uint32_t>(x * 255 / 31) << 24) | 0x00ff4040u;
            framed(painter({230, 255, 60, 50}, buffer, ContentScale::FillBounds));
            ops.push_back(pushTransform({330, 250, 64, 40}, 20));
            ops.push_back(painter({330, 250, 64, 40}, bitmap, ContentScale::FillBounds));
            ops.push_back(pop(DrawOpType::PopTransform));
            return context.renderOps(std::move(ops));
        }
    }

    namespace {
        DrawOp styled(DrawOp op, int weight, bool italic, std::vector<std::string> family = {test_support::testFontFamily}) {
            op.textStyle.fontWeight = weight;
            op.textStyle.italic = italic;
            op.textStyle.fontFamily = std::move(family);
            return op;
        }

        Image text(CaseContext& context) {
            // 只有常规体的族，用来验伪粗体和伪斜体
            FontFamilyDefinition regularOnly;
            regularOnly.name = "Canton Test Regular Only";
            regularOnly.faces.push_back({std::make_shared<const std::vector<std::uint8_t>>(test_support::readFile(context.sources / "tests/fixtures/fonts/CantonTestSans-Regular.ttf")), std::nullopt, std::nullopt});
            context.fonts.define(&context, std::move(regularOnly));
            const std::vector<std::string> synthetic{"Canton Test Regular Only"};

            std::vector<DrawOp> ops;
            // 字号与真实的三个字面
            ops.push_back(label({16, 12, 300, 20}, "Canton 0123456789", 0xffffffffu, 16));
            ops.push_back(styled(label({16, 36, 300, 20}, "Bold face 700", 0xfffbbf24u, 16), 700, false));
            ops.push_back(styled(label({16, 60, 300, 20}, "Italic face", 0xff34d399u, 16), 400, true));
            ops.push_back(label({330, 12, 180, 40}, "Big 28", 0xff60a5fau, 28));
            ops.push_back(label({330, 56, 180, 12}, "tiny text at 9", 0xffcbd5e1u, 9));
            // 伪粗体、伪斜体，以及关闭伪造
            ops.push_back(styled(label({16, 92, 240, 20}, "Synthetic bold", 0xffffffffu, 16), 700, false, synthetic));
            ops.push_back(styled(label({16, 116, 240, 20}, "Synthetic italic", 0xffffffffu, 16), 400, true, synthetic));
            auto plain = styled(label({16, 140, 240, 20}, "Synthesis off", 0xffffffffu, 16), 700, true, synthetic);
            plain.textStyle.fontSynthesis = false;
            ops.push_back(plain);
            // 换行、最大行数与省略
            const auto boxed = [&](DrawOp op) {
                ops.push_back(stroke(expandedBy(op.rect, 2), 0xff475569u, 1));
                ops.push_back(std::move(op));
            };
            auto wrapped = label({280, 92, 110, 60}, "Wrapped text breaks at word boundaries", 0xffe5e7ebu, 12);
            wrapped.maxLines = 0;
            boxed(wrapped);
            auto limited = label({404, 92, 100, 30}, "Two lines then an ellipsis shows up here", 0xffe5e7ebu, 12);
            limited.maxLines = 2;
            limited.overflow = TextOverflow::Ellipsis;
            boxed(limited);
            auto single = label({404, 136, 100, 16}, "Single line ellipsis", 0xffe5e7ebu, 12);
            single.overflow = TextOverflow::Ellipsis;
            boxed(single);
            // 三种对齐、溢出裁剪与溢出可见
            for (int i = 0; i < 3; ++i) {
                auto aligned = label({16, 176 + static_cast<float>(i) * 22, 200, 16}, i == 0 ? "start" : i == 1 ? "center" : "end", 0xffe5e7ebu, 14);
                aligned.textAlign = i == 0 ? TextAlign::Start : i == 1 ? TextAlign::Center : TextAlign::End;
                boxed(aligned);
            }
            auto clippedText = label({240, 176, 80, 16}, "Clipped overflow text", 0xfffca5a5u, 14);
            clippedText.overflow = TextOverflow::Clip;
            boxed(clippedText);
            auto visible = label({240, 204, 80, 16}, "Visible overflow text", 0xff86efacu, 14);
            visible.overflow = TextOverflow::Visible;
            boxed(visible);
            // 行高、半透明、旋转与放大
            auto tall = label({16, 250, 200, 60}, "Line height 30 over two lines here", 0xffe5e7ebu, 12);
            tall.maxLines = 0;
            tall.textStyle.lineHeight = 30;
            tall.rect.width = 130;
            boxed(tall);
            ops.push_back(label({170, 260, 150, 20}, "Half alpha", 0x80ffffffu, 16));
            ops.push_back(pushTransform({340, 250, 80, 20}, -20));
            ops.push_back(label({340, 250, 120, 20}, "Rotated", 0xffffffffu, 16));
            ops.push_back(pop(DrawOpType::PopTransform));
            ops.push_back(pushTransform({420, 270, 40, 12}, 0, 2.5f, 2.5f));
            ops.push_back(label({420, 270, 60, 12}, "x2.5", 0xfffbbf24u, 10));
            ops.push_back(pop(DrawOpType::PopTransform));
            auto image = context.renderOps(std::move(ops));
            context.fonts.removeOwned(&context);
            return image;
        }

        // 设备丢失自动恢复：绘制中途 DEVICE_REMOVED、窗口呈现 DEVICE_RESET（含交换链重建）、连续故障上限与恢复后逐像素一致
        Image deviceRebuild(CaseContext& context) {
            std::vector<DrawOp> ops;
            ops.push_back(withGradient(fill({20, 20, 200, 100}, 0, DrawShapeType::Rounded, CornerRadii::uniform(18)), linear({0, 0}, {1, 1}, sunset)));
            ops.push_back(painter({240, 20, 120, 100}, testBitmap(48, 40), ContentScale::Crop));
            ops.push_back(drawPath(star(50), {380, 20}, 0xfffbbf24u));
            ops.push_back(pushClip({20, 140, 200, 60}, DrawShapeType::Rounded, CornerRadii::uniform(30)));
            ops.push_back(pushClip({60, 130, 200, 80}, DrawShapeType::Circle));
            ops.push_back(fill({0, 120, 300, 100}, 0xff38bdf8u));
            ops.push_back(pop(DrawOpType::PopClip));
            ops.push_back(pop(DrawOpType::PopClip));
            ops.push_back(label({240, 150, 260, 30}, "Device rebuilt", 0xffffffffu, 24));
            for (auto& op : ops) DrawOpsBuilder::prepareText(op, context.text);
            PublishedFrameContent content;
            content.scenePaint = test_support::fragmentOf(std::move(ops));
            content.viewport = {static_cast<float>(context.width), static_cast<float>(context.height)};

            auto offscreen = context.renderer.createOffscreenTarget(context.width, context.height);
            context.renderer.render(*offscreen, {&content});
            const Image before{context.width, context.height, context.renderer.readPixels(*offscreen)};

            // 1. 同一离屏目标在绘制中途遭遇 DEVICE_REMOVED：单次 render 内自动清理半帧状态、重建设备与图集并重画
            auto rebuilds = context.renderer.counters().deviceRebuilds;
            context.renderer.simulateDeviceLoss(DXGI_ERROR_DEVICE_REMOVED);
            context.renderer.render(*offscreen, {&content});
            if (context.renderer.counters().deviceRebuilds != rebuilds + 1) throw std::runtime_error("DEVICE_REMOVED 没有触发自动重建");
            if (test_support::compareImages(before, {context.width, context.height, context.renderer.readPixels(*offscreen)}, 0).exceeding != 0) throw std::runtime_error("DEVICE_REMOVED 自动恢复后的画面不一致");

            // 2. 窗口目标在 Present 阶段遭遇 DEVICE_RESET：自动重建设备、DXGI 交换链与 DirectComposition 并再次呈现
            struct HiddenWindow {
                HWND handle = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 128, 96, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                ~HiddenWindow() {
                    if (handle) DestroyWindow(handle);
                }
            } window;
            if (!window.handle) throw std::runtime_error("无法创建测试窗口");
            auto windowTarget = context.renderer.createWindowTarget(window.handle);
            context.renderer.resize(*windowTarget, 128, 96);
            context.renderer.render(*windowTarget, {&content});
            rebuilds = context.renderer.counters().deviceRebuilds;
            context.renderer.simulateDeviceLoss(DXGI_ERROR_DEVICE_RESET, 1, true);
            context.renderer.render(*windowTarget, {&content});
            if (context.renderer.counters().deviceRebuilds != rebuilds + 1) throw std::runtime_error("窗口交换链没有随 DEVICE_RESET 重建并呈现");

            // 3. 连续丢失超过重试上限必须上报异常，不死循环；故障解除后同一目标继续恢复一致画面
            rebuilds = context.renderer.counters().deviceRebuilds;
            context.renderer.simulateDeviceLoss(DXGI_ERROR_DEVICE_REMOVED, 3);
            bool propagated = false;
            try {
                context.renderer.render(*offscreen, {&content});
            } catch (const std::runtime_error&) {
                propagated = true;
            }
            if (!propagated || context.renderer.counters().deviceRebuilds != rebuilds + 2) throw std::runtime_error("连续设备丢失的重试上限行为不符");

            context.renderer.render(*offscreen, {&content});
            const Image after{context.width, context.height, context.renderer.readPixels(*offscreen)};
            if (test_support::compareImages(before, after, 0).exceeding != 0) throw std::runtime_error("故障解除后恢复的画面与重建前不一致");
            return after;
        }
    }

    namespace {
        // SVG：裁剪路径、复用、蒙版、按外包盒取比例的渐变，以及两个方向不同的缩放
        constexpr std::string_view svgFeatures = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="96" height="96" viewBox="0 0 96 96">
  <defs>
    <clipPath id="disc"><circle cx="48" cy="48" r="40"/></clipPath>
    <linearGradient id="fade" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#ffffff"/>
      <stop offset="1" stop-color="#000000"/>
    </linearGradient>
    <mask id="ramp"><rect x="0" y="0" width="96" height="96" fill="url(#fade)"/></mask>
    <g id="dot"><circle r="7" fill="#fbbf24" stroke="#0f172a" stroke-width="2"/></g>
  </defs>
  <g clip-path="url(#disc)">
    <rect width="96" height="96" fill="#1e293b"/>
    <rect x="0" y="0" width="96" height="16" fill="#38bdf8"/>
    <rect x="0" y="32" width="96" height="16" fill="#f472b6"/>
    <rect x="0" y="64" width="96" height="16" fill="#34d399"/>
  </g>
  <rect x="8" y="78" width="80" height="12" fill="#a78bfa" mask="url(#ramp)"/>
  <use xlink:href="#dot" x="24" y="24"/>
  <use href="#dot" x="72" y="24"/>
</svg>)SVG";

        Image svg(CaseContext& context) {
            std::vector<DrawOp> ops;
            const auto content = canton::runtime::decodeSvgResource(svgFeatures, "features.svg");
            const auto framed = [&](Rect rect, ContentScale scale, std::uint64_t identity) {
                ops.push_back(fill(rect, 0xff334155u));
                auto op = painter(rect, content, scale);
                op.painter.identity = identity;
                ops.push_back(std::move(op));
            };
            framed({10, 10, 96, 96}, ContentScale::Unscaled, 101);
            framed({120, 10, 192, 192}, ContentScale::Fit, 102);
            framed({326, 10, 184, 96}, ContentScale::FillBounds, 103);
            framed({326, 120, 48, 82}, ContentScale::FillBounds, 104);
            return context.renderOps(std::move(ops));
        }
        Image shaderBrushCase(CaseContext& context) {
            canton::d3d11::D3DShaderCompiler compiler;
            const std::string hlsl =
                "#include <canton.hlsl>\n"
                "cbuffer Params : register(b0) { float4 c1; float4 c2; float freq; };\n"
                "float4 main(CantonInput input) : SV_Target {\n"
                "    float wave = 0.5 + 0.5 * sin(input.uv.x * freq + input.uv.y * 5.0);\n"
                "    return lerp(c1, c2, wave);\n"
                "}\n";
            const auto compiled = compiler.compileHlsl(hlsl, "main", "regression_brush.hlsl");
            auto program = std::make_shared<RuntimeEffectProgram>();
            program->identity = 9001;
            program->revision = 1;
            program->debugName = "regression_brush.hlsl";
            program->entryPoint = "main";
            program->dxbc = compiled.dxbc;
            program->reflection = compiled.reflection;

            auto makeShader = [&](std::uint32_t c1, std::uint32_t c2, float freq) {
                RuntimeShaderData data;
                data.program = program;
                data.version = 1;
                std::vector<ShaderUniformInput> uniforms{
                    {"c1", ShaderUniformInputKind::Color, {}, {}, c1},
                    {"c2", ShaderUniformInputKind::Color, {}, {}, c2},
                    {"freq", ShaderUniformInputKind::Float, {freq}, {}, 0},
                };
                data.packedBuffers = packRuntimeShaderUniforms(program->reflection, uniforms);
                return RuntimeShaderRef(std::move(data));
            };

            const auto brushA = makeShader(0xff0ea5e9u, 0xfff43f5eu, 9.0f);
            const auto brushB = makeShader(0xfffbbf24u, 0xff22c55eu, 14.0f);

            std::vector<DrawOp> ops;
            // 圆角矩形背景
            auto bg = fill({16, 16, 140, 96}, 0xffffffffu, DrawShapeType::Rounded, CornerRadii::uniform(18));
            bg.shader = brushA;
            ops.push_back(bg);

            // 粗边框描边
            auto br = border({176, 16, 140, 96}, 0xffffffffu, 8.0f, DrawShapeType::Rounded, {24, 6, 24, 6});
            br.shader = brushB;
            ops.push_back(br);

            // 粗线段
            auto ln = line({340, 26}, {496, 102}, 0xffffffffu, 14.0f, StrokeCap::Round);
            ln.shader = brushA;
            ops.push_back(ln);

            // 自定义五角星 Path 填充 + 描边
            PathData star;
            constexpr float cx = 110.0f, cy = 200.0f;
            for (int i = 0; i < 10; ++i) {
                const float angle = -1.5707963f + static_cast<float>(i) * 0.62831853f;
                const float r = (i % 2 == 0) ? 58.0f : 26.0f;
                star.verbs.push_back(i == 0 ? PathVerb::MoveTo : PathVerb::LineTo);
                star.values.push_back(cx + std::cos(angle) * r);
                star.values.push_back(cy + std::sin(angle) * r);
            }
            star.verbs.push_back(PathVerb::Close);
            star.bounds = {52, 142, 116, 116};
            const PathRef starRef(std::move(star));

            DrawOp starFill;
            starFill.type = DrawOpType::DrawPath;
            starFill.shape = DrawShapeType::Path;
            starFill.rect = {52, 142, 116, 116};
            starFill.path = starRef;
            starFill.color = 0xffffffffu;
            starFill.shader = brushB;
            ops.push_back(starFill);

            DrawOp starStroke = starFill;
            starStroke.rect = {240, 142, 116, 116};
            starStroke.strokeWidth = 6.0f;
            starStroke.join = StrokeJoin::Round;
            starStroke.shader = brushA;
            ops.push_back(starStroke);

            return context.renderOps(std::move(ops));
        }

        Image renderEffectsCase(CaseContext& context) {
            canton::d3d11::D3DShaderCompiler compiler;
            const std::string waveHlsl =
                "#include <canton.hlsl>\n"
                "Texture2D content : register(t0);\n"
                "float4 main(CantonInput input) : SV_Target {\n"
                "    float2 uv = input.uv;\n"
                "    uv.x += sin(uv.y * 18.0) * 0.04;\n"
                "    return content.SampleLevel(cantonLinearClamp, uv, 0);\n"
                "}\n";
            const auto compiled = compiler.compileHlsl(waveHlsl, "main", "regression_wave.hlsl");
            auto program = std::make_shared<RuntimeEffectProgram>();
            program->identity = 9002;
            program->revision = 1;
            program->debugName = "regression_wave.hlsl";
            program->entryPoint = "main";
            program->dxbc = compiled.dxbc;
            program->reflection = compiled.reflection;

            auto root = std::make_shared<PaintFragment>();
            root->bounds = {{0, 0, 520, 320}, true, false};

            auto makeCard = [&](float x, float y, RenderEffectRef effect, bool clipCard) {
                auto frag = std::make_shared<PaintFragment>();
                auto ops = std::make_shared<std::vector<DrawOp>>();
                ops->push_back(fill({0, 0, 96, 96}, 0xff1e293bu));
                ops->push_back(fill({0, 0, 48, 48}, 0xfff43f5eu));
                ops->push_back(fill({48, 48, 48, 48}, 0xff38bdf8u));
                ops->push_back(fill({22, 22, 52, 52}, 0xfffbbf24u, DrawShapeType::Circle));
                frag->content = ops;

                auto layer = std::make_shared<PaintLayerFragment>();
                layer->bounds = {0, 0, 96, 96};
                layer->useOffscreenGroup = true;
                layer->groupAlpha = 1.0f;
                layer->groupRenderEffect = std::move(effect);
                if (clipCard) {
                    TransformModifierSemantics t;
                    t.clip = true;
                    layer->value = t;
                    layer->before.push_back(pushClip({0, 0, 96, 96}, DrawShapeType::Rounded, CornerRadii::uniform(12)));
                    layer->after.push_back(pop(DrawOpType::PopClip));
                }
                frag->layer = layer;
                frag->bounds = {{-40, -40, 176, 176}, true, false};
                root->children.push_back({frag, {x, y}});
            };

            // 顶排：blur 四种 TileMode（Clamp / Repeated / Mirror / Decal）
            const TileMode modes[4]{TileMode::Clamp, TileMode::Repeated, TileMode::Mirror, TileMode::Decal};
            for (int i = 0; i < 4; ++i) {
                RenderEffectData re;
                RenderEffectStep step;
                step.kind = RenderEffectStepKind::Blur;
                step.radiusX = 10.0f;
                step.radiusY = 10.0f;
                step.edgeTreatment = modes[i];
                re.steps.push_back(step);
                makeCard(18.0f + static_cast<float>(i) * 124.0f, 20.0f, RenderEffectRef(std::move(re)), modes[i] != TileMode::Decal);
            }

            // 底排：ColorFilter(Tint) / ColorFilter(ColorMatrix) / RenderEffect.shader / RenderEffect.chain
            {
                RenderEffectData re;
                RenderEffectStep s;
                s.kind = RenderEffectStepKind::ColorFilter;
                s.colorFilter.kind = ColorFilterKind::Tint;
                s.colorFilter.color = 0xaa22c55eu;
                s.colorFilter.blendMode = BlendMode::SrcAtop;
                re.steps.push_back(s);
                makeCard(18.0f, 168.0f, RenderEffectRef(std::move(re)), true);
            }
            {
                RenderEffectData re;
                RenderEffectStep s;
                s.kind = RenderEffectStepKind::ColorFilter;
                s.colorFilter.kind = ColorFilterKind::ColorMatrix;
                s.colorFilter.matrix = {0.213f, 0.715f, 0.072f, 0, 15, 0.213f, 0.715f, 0.072f, 0, 15, 0.213f, 0.715f, 0.072f, 0, 15, 0, 0, 0, 1, 0};
                re.steps.push_back(s);
                makeCard(142.0f, 168.0f, RenderEffectRef(std::move(re)), true);
            }
            {
                RenderEffectData re;
                RenderEffectStep s;
                s.kind = RenderEffectStepKind::Shader;
                RuntimeShaderData sd;
                sd.program = program;
                sd.version = 1;
                s.shader = RuntimeShaderRef(std::move(sd));
                s.uniformShaderName = "content";
                re.steps.push_back(s);
                makeCard(266.0f, 168.0f, RenderEffectRef(std::move(re)), true);
            }
            {
                RenderEffectData re;
                RenderEffectStep blurStep;
                blurStep.kind = RenderEffectStepKind::Blur;
                blurStep.radiusX = 6.0f;
                blurStep.radiusY = 6.0f;
                blurStep.edgeTreatment = TileMode::Clamp;
                RenderEffectStep lightStep;
                lightStep.kind = RenderEffectStepKind::ColorFilter;
                lightStep.colorFilter.kind = ColorFilterKind::Lighting;
                lightStep.colorFilter.multiply = 0xffffc080u;
                lightStep.colorFilter.add = 0x00101830u;
                re.steps.push_back(blurStep);
                re.steps.push_back(lightStep);
                makeCard(390.0f, 168.0f, RenderEffectRef(std::move(re)), true);
            }

            PublishedFrameContent content;
            content.scenePaint = {root, {0, 0}};
            return context.render(content);
        }

        Image blendModesCase(CaseContext& context) {
            auto root = std::make_shared<PaintFragment>();
            root->bounds = {{0, 0, 520, 288}, true, false};
            const BlendMode modes[8]{
                BlendMode::Multiply, BlendMode::Screen, BlendMode::Overlay, BlendMode::Plus,
                BlendMode::Difference, BlendMode::Hue, BlendMode::DstIn, BlendMode::Clear,
            };
            for (int i = 0; i < 8; ++i) {
                const float x = 16.0f + static_cast<float>(i % 4) * 124.0f;
                const float y = 16.0f + static_cast<float>(i / 4) * 136.0f;

                auto bg = std::make_shared<PaintFragment>();
                auto bgOps = std::make_shared<std::vector<DrawOp>>();
                bgOps->push_back(fill({0, 0, 110, 120}, 0xff1e293bu, DrawShapeType::Rounded, CornerRadii::uniform(12)));
                bg->content = bgOps;
                bg->bounds = {{0, 0, 110, 120}, true, false};
                root->children.push_back({bg, {x, y}});

                // 将两圆混合放进独立离屏组（尤其 DstIn / Clear 仅作用于两圆自身，不穿透卡片背景）
                auto fg = std::make_shared<PaintFragment>();
                auto fgOps = std::make_shared<std::vector<DrawOp>>();
                fgOps->push_back(fill({12, 20, 62, 62}, 0xff38bdf8u, DrawShapeType::Circle));
                auto second = fill({36, 36, 62, 62}, 0xfff472b6u, DrawShapeType::Circle);
                second.blendMode = modes[i];
                fgOps->push_back(second);
                fg->content = fgOps;
                fg->bounds = {{0, 0, 110, 120}, true, false};
                if (modes[i] == BlendMode::DstIn || modes[i] == BlendMode::Clear) {
                    auto layer = std::make_shared<PaintLayerFragment>();
                    layer->bounds = {0, 0, 110, 120};
                    layer->useOffscreenGroup = true;
                    layer->groupAlpha = 1.0f;
                    fg->layer = layer;
                }
                root->children.push_back({fg, {x, y}});
            }
            PublishedFrameContent content;
            content.scenePaint = {root, {0, 0}};
            return context.render(content);
        }

        Image groupOpacityAndLayersCase(CaseContext& context) {
            auto root = std::make_shared<PaintFragment>();
            root->bounds = {{0, 0, 520, 260}, true, false};

            auto makeOpacityCard = [&](float x, bool offscreen) {
                auto frag = std::make_shared<PaintFragment>();
                auto ops = std::make_shared<std::vector<DrawOp>>();
                ops->push_back(fill({0, 0, 140, 140}, 0xff1e293bu, DrawShapeType::Rounded, CornerRadii::uniform(14)));
                ops->push_back(fill({14, 24, 72, 72}, 0xffef4444u, DrawShapeType::Circle));
                ops->push_back(fill({54, 44, 72, 72}, 0xff3b82f6u, DrawShapeType::Circle));
                frag->content = ops;
                auto layer = std::make_shared<PaintLayerFragment>();
                layer->bounds = {0, 0, 140, 140};
                layer->useOffscreenGroup = offscreen;
                layer->groupAlpha = 0.5f;
                layer->contentAlpha = offscreen ? 1.0f : 0.5f;
                frag->layer = layer;
                frag->bounds = {{0, 0, 140, 140}, true, false};
                root->children.push_back({frag, {x, 20}});
            };
            makeOpacityCard(18.0f, true);
            makeOpacityCard(176.0f, false);

            // 右侧：RenderLayer 录制复用 + DstIn 渐变遮罩倒影
            auto reflectFrag = std::make_shared<PaintFragment>();
            auto ops = std::make_shared<std::vector<DrawOp>>();

            DrawOp beginCard;
            beginCard.type = DrawOpType::BeginRecordLayer;
            beginCard.layerHandle = {77, 1};
            beginCard.rect = {0, 0, 140, 72};
            ops->push_back(beginCard);
            ops->push_back(withGradient(fill({0, 0, 140, 72}, 0xffffffffu, DrawShapeType::Rounded, CornerRadii::uniform(14)), linear({0, 0}, {1, 1}, {{0, 0xff38bdf8u}, {1, 0xffa855f7u}})));
            ops->push_back(fill({18, 16, 40, 40}, 0xfffbbf24u, DrawShapeType::Circle));
            ops->push_back(fill({70, 22, 54, 10}, 0xffffffffu, DrawShapeType::Rounded, CornerRadii::uniform(4)));
            ops->push_back(fill({70, 40, 38, 8}, 0xbbffffffu, DrawShapeType::Rounded, CornerRadii::uniform(4)));
            DrawOp endRec;
            endRec.type = DrawOpType::EndRecordLayer;
            ops->push_back(endRec);

            // 绘制原层
            DrawOp drawCard;
            drawCard.type = DrawOpType::DrawLayer;
            RenderLayerSnapshot cardSnap;
            cardSnap.handle = {77, 1};
            cardSnap.topLeftX = 350;
            cardSnap.topLeftY = 20;
            drawCard.layerSnapshot = RenderLayerSnapshotRef(cardSnap);
            ops->push_back(drawCard);

            // 录制倒影层（上下翻转 + DstIn 渐变遮罩）
            DrawOp beginRefl;
            beginRefl.type = DrawOpType::BeginRecordLayer;
            beginRefl.layerHandle = {78, 1};
            beginRefl.rect = {0, 0, 140, 72};
            ops->push_back(beginRefl);
            ops->push_back(pushTransform({0, 0, 140, 72}, 0, 1, -1));
            DrawOp drawFlipped;
            drawFlipped.type = DrawOpType::DrawLayer;
            RenderLayerSnapshot flipSnap;
            flipSnap.handle = {77, 1};
            drawFlipped.layerSnapshot = RenderLayerSnapshotRef(flipSnap);
            ops->push_back(drawFlipped);
            ops->push_back(pop(DrawOpType::PopTransform));
            auto maskOp = withGradient(fill({0, 0, 140, 72}, 0xffffffffu), linear({0, 0}, {0, 1}, {{0, 0xffffffffu}, {1, 0x00ffffffu}}));
            maskOp.blendMode = BlendMode::DstIn;
            ops->push_back(maskOp);
            ops->push_back(endRec);

            DrawOp drawRefl;
            drawRefl.type = DrawOpType::DrawLayer;
            RenderLayerSnapshot reflSnap;
            reflSnap.handle = {78, 1};
            reflSnap.alpha = 0.7f;
            reflSnap.topLeftX = 350;
            reflSnap.topLeftY = 100;
            drawRefl.layerSnapshot = RenderLayerSnapshotRef(reflSnap);
            ops->push_back(drawRefl);

            reflectFrag->content = ops;
            reflectFrag->bounds = {{0, 0, 520, 260}, false, false};
            root->children.push_back({reflectFrag, {0, 0}});

            PublishedFrameContent content;
            content.scenePaint = {root, {0, 0}};
            return context.render(content);
        }
    }

    void addDrawCases(std::vector<Case>& cases) {
        cases.push_back({"shapes", 520, 370, shapes});
        cases.push_back({"gradients", 520, 320, gradients});
        cases.push_back({"shadows", 520, 320, shadows});
        cases.push_back({"paths", 520, 320, paths});
        cases.push_back({"clips-and-transforms", 520, 320, clipsAndTransforms});
        cases.push_back({"images", 520, 320, images});
        cases.push_back({"svg", 520, 220, svg});
        cases.push_back({"text", 520, 320, text});
        cases.push_back({"device-rebuild", 520, 220, deviceRebuild});
        cases.push_back({"shader-brush", 520, 280, shaderBrushCase});
        cases.push_back({"render-effects", 520, 300, renderEffectsCase});
        cases.push_back({"blend-modes", 520, 288, blendModesCase});
        cases.push_back({"group-opacity-and-layers", 520, 220, groupOpacityAndLayersCase});
    }
}
