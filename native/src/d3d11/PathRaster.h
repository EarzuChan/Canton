#pragma once

#include <core/Affine.h>
#include <core/Brush.h>
#include <core/Geometry.h>

#include <cstdint>
#include <vector>

namespace canton::d3d11 {
    struct Polyline {
        std::vector<core::Point> points;
        bool closed = false;
    };

    // 整数像素范围，右下不含
    struct PixelRect {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;

        [[nodiscard]] int width() const noexcept {
            return right - left;
        }

        [[nodiscard]] int height() const noexcept {
            return bottom - top;
        }

        [[nodiscard]] bool empty() const noexcept {
            return right <= left || bottom <= top;
        }

        bool operator==(const PixelRect&) const = default;
    };

    [[nodiscard]] PixelRect intersect(const PixelRect& a, const PixelRect& b) noexcept;

    // 设备空间里的 8 位覆盖率，bounds 是它在渲染目标上的位置
    struct CoverageMask {
        PixelRect bounds;
        std::vector<std::uint8_t> pixels;
    };

    struct StrokeStyle {
        float width = 1;
        core::StrokeCap cap = core::StrokeCap::Butt;
        core::StrokeJoin join = core::StrokeJoin::Miter;
        // 尖角长度与线宽之比超过它时改用斜接
        float miterLimit = 4;
    };

    // 把路径展平成折线。坐标先加 offset，曲线按 scale（局部到设备的放大倍数）决定细分段数，输出仍是局部坐标
    [[nodiscard]] std::vector<Polyline> flattenPath(const core::PathData& path, core::Point offset, float scale);

    // 描边轮廓：每条折线产出若干闭合多边形，按非零规则填充即为描边区域
    [[nodiscard]] std::vector<Polyline> strokePolylines(const std::vector<Polyline>& lines, const StrokeStyle& style, float scale);

    void transformPolylines(std::vector<Polyline>& lines, const core::Affine& transform);

    // 精确面积覆盖率光栅化。输出范围是多边形外包盒与 clip 的交集
    [[nodiscard]] CoverageMask rasterizePolygons(const std::vector<Polyline>& polygons, bool evenOdd, const PixelRect& clip);
}
