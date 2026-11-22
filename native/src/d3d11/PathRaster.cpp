#include <d3d11/PathRaster.h>

#include <algorithm>
#include <cmath>

namespace canton::d3d11 {
    namespace {
        using core::Point;
        constexpr float pi = 3.14159265358979323846f;
        // 曲线展平允许的最大偏差，设备像素
        constexpr float flattenTolerance = 0.2f;

        Point operator+(Point a, Point b) {
            return {a.x + b.x, a.y + b.y};
        }

        Point operator-(Point a, Point b) {
            return {a.x - b.x, a.y - b.y};
        }

        Point operator*(Point a, float s) {
            return {a.x * s, a.y * s};
        }

        float length(Point a) {
            return std::sqrt(a.x * a.x + a.y * a.y);
        }

        float cross(Point a, Point b) {
            return a.x * b.y - a.y * b.x;
        }

        float dot(Point a, Point b) {
            return a.x * b.x + a.y * b.y;
        }

        int segmentsFor(float deviation, float factor, float scale) {
            const auto count = std::ceil(std::sqrt(factor * deviation * scale / flattenTolerance));
            return std::isfinite(count) ? std::clamp(static_cast<int>(count), 1, 512) : 1;
        }

        int arcSegments(float radius, float sweep, float scale) {
            const auto device = std::max(radius * scale, flattenTolerance);
            const auto step = 2.0f * std::acos(std::clamp(1.0f - flattenTolerance / device, -1.0f, 1.0f));
            const auto count = std::ceil(std::abs(sweep) / std::max(step, 1e-3f));
            return std::isfinite(count) ? std::clamp(static_cast<int>(count), 1, 1024) : 1;
        }

        void push(Polyline& line, Point point) {
            if (line.points.empty() || line.points.back() != point) line.points.push_back(point);
        }
    }

