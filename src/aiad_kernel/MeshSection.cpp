// Section view on meshes: clip at a plane, then close the cut with planar caps (loops chained from the
// cut segments, holes bridged to their outer loop, ear clipping).
#include "Kernel.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace aiad::kernel
{

namespace
{

using P2 = std::array<double, 2>;
using P3 = std::array<double, 3>;

P3 Lerp(const P3& a, const P3& b, double t)
{
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

double Cross2(const P2& o, const P2& a, const P2& b)
{
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
}

double SignedArea(const std::vector<P2>& loop)
{
    double a = 0;
    for (size_t i = 0; i < loop.size(); ++i)
    {
        const P2& p = loop[i];
        const P2& q = loop[(i + 1) % loop.size()];
        a += p[0] * q[1] - q[0] * p[1];
    }
    return a / 2;
}

bool Inside(const std::vector<P2>& loop, const P2& p)
{
    bool in = false;
    for (size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++)
        if ((loop[i][1] > p[1]) != (loop[j][1] > p[1]) &&
            p[0] < (loop[j][0] - loop[i][0]) * (p[1] - loop[i][1]) / (loop[j][1] - loop[i][1]) + loop[i][0])
            in = !in;
    return in;
}

bool InTriangle(const P2& p, const P2& a, const P2& b, const P2& c)
{
    return Cross2(a, b, p) > 0 && Cross2(b, c, p) > 0 && Cross2(c, a, p) > 0;
}

// Ear clipping of a simple counter-clockwise polygon (indices into `pts`).
void EarClip(const std::vector<P2>& pts, std::vector<int> poly, std::vector<std::array<int, 3>>& out)
{
    int guard = 0;
    while (poly.size() > 3 && guard++ < 100000)
    {
        bool clipped = false;
        for (size_t i = 0; i < poly.size(); ++i)
        {
            const int a = poly[(i + poly.size() - 1) % poly.size()], b = poly[i], c = poly[(i + 1) % poly.size()];
            if (Cross2(pts[size_t(a)], pts[size_t(b)], pts[size_t(c)]) <= 1e-18)
                continue;  // reflex or degenerate
            bool ear = true;
            for (const int v : poly)
                if (v != a && v != b && v != c && InTriangle(pts[size_t(v)], pts[size_t(a)], pts[size_t(b)], pts[size_t(c)]))
                {
                    ear = false;
                    break;
                }
            if (!ear)
                continue;
            out.push_back({a, b, c});
            poly.erase(poly.begin() + long(i));
            clipped = true;
            break;
        }
        if (!clipped)
        {
            // Numerically stuck (collinear runs): drop the flattest vertex and go on.
            size_t worst = 0;
            double best = 1e300;
            for (size_t i = 0; i < poly.size(); ++i)
            {
                const double area = std::abs(Cross2(pts[size_t(poly[(i + poly.size() - 1) % poly.size()])], pts[size_t(poly[i])],
                                                     pts[size_t(poly[(i + 1) % poly.size()])]));
                if (area < best)
                {
                    best = area;
                    worst = i;
                }
            }
            poly.erase(poly.begin() + long(worst));
        }
    }
    if (poly.size() == 3 && Cross2(pts[size_t(poly[0])], pts[size_t(poly[1])], pts[size_t(poly[2])]) > 0)
        out.push_back({poly[0], poly[1], poly[2]});
}

}  // namespace

double MeshArea(const Mesh& mesh)
{
    double area = 0;
    for (const auto& t : mesh.triangles)
    {
        const auto& a = mesh.points[size_t(t[0])];
        const auto& b = mesh.points[size_t(t[1])];
        const auto& c = mesh.points[size_t(t[2])];
        const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        area += 0.5 * std::sqrt(std::pow(uy * vz - uz * vy, 2) + std::pow(uz * vx - ux * vz, 2) + std::pow(ux * vy - uy * vx, 2));
    }
    return area;
}

void SectionMesh(const Mesh& mesh, const Vec3& point, const Vec3& normal, Mesh& kept, Mesh& cap)
{
    kept = Mesh();
    cap = Mesh();
    const double ln = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    const P3 n{normal.x / ln, normal.y / ln, normal.z / ln};
    auto dist = [&](const std::array<float, 3>& p) { return (p[0] - point.x) * n[0] + (p[1] - point.y) * n[1] + (p[2] - point.z) * n[2]; };
    // In-plane frame for the caps.
    const P3 helper = std::abs(n[0]) < 0.9 ? P3{1, 0, 0} : P3{0, 1, 0};
    P3 u{helper[1] * n[2] - helper[2] * n[1], helper[2] * n[0] - helper[0] * n[2], helper[0] * n[1] - helper[1] * n[0]};
    const double lu = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    u = {u[0] / lu, u[1] / lu, u[2] / lu};
    const P3 v{n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0]};
    auto to2 = [&](const P3& p) { return P2{(p[0] - point.x) * u[0] + (p[1] - point.y) * u[1] + (p[2] - point.z) * u[2],
                                            (p[0] - point.x) * v[0] + (p[1] - point.y) * v[1] + (p[2] - point.z) * v[2]}; };

    std::vector<std::pair<P3, P3>> segments;  // cut edges, oriented so the material is on their left (seen from +n)
    auto emit = [&](const P3& p, const P3& nn) {
        kept.points.push_back({float(p[0]), float(p[1]), float(p[2])});
        kept.normals.push_back({float(nn[0]), float(nn[1]), float(nn[2])});
        return int(kept.points.size()) - 1;
    };
    for (const auto& t : mesh.triangles)
    {
        P3 p[3], nn[3];
        double d[3];
        for (int k = 0; k < 3; ++k)
        {
            const auto& q = mesh.points[size_t(t[size_t(k)])];
            const auto& m = mesh.normals[size_t(t[size_t(k)])];
            p[k] = {q[0], q[1], q[2]};
            nn[k] = {m[0], m[1], m[2]};
            d[k] = dist(q);
        }
        if (d[0] <= 0 && d[1] <= 0 && d[2] <= 0)
        {
            kept.triangles.push_back({emit(p[0], nn[0]), emit(p[1], nn[1]), emit(p[2], nn[2])});
            continue;
        }
        if (d[0] > 0 && d[1] > 0 && d[2] > 0)
            continue;
        // Sutherland-Hodgman on the triangle; the polygon keeps the triangle's winding.
        std::vector<std::pair<P3, P3>> poly;
        std::vector<P3> crossings;
        for (int k = 0; k < 3; ++k)
        {
            const int j = (k + 1) % 3;
            if (d[k] <= 0)
                poly.push_back({p[k], nn[k]});
            if ((d[k] <= 0) != (d[j] <= 0))
            {
                const double s = d[k] / (d[k] - d[j]);
                const P3 x = Lerp(p[k], p[j], s);
                poly.push_back({x, Lerp(nn[k], nn[j], s)});
                crossings.push_back(x);
            }
        }
        if (poly.size() >= 3)
        {
            const int first = emit(poly[0].first, poly[0].second);
            int prev = emit(poly[1].first, poly[1].second);
            for (size_t k = 2; k < poly.size(); ++k)
            {
                const int cur = emit(poly[k].first, poly[k].second);
                kept.triangles.push_back({first, prev, cur});
                prev = cur;
            }
        }
        if (crossings.size() == 2)
            segments.push_back({crossings[0], crossings[1]});
    }

    // Chain the segments into loops (endpoints matched on a 1 micrometre grid).
    auto key = [&](const P3& p) {
        const P2 q = to2(p);
        return std::make_pair(std::llround(q[0] * 1e6), std::llround(q[1] * 1e6));
    };
    std::multimap<std::pair<long long, long long>, size_t> byEnd;
    for (size_t i = 0; i < segments.size(); ++i)
    {
        byEnd.insert({key(segments[i].first), i});
        byEnd.insert({key(segments[i].second), i});
    }
    std::vector<bool> used(segments.size(), false);
    std::vector<std::vector<P2>> loops;
    for (size_t s = 0; s < segments.size(); ++s)
    {
        if (used[s])
            continue;
        used[s] = true;
        std::vector<P2> loop{to2(segments[s].first)};
        P3 end = segments[s].second;
        const auto start = key(segments[s].first);
        for (size_t guard = 0; guard < segments.size(); ++guard)
        {
            const auto k = key(end);
            if (k == start)
                break;
            loop.push_back(to2(end));
            bool next = false;
            auto range = byEnd.equal_range(k);
            for (auto it = range.first; it != range.second; ++it)
                if (!used[it->second])
                {
                    used[it->second] = true;
                    end = key(segments[it->second].first) == k ? segments[it->second].second : segments[it->second].first;
                    next = true;
                    break;
                }
            if (!next)
                break;
        }
        if (loop.size() >= 3 && std::abs(SignedArea(loop)) > 1e-14)
            loops.push_back(std::move(loop));
    }

    // Outer loops (counter-clockwise) and their holes (clockwise), by nesting depth.
    std::vector<int> depth(loops.size(), 0);
    for (size_t i = 0; i < loops.size(); ++i)
        for (size_t j = 0; j < loops.size(); ++j)
            if (i != j && Inside(loops[j], loops[i][0]))
                ++depth[i];
    for (size_t i = 0; i < loops.size(); ++i)
        if ((depth[i] % 2 == 0) != (SignedArea(loops[i]) > 0))
            std::reverse(loops[i].begin(), loops[i].end());
    const float nx = float(n[0]), ny = float(n[1]), nz = float(n[2]);
    for (size_t o = 0; o < loops.size(); ++o)
    {
        if (depth[o] % 2 != 0)
            continue;
        // Holes directly inside this outer loop, bridged from their rightmost vertex.
        std::vector<P2> pts = loops[o];
        std::vector<int> poly(pts.size());
        for (size_t k = 0; k < poly.size(); ++k)
            poly[k] = int(k);
        std::vector<size_t> holes;
        for (size_t h = 0; h < loops.size(); ++h)
            if (depth[h] == depth[o] + 1 && Inside(loops[o], loops[h][0]))
                holes.push_back(h);
        std::sort(holes.begin(), holes.end(), [&](size_t a, size_t b) {
            auto maxX = [&](size_t h) { double m = -1e300; for (const P2& p : loops[h]) m = std::max(m, p[0]); return m; };
            return maxX(a) > maxX(b);
        });
        for (const size_t h : holes)
        {
            const std::vector<P2>& hole = loops[h];
            size_t hi = 0;
            for (size_t k = 1; k < hole.size(); ++k)
                if (hole[k][0] > hole[hi][0])
                    hi = k;
            const P2 m = hole[hi];
            // Nearest polygon vertex to the right of m that m can see (no polygon edge in between).
            int bridge = -1;
            double best = 1e300;
            for (size_t k = 0; k < poly.size(); ++k)
            {
                const P2& c = pts[size_t(poly[k])];
                if (c[0] < m[0])
                    continue;
                const double dd = std::hypot(c[0] - m[0], c[1] - m[1]);
                if (dd >= best)
                    continue;
                bool blocked = false;
                for (size_t e = 0; e < poly.size() && !blocked; ++e)
                {
                    const P2& a = pts[size_t(poly[e])];
                    const P2& b = pts[size_t(poly[(e + 1) % poly.size()])];
                    if (poly[e] == poly[k] || poly[(e + 1) % poly.size()] == poly[k])
                        continue;
                    const double d1 = Cross2(m, c, a), d2 = Cross2(m, c, b), d3 = Cross2(a, b, m), d4 = Cross2(a, b, c);
                    blocked = ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
                }
                if (!blocked)
                {
                    best = dd;
                    bridge = int(k);
                }
            }
            if (bridge < 0)
                continue;
            // Splice: ... bridge, hole[hi], hole[hi+1], ..., hole[hi], bridge, ...
            std::vector<int> spliced(poly.begin(), poly.begin() + bridge + 1);
            const int base = int(pts.size());
            pts.insert(pts.end(), hole.begin(), hole.end());
            for (size_t k = 0; k <= hole.size(); ++k)
                spliced.push_back(base + int((hi + k) % hole.size()));
            spliced.push_back(poly[size_t(bridge)]);
            spliced.insert(spliced.end(), poly.begin() + bridge + 1, poly.end());
            poly = std::move(spliced);
        }
        std::vector<std::array<int, 3>> tris;
        EarClip(pts, poly, tris);
        const int first = int(cap.points.size());
        for (const P2& p : pts)
        {
            cap.points.push_back({float(point.x + u[0] * p[0] + v[0] * p[1]), float(point.y + u[1] * p[0] + v[1] * p[1]),
                                  float(point.z + u[2] * p[0] + v[2] * p[1])});
            cap.normals.push_back({nx, ny, nz});
        }
        for (const auto& t : tris)
            cap.triangles.push_back({first + t[0], first + t[1], first + t[2]});
    }
}

}  // namespace aiad::kernel