    PixelRect intersect(const PixelRect& a, const PixelRect& b) noexcept {
        PixelRect result{std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
        if (result.right < result.left) result.right = result.left;
        if (result.bottom < result.top) result.bottom = result.top;
        return result;
    }

    std::vector<Polyline> flattenPath(const core::PathData& path, Point offset, float scale) {
        std::vector<Polyline> result;
        Polyline current;
        Point start{}, pen{};
        bool open = false;
        const auto finish = [&](bool closed) {
            if (current.points.size() > 1) {
                if (closed && current.points.front() == current.points.back()) current.points.pop_back();
                current.closed = closed;
                result.push_back(std::move(current));
            }
            current = {};
            open = false;
        };
        const auto begin = [&](Point point) {
            if (open) finish(false);
            start = pen = point;
            current.points.push_back(point);
            open = true;
        };
        const auto ensure = [&] {
            if (!open) begin(pen);
        };
        std::size_t index = 0;
        const auto value = [&](std::size_t at) {
            return index + at < path.values.size() ? path.values[index + at] : 0.0f;
        };
        for (const auto verb : path.verbs) {
            switch (verb) {
                case core::PathVerb::MoveTo:
                    begin({value(0) + offset.x, value(1) + offset.y});
                    break;
                case core::PathVerb::LineTo:
                    ensure();
                    pen = {value(0) + offset.x, value(1) + offset.y};
                    push(current, pen);
                    break;
                case core::PathVerb::QuadTo: {
                    ensure();
                    const Point c{value(0) + offset.x, value(1) + offset.y}, end{value(2) + offset.x, value(3) + offset.y};
                    const auto count = segmentsFor(length(pen - c * 2.0f + end), 0.25f, scale);
                    for (int i = 1; i <= count; ++i) {
                        const auto t = static_cast<float>(i) / static_cast<float>(count), u = 1.0f - t;
                        push(current, pen * (u * u) + c * (2 * u * t) + end * (t * t));
                    }
                    pen = end;
                    break;
                }
                case core::PathVerb::CubicTo: {
                    ensure();
                    const Point c1{value(0) + offset.x, value(1) + offset.y}, c2{value(2) + offset.x, value(3) + offset.y}, end{value(4) + offset.x, value(5) + offset.y};
                    const auto count = segmentsFor(std::max(length(pen - c1 * 2.0f + c2), length(c1 - c2 * 2.0f + end)), 0.75f, scale);
                    for (int i = 1; i <= count; ++i) {
                        const auto t = static_cast<float>(i) / static_cast<float>(count), u = 1.0f - t;
                        push(current, pen * (u * u * u) + c1 * (3 * u * u * t) + c2 * (3 * u * t * t) + end * (t * t * t));
                    }
                    pen = end;
                    break;
                }
                case core::PathVerb::ArcTo: {
                    // 角度从三点钟方向起算，顺时针为正
                    const auto rx = value(2) * 0.5f, ry = value(3) * 0.5f;
                    const Point center{value(0) + offset.x + rx, value(1) + offset.y + ry};
                    const auto from = value(4) * pi / 180.0f;
                    const auto sweep = std::clamp(value(5), -360.0f, 360.0f) * pi / 180.0f;
                    const auto at = [&](float angle) {
                        return Point{center.x + rx * std::cos(angle), center.y + ry * std::sin(angle)};
                    };
                    if (value(6) != 0 || !open)
                        begin(at(from));
                    else
                        push(current, at(from));
                    const auto count = arcSegments(std::max(std::abs(rx), std::abs(ry)), sweep, scale);
                    for (int i = 1; i <= count; ++i) push(current, at(from + sweep * static_cast<float>(i) / static_cast<float>(count)));
                    pen = current.points.back();
                    break;
                }
                case core::PathVerb::Close:
                    if (open) {
                        finish(true);
                        pen = start;
                    }
                    break;
            }
            index += static_cast<std::size_t>(core::pathVerbArity(verb));
        }
        if (open) finish(false);
        return result;
    }

    void transformPolylines(std::vector<Polyline>& lines, const core::Affine& t) {
        for (auto& line : lines)
            for (auto& point : line.points) point = {static_cast<float>(t.a * point.x + t.c * point.y + t.tx), static_cast<float>(t.b * point.x + t.d * point.y + t.ty)};
    }

    namespace {
        // 描边的一侧：沿折线方向向左偏移半个线宽，转角外侧按线接样式补形，内侧经顶点折回
        class StrokeSide {
           public:
            StrokeSide(const StrokeStyle& style, float scale) : half_(style.width * 0.5f), style_(style), scale_(scale) {}

            void join(std::vector<Point>& out, Point vertex, Point before, Point after) const {
                const auto n0 = normal(before), n1 = normal(after);
                const auto turn = cross(before, after);
                const auto from = vertex + n0 * half_, to = vertex + n1 * half_;
                // y 向下时 turn < 0 是向左转，左侧成为内侧
                if (turn <= 0 || length(from - to) < 1e-4f) {
                    add(out, from);
                    if (turn < 0) add(out, vertex);
                    add(out, to);
                    return;
                }
                add(out, from);
                if (style_.join == core::StrokeJoin::Round)
                    arc(out, vertex, std::atan2(n0.y, n0.x), std::atan2(n1.y, n1.x), false);
                else if (style_.join == core::StrokeJoin::Miter) {
                    // 两条偏移线的交点；尖角过长时退化为斜接
                    const auto cosine = std::clamp(dot(n0, n1), -1.0f, 1.0f);
                    const auto cosHalf = std::sqrt(std::max(0.0f, (1.0f + cosine) * 0.5f));
                    if (cosHalf > 1e-4f && 1.0f / cosHalf <= style_.miterLimit) {
                        const auto direction = n0 + n1;
                        const auto size = length(direction);
                        if (size > 1e-6f) add(out, vertex + direction * (half_ / cosHalf / size));
                    }
                }
                add(out, to);
            }

            void cap(std::vector<Point>& out, Point end, Point direction) const {
                // 从左侧绕过端点到右侧
                const auto n = normal(direction);
                const auto unit = direction * (1.0f / std::max(length(direction), 1e-12f));
                const auto left = end + n * half_, right = end - n * half_;
                add(out, left);
                if (style_.cap == core::StrokeCap::Square) {
                    add(out, left + unit * half_);
                    add(out, right + unit * half_);
                } else if (style_.cap == core::StrokeCap::Round) {
                    const auto start = std::atan2(n.y, n.x);
                    arc(out, end, start, start + pi, true);
                }
                add(out, right);
            }

            [[nodiscard]] Point offset(Point point, Point direction) const {
                return point + normal(direction) * half_;
            }

           private:
            static void add(std::vector<Point>& out, Point point) {
                if (out.empty() || out.back() != point) out.push_back(point);
            }

            // 方向的左手法线（y 向下的屏幕坐标里指向行进方向左侧）
            static Point normal(Point direction) {
                const auto size = std::max(length(direction), 1e-12f);
                return {direction.y / size, -direction.x / size};
            }

            void arc(std::vector<Point>& out, Point center, float from, float to, bool exact) const {
                auto sweep = to - from;
                if (!exact) {
                    // 外侧圆角取较短的那段弧
                    while (sweep > pi) sweep -= 2 * pi;
                    while (sweep < -pi) sweep += 2 * pi;
                }
                const auto count = arcSegments(half_, sweep, scale_);
                for (int i = 1; i < count; ++i) {
                    const auto angle = from + sweep * static_cast<float>(i) / static_cast<float>(count);
                    add(out, {center.x + half_ * std::cos(angle), center.y + half_ * std::sin(angle)});
                }
            }

            float half_;
            StrokeStyle style_;
            float scale_;
        };

        std::vector<Point> reversed(std::vector<Point> points) {
            std::reverse(points.begin(), points.end());
            return points;
        }
    }

    std::vector<Polyline> strokePolylines(const std::vector<Polyline>& lines, const StrokeStyle& style, float scale) {
        std::vector<Polyline> result;
        if (!(style.width > 0)) return result;
        const StrokeSide side(style, scale);
        for (const auto& line : lines) {
            std::vector<Point> points;
            for (const auto point : line.points)
                if (points.empty() || points.back() != point) points.push_back(point);
            if (line.closed && points.size() > 1 && points.front() == points.back()) points.pop_back();
            const auto count = points.size();
            if (count == 1) {
                // 单点只有圆头和方头有形状
                if (style.cap == core::StrokeCap::Butt) continue;
                Polyline dot;
                dot.closed = true;
                side.cap(dot.points, points[0], {1, 0});
                side.cap(dot.points, points[0], {-1, 0});
                result.push_back(std::move(dot));
                continue;
            }
            if (count < 2) continue;
            const auto direction = [&](std::size_t from) {
                return points[(from + 1) % count] - points[from];
            };
            if (line.closed && count > 2) {
                // 闭合路径：左右两侧各成一圈，反向相减得到环
                Polyline left, right;
                left.closed = right.closed = true;
                const auto back = reversed(points);
                const auto backDirection = [&](std::size_t from) {
                    return back[(from + 1) % count] - back[from];
                };
                for (std::size_t i = 0; i < count; ++i) {
                    side.join(left.points, points[i], direction((i + count - 1) % count), direction(i));
                    side.join(right.points, back[i], backDirection((i + count - 1) % count), backDirection(i));
                }
                result.push_back(std::move(left));
                result.push_back(std::move(right));
                continue;
            }
            Polyline outline;
            outline.closed = true;
            outline.points.push_back(side.offset(points[0], direction(0)));
            for (std::size_t i = 1; i + 1 < count; ++i) side.join(outline.points, points[i], direction(i - 1), direction(i));
            side.cap(outline.points, points[count - 1], direction(count - 2));
            for (std::size_t i = count - 2; i >= 1; --i) side.join(outline.points, points[i], points[i] - points[i + 1], points[i - 1] - points[i]);
            side.cap(outline.points, points[0], points[0] - points[1]);
            result.push_back(std::move(outline));
        }
        return result;
    }

    CoverageMask rasterizePolygons(const std::vector<Polyline>& polygons, bool evenOdd, const PixelRect& clip) {
        CoverageMask mask;
        float minX = 0, minY = 0, maxX = 0, maxY = 0;
        bool any = false;
        for (const auto& polygon : polygons)
            for (const auto point : polygon.points) {
                if (!std::isfinite(point.x) || !std::isfinite(point.y)) return mask;
                minX = any ? std::min(minX, point.x) : point.x;
                minY = any ? std::min(minY, point.y) : point.y;
                maxX = any ? std::max(maxX, point.x) : point.x;
                maxY = any ? std::max(maxY, point.y) : point.y;
                any = true;
            }
        if (!any) return mask;
        const auto bound = [](float value) {
            return static_cast<int>(std::clamp(value, -1.0e8f, 1.0e8f));
        };
        mask.bounds = intersect({bound(std::floor(minX)), bound(std::floor(minY)), bound(std::ceil(maxX)), bound(std::ceil(maxY))}, clip);
        if (mask.bounds.empty()) return mask;
        const auto width = mask.bounds.width(), height = mask.bounds.height();
        // 每行多留两格：右侧越界的边把面积记在这里，不影响可见像素
        const auto stride = static_cast<std::size_t>(width) + 2;
        std::vector<float> area(stride * static_cast<std::size_t>(height), 0.0f);
        const auto originX = static_cast<float>(mask.bounds.left), originY = static_cast<float>(mask.bounds.top);
        const auto limit = static_cast<float>(width);
        const auto edge = [&](Point from, Point to) {
            from = {from.x - originX, from.y - originY};
            to = {to.x - originX, to.y - originY};
            if (from.y == to.y) return;
            const auto direction = from.y < to.y ? 1.0f : -1.0f;
            if (from.y > to.y) std::swap(from, to);
            const auto slope = (to.x - from.x) / (to.y - from.y);
            auto x = from.x;
            if (from.y < 0) x -= from.y * slope;
            const auto firstRow = std::max(0, static_cast<int>(std::floor(from.y)));
            const auto lastRow = std::min(height, static_cast<int>(std::ceil(to.y)));
            for (int row = firstRow; row < lastRow; ++row) {
                auto* line = area.data() + stride * static_cast<std::size_t>(row);
                const auto dy = std::min(static_cast<float>(row + 1), to.y) - std::max(static_cast<float>(row), from.y);
                const auto next = x + slope * dy;
                const auto d = dy * direction;
                // 左侧越界的边对它右边所有像素都是整格覆盖，夹到 0 即可；右侧越界夹到宽度
                auto x0 = std::clamp(std::min(x, next), 0.0f, limit), x1 = std::clamp(std::max(x, next), 0.0f, limit);
                const auto x0Floor = std::floor(x0);
                const auto x0Index = static_cast<int>(x0Floor);
                const auto x1Ceil = std::ceil(x1);
                const auto x1Index = static_cast<int>(x1Ceil);
                if (x1Index <= x0Index + 1) {
                    const auto middle = 0.5f * (x0 + x1) - x0Floor;
                    line[x0Index] += d - d * middle;
                    line[x0Index + 1] += d * middle;
                } else {
                    const auto s = 1.0f / (x1 - x0);
                    const auto x0Fraction = x0 - x0Floor;
                    const auto a0 = 0.5f * s * (1.0f - x0Fraction) * (1.0f - x0Fraction);
                    const auto x1Fraction = x1 - x1Ceil + 1.0f;
                    const auto am = 0.5f * s * x1Fraction * x1Fraction;
                    line[x0Index] += d * a0;
                    if (x1Index == x0Index + 2)
                        line[x0Index + 1] += d * (1.0f - a0 - am);
                    else {
                        const auto a1 = s * (1.5f - x0Fraction);
                        line[x0Index + 1] += d * (a1 - a0);
                        for (int column = x0Index + 2; column < x1Index - 1; ++column) line[column] += d * s;
                        const auto a2 = a1 + static_cast<float>(x1Index - x0Index - 3) * s;
                        line[x1Index - 1] += d * (1.0f - a2 - am);
                    }
                    line[x1Index] += d * am;
                }
                x = next;
            }
        };
        for (const auto& polygon : polygons) {
            const auto count = polygon.points.size();
            if (count < 2) continue;
            for (std::size_t i = 0; i + 1 < count; ++i) edge(polygon.points[i], polygon.points[i + 1]);
            // 填充时开放的子路径按闭合处理
            edge(polygon.points[count - 1], polygon.points[0]);
        }
        mask.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
        for (int row = 0; row < height; ++row) {
            const auto* line = area.data() + stride * static_cast<std::size_t>(row);
            auto* out = mask.pixels.data() + static_cast<std::size_t>(width) * static_cast<std::size_t>(row);
            float winding = 0;
            for (int column = 0; column < width; ++column) {
                winding += line[column];
                auto coverage = std::abs(winding);
                if (evenOdd) {
                    coverage = std::fmod(coverage, 2.0f);
                    if (coverage > 1.0f) coverage = 2.0f - coverage;
                } else
                    coverage = std::min(coverage, 1.0f);
                out[column] = static_cast<std::uint8_t>(coverage * 255.0f + 0.5f);
            }
        }
        return mask;
    }
}
