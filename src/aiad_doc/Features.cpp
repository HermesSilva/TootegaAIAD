#include "Features.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace aiad::doc
{

namespace
{

constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;

using kernel::Plane;
using kernel::Profile;
using kernel::Solid;
using kernel::Vec3;

struct Eval
{
    const NameLookup& lookup;
    std::string& error;
    std::string where;

    bool Number(const Json& j, double& out)
    {
        if (j.is_number())
        {
            out = j.get<double>();
            return true;
        }
        if (j.is_string())
        {
            std::string e;
            if (Evaluate(j.get<std::string>(), lookup, out, e))
                return true;
            error = where + ": " + e;
            return false;
        }
        error = where + ": expected a number or a formula, got " + j.dump();
        return false;
    }
    bool Field(const Json& obj, const char* key, double& out, std::optional<double> fallback = std::nullopt)
    {
        if (!obj.contains(key))
        {
            if (fallback)
            {
                out = *fallback;
                return true;
            }
            error = where + ": missing \"" + key + "\"";
            return false;
        }
        const std::string saved = where;
        where += "." + std::string(key);
        const bool ok = Number(obj[key], out);
        where = saved;
        return ok;
    }
    bool Vector(const Json& obj, const char* key, Vec3& out, std::optional<Vec3> fallback = std::nullopt)
    {
        if (!obj.contains(key))
        {
            if (fallback)
            {
                out = *fallback;
                return true;
            }
            error = where + ": missing \"" + key + "\"";
            return false;
        }
        const Json& a = obj[key];
        if (!a.is_array() || a.size() != 3)
        {
            error = where + "." + key + ": expected [x, y, z]";
            return false;
        }
        const std::string saved = where;
        where += "." + std::string(key);
        const bool ok = Number(a[0], out.x) && Number(a[1], out.y) && Number(a[2], out.z);
        where = saved;
        return ok;
    }
    bool Pair(const Json& obj, const char* key, double& u, double& v)
    {
        if (!obj.contains(key) || !obj[key].is_array() || obj[key].size() != 2)
        {
            error = where + "." + key + ": expected [u, v]";
            return false;
        }
        return Number(obj[key][0], u) && Number(obj[key][1], v);
    }
};

Vec3 Axis(char c)
{
    return c == 'X' ? Vec3{1, 0, 0} : c == 'Y' ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
}

bool MakePlane(const Json& j, Eval& e, Plane& plane)
{
    if (j.is_string())
    {
        const std::string n = j.get<std::string>();
        if (n.size() != 2 || n[0] == n[1] || std::string("XYZ").find(n[0]) == std::string::npos || std::string("XYZ").find(n[1]) == std::string::npos)
        {
            e.error = e.where + ".plane: \"" + n + "\" is not a plane (XY, YZ, ZX, XZ, YX, ZY or an object)";
            return false;
        }
        plane = Plane{{0, 0, 0}, Axis(n[0]), Axis(n[1])};
        return true;
    }
    if (j.is_object() && j.contains("base"))
    {
        if (!MakePlane(j["base"], e, plane))
            return false;
        double offset = 0;
        Vec3 origin;
        if (!e.Field(j, "offset", offset, 0.0) || !e.Vector(j, "origin", origin, Vec3{}))
            return false;
        const Vec3 n = plane.Normal();
        plane.origin = {origin.x + n.x * offset, origin.y + n.y * offset, origin.z + n.z * offset};
        return true;
    }
    if (j.is_object())
        return e.Vector(j, "origin", plane.origin, Vec3{}) && e.Vector(j, "x", plane.xAxis) && e.Vector(j, "y", plane.yAxis);
    e.error = e.where + ".plane: expected a name or an object";
    return false;
}

// Arc bulge for a counter-clockwise sweep [rad].
double Bulge(double sweep)
{
    return std::tan(sweep / 4.0);
}

bool CircleSketch(const Json& c, Eval& e, Profile& p)
{
    double cu, cv, r;
    if (!e.Pair(c, "center", cu, cv) || !e.Field(c, "radius", r))
        return false;
    p = {{cu + r, cv, 1.0}, {cu - r, cv, 1.0}};
    return true;
}

bool SprocketSketch(const Json& g, Eval& e, Profile& p)
{
    double root, tip, teethD;
    if (!e.Field(g, "root_radius", root) || !e.Field(g, "tip_radius", tip) || !e.Field(g, "teeth", teethD))
        return false;
    const int teeth = int(std::lround(teethD));
    if (teeth < 3 || tip <= root)
    {
        e.error = e.where + ": a sprocket needs at least 3 teeth and tip_radius > root_radius";
        return false;
    }
    // Per pitch: root land (arc), rising flank, tip land (arc), falling flank.
    const double pitch = 2 * kPi / teeth, f[] = {0.0, 0.18, 0.38, 0.62, 0.82};
    auto at = [](double r, double a) { return std::array<double, 2>{r * std::cos(a), r * std::sin(a)}; };
    p.clear();
    for (int t = 0; t < teeth; ++t)
    {
        const double b = t * pitch;
        const auto v0 = at(root, b - (1.0 - f[4]) * pitch), v1 = at(root, b + f[1] * pitch), v2 = at(tip, b + f[2] * pitch), v3 = at(tip, b + f[3] * pitch);
        // Root arc from the previous falling flank to this rising flank, flank, tip arc, flank.
        p.push_back({v0[0], v0[1], Bulge((f[1] + 1.0 - f[4]) * pitch)});
        p.push_back({v1[0], v1[1], 0.0});
        p.push_back({v2[0], v2[1], Bulge((f[3] - f[2]) * pitch)});
        p.push_back({v3[0], v3[1], 0.0});
    }
    return true;
}

// Involute spur gear: module m, z teeth, pressure angle (20 deg); pitch radius m z / 2, addendum m,
// dedendum 1.25 m. Flanks sampled as lines, tip and root as arcs.
bool GearSketch(const Json& g, Eval& e, Profile& p)
{
    double module, teethD, pressure, rotation, cu, cv;
    if (!e.Field(g, "module", module) || !e.Field(g, "teeth", teethD) || !e.Field(g, "pressure_angle", pressure, 20.0) ||
        !e.Field(g, "rotation", rotation, 0.0))
        return false;
    cu = cv = 0.0;
    if (g.contains("center") && !e.Pair(g, "center", cu, cv))
        return false;
    const int z = int(std::lround(teethD));
    const double r = module * z / 2.0, rb = r * std::cos(pressure * kDeg), ra = r + module, rf = std::max(r - 1.25 * module, 0.2 * r);
    auto inv = [](double a) { return std::tan(a) - a; };
    // Half angle of a tooth at radius rho (zero at the pitch circle's half thickness pi / 2z).
    auto half = [&](double rho) { return kPi / (2.0 * z) + inv(pressure * kDeg) - inv(std::acos(std::min(1.0, rb / rho))); };
    if (z < 6 || module <= 0.0 || half(ra) <= 0.0)
    {
        e.error = e.where + ": a gear needs module > 0 and at least 6 teeth (with this pressure angle the teeth come to a point)";
        return false;
    }
    const double start = std::max(rb, rf), pitch = 2.0 * kPi / z;
    auto at = [&](double rho, double a) { return kernel::ProfileVertex{cu + rho * std::cos(a), cv + rho * std::sin(a), 0.0}; };
    p.clear();
    for (int t = 0; t < z; ++t)
    {
        const double c = rotation * kDeg + t * pitch;
        if (rf < start)
            p.push_back(at(rf, c - half(start)));   // radial line from the root up to the involute
        const int n = 8;
        for (int i = 0; i <= n; ++i)
        {
            const double rho = start + (ra - start) * i / n;
            p.push_back(at(rho, c - half(rho)));
        }
        p.back().bulge = Bulge(2.0 * half(ra));   // tip arc
        for (int i = n; i >= 0; --i)
        {
            const double rho = start + (ra - start) * i / n;
            p.push_back(at(rho, c + half(rho)));
        }
        if (rf < start)
            p.push_back(at(rf, c + half(start)));
        p.back().bulge = Bulge(pitch - 2.0 * half(start));   // root arc to the next tooth
    }
    return true;
}

// A strip wound as an Archimedean spiral (hairspring, mainspring): centre line r = inner + pitch * angle / 2 pi
// over `turns`, `thickness` wide. Out along the outer edge, back along the inner one (arcs of 20 degrees).
bool SpiralSketch(const Json& s, Eval& e, Profile& p)
{
    double cu = 0.0, cv = 0.0, inner, pitch, turns, thickness, rotation;
    if (!e.Field(s, "inner_radius", inner) || !e.Field(s, "pitch", pitch) || !e.Field(s, "turns", turns) || !e.Field(s, "thickness", thickness) ||
        !e.Field(s, "rotation", rotation, 0.0) || (s.contains("center") && !e.Pair(s, "center", cu, cv)))
        return false;
    if (thickness <= 0.0 || pitch <= thickness || inner <= thickness / 2.0 || turns <= 0.0)
    {
        e.error = e.where + ": a spiral needs thickness > 0, pitch > thickness (the coils apart), inner_radius > thickness / 2 and turns > 0";
        return false;
    }
    const int n = std::max(2, int(std::ceil(turns * 18.0)));
    const double sweep = 2.0 * kPi * turns / n, bulge = Bulge(sweep);
    auto at = [&](int i, double offset) {
        const double a = i * sweep, r = inner + pitch * a / (2.0 * kPi) + offset;
        return kernel::ProfileVertex{cu + r * std::cos(a + rotation * kDeg), cv + r * std::sin(a + rotation * kDeg), 0.0};
    };
    p.clear();
    for (int i = 0; i <= n; ++i)
    {
        p.push_back(at(i, thickness / 2.0));
        p.back().bulge = i < n ? bulge : 0.0;   // the end: a straight cut across the strip
    }
    for (int i = n; i >= 0; --i)
    {
        p.push_back(at(i, -thickness / 2.0));
        p.back().bulge = i > 0 ? -bulge : 0.0;
    }
    return true;
}

bool HullSketch(const Json& h, Eval& e, Profile& p)
{
    struct Sample
    {
        double u, v, angle;
        int circle;
    };
    std::vector<Sample> pts;
    std::vector<std::array<double, 3>> circles;
    if (!h.contains("circles") || !h["circles"].is_array() || h["circles"].size() < 1)
    {
        e.error = e.where + ".hull: needs \"circles\"";
        return false;
    }
    for (const Json& c : h["circles"])
    {
        double cu, cv, r, from, to;
        if (!e.Pair(c, "center", cu, cv) || !e.Field(c, "radius", r) || !e.Field(c, "from", from, 0.0) || !e.Field(c, "to", to, 360.0))
            return false;
        const int id = int(circles.size());
        circles.push_back({cu, cv, r});
        const int n = std::max(8, int(std::ceil((to - from) / 0.5)));
        for (int i = 0; i <= n; ++i)
        {
            const double a = (from + (to - from) * i / n) * kDeg;
            pts.push_back({cu + r * std::cos(a), cv + r * std::sin(a), a, id});
        }
    }
    std::sort(pts.begin(), pts.end(), [](const Sample& a, const Sample& b) { return a.u < b.u || (a.u == b.u && a.v < b.v); });
    auto cross = [](const Sample& o, const Sample& a, const Sample& b) { return (a.u - o.u) * (b.v - o.v) - (a.v - o.v) * (b.u - o.u); };
    std::vector<Sample> hull(2 * pts.size());
    size_t k = 0;
    for (size_t i = 0; i < pts.size(); ++i)
    {
        while (k >= 2 && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0)
            --k;
        hull[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i)
    {
        while (k >= t && cross(hull[k - 2], hull[k - 1], pts[i - 1]) <= 0)
            --k;
        hull[k++] = pts[i - 1];
    }
    hull.resize(k - 1);  // counter-clockwise
    // Runs on the same circle become one exact arc; between runs, tangent lines.
    p.clear();
    const size_t n = hull.size();
    size_t start = 0;
    while (start < n && hull[start].circle == hull[(start + n - 1) % n].circle)
        ++start;
    if (start == n)  // a single circle
        start = 0;
    for (size_t i = 0; i < n;)
    {
        const Sample& first = hull[(start + i) % n];
        size_t j = i;
        while (j + 1 < n && hull[(start + j + 1) % n].circle == first.circle)
            ++j;
        const Sample& last = hull[(start + j) % n];
        double sweep = last.angle - first.angle;
        while (sweep < 0)
            sweep += 2 * kPi;
        if (j > i && sweep > 1e-6)
        {
            p.push_back({first.u, first.v, Bulge(sweep)});
            p.push_back({last.u, last.v, 0.0});
        }
        else
            p.push_back({first.u, first.v, 0.0});
        i = j + 1;
    }
    return p.size() >= 2;
}

bool CamSketch(const Json& c, Eval& e, std::vector<std::array<double, 2>>& smooth)
{
    double base, lift, open, close;
    if (!e.Field(c, "base_radius", base) || !e.Field(c, "lift", lift) || !e.Field(c, "open", open) || !e.Field(c, "close", close))
        return false;
    const double duration = std::fmod(close - open + 720.0, 720.0);
    smooth.clear();
    for (int i = 0; i < 360; i += 2)
    {
        // The lobe point at cam angle psi faces the follower (-u) at cycle angle 360 - 2 psi.
        const double psi = i, cycle = std::fmod(360.0 - 2.0 * psi + 720.0, 720.0);
        const double u = std::fmod(cycle - open + 720.0, 720.0);
        const double s = u < duration ? std::sin(kPi * u / duration) : 0.0;
        const double r = base + lift * s * s;
        smooth.push_back({r * std::cos(psi * kDeg), r * std::sin(psi * kDeg)});
    }
    return true;
}

// Sketch -> exact profile, or smooth points (cam).
bool MakeSketch(const Json& s, Eval& e, Profile& profile, std::vector<std::array<double, 2>>& smooth)
{
    smooth.clear();
    profile.clear();
    if (s.is_array())
    {
        for (size_t i = 0; i < s.size(); ++i)
        {
            const Json& v = s[i];
            const std::string saved = e.where;
            e.where += std::format(".profile[{}]", i);
            kernel::ProfileVertex pv;
            const bool ok = v.is_array() && (v.size() == 2 || v.size() == 3) && e.Number(v[0], pv.u) && e.Number(v[1], pv.v) &&
                            (v.size() == 2 || e.Number(v[2], pv.bulge));
            if (!ok && e.error.empty())
                e.error = e.where + ": expected [u, v] or [u, v, bulge]";
            e.where = saved;
            if (!ok)
                return false;
            profile.push_back(pv);
        }
        return true;
    }
    if (!s.is_object() || s.size() != 1)
    {
        e.error = e.where + ".profile: expected a vertex list or one sketch tool";
        return false;
    }
    const std::string tool = s.begin().key();
    const Json& a = s.begin().value();
    const std::string saved = e.where;
    e.where += ".profile." + tool;
    bool ok = false;
    if (tool == "circle")
        ok = CircleSketch(a, e, profile);
    else if (tool == "rect")
    {
        double u0, v0, u1, v1;
        ok = e.Pair(a, "min", u0, v0) && e.Pair(a, "max", u1, v1);
        profile = {{u0, v0, 0}, {u1, v0, 0}, {u1, v1, 0}, {u0, v1, 0}};
    }
    else if (tool == "rounded_rect")
    {
        double u0, v0, u1, v1, r;
        ok = e.Pair(a, "min", u0, v0) && e.Pair(a, "max", u1, v1) && e.Field(a, "radius", r);
        const double b = Bulge(kPi / 2);
        profile = {{u0 + r, v0, 0}, {u1 - r, v0, b}, {u1, v0 + r, 0}, {u1, v1 - r, b}, {u1 - r, v1, 0}, {u0 + r, v1, b}, {u0, v1 - r, 0}, {u0, v0 + r, b}};
    }
    else if (tool == "polygon")
    {
        double cu, cv, af, sidesD, rot;
        ok = e.Pair(a, "center", cu, cv) && e.Field(a, "across_flats", af) && e.Field(a, "sides", sidesD) && e.Field(a, "rotation", rot, 0.0);
        const int sides = int(std::lround(sidesD));
        const double rc = af / 2.0 / std::cos(kPi / sides);
        for (int i = 0; ok && i < sides; ++i)
        {
            const double t = rot * kDeg + 2 * kPi * i / sides;
            profile.push_back({cu + rc * std::cos(t), cv + rc * std::sin(t), 0});
        }
    }
    else if (tool == "sprocket")
        ok = SprocketSketch(a, e, profile);
    else if (tool == "gear")
        ok = GearSketch(a, e, profile);
    else if (tool == "hull")
        ok = HullSketch(a, e, profile);
    else if (tool == "cam")
        ok = CamSketch(a, e, smooth);
    else if (tool == "spiral")
        ok = SpiralSketch(a, e, profile);
    else
        e.error = e.where + ": unknown sketch tool (circle, rect, rounded_rect, polygon, sprocket, gear, hull, cam, spiral)";
    e.where = saved;
    return ok;
}

bool Combine(Solid& part, Solid&& tool, const std::string& mode, Eval& e)
{
    if (!tool.Valid())
        return false;
    if (!part.Valid())
    {
        if (mode != "add")
        {
            e.error = e.where + ": the first feature must add material (mode \"add\")";
            return false;
        }
        part = std::move(tool);
        return true;
    }
    std::string err;
    const kernel::BooleanOp op = mode == "cut" ? kernel::BooleanOp::Subtract : mode == "intersect" ? kernel::BooleanOp::Intersect : kernel::BooleanOp::Union;
    if (!kernel::Boolean(part, std::move(tool), op, err))
    {
        e.error = e.where + ": " + err;
        return false;
    }
    return true;
}

}  // namespace

bool MovePart(const Json& features, size_t from, const NameLookup& lookup, Solid& solid, std::string& error)
{
    error.clear();
    Eval e{lookup, error, ""};
    for (size_t i = from; features.is_array() && i < features.size(); ++i)
    {
        const Json& f = features[i];
        const std::string op = f.is_object() ? f.value("op", "") : "";
        e.where = std::format("feature {} ({})", i + 1, op);
        Vec3 a, b;
        double angle = 0;
        std::string kerr;
        if (op != "rotate" && op != "translate")
        {
            error = e.where + ": an imported part can only be moved (rotate, translate) after its import";
            return false;
        }
        const bool ok = op == "rotate" ? e.Vector(f, "pivot", a, Vec3{}) && e.Vector(f, "axis", b) && e.Field(f, "angle", angle) : e.Vector(f, "offset", a);
        if (!ok)
            return false;
        if (!(op == "rotate" ? kernel::Rotate(solid, a, b, angle, kerr) : kernel::Translate(solid, a, kerr)))
        {
            error = e.where + ": " + kerr;
            return false;
        }
    }
    return true;
}

Solid BuildPart(const Json& features, const NameLookup& lookup, std::string& error)
{
    error.clear();
    Eval e{lookup, error, ""};
    if (!features.is_array() || features.empty())
    {
        error = "a part needs at least one feature";
        return Solid();
    }
    Solid part, lastTool;
    std::string lastMode = "add";
    for (size_t i = 0; i < features.size(); ++i)
    {
        const Json& f = features[i];
        e.where = std::format("feature {}", i + 1);
        if (!f.is_object() || !f.contains("op") || !f["op"].is_string())
        {
            error = e.where + ": needs an \"op\"";
            return Solid();
        }
        const std::string op = f["op"];
        e.where += " (" + op + ")";
        const std::string mode = f.value("mode", std::string("add"));
        if (mode != "add" && mode != "cut" && mode != "intersect")
        {
            error = e.where + ": mode must be add, cut or intersect";
            return Solid();
        }
        std::string kerr;
        Solid tool;
        if (op == "extrude" || op == "revolve")
        {
            Plane plane;
            Profile profile;
            std::vector<std::array<double, 2>> smooth;
            if (!f.contains("plane") || !MakePlane(f["plane"], e, plane) || !f.contains("profile") || !MakeSketch(f["profile"], e, profile, smooth))
            {
                if (error.empty())
                    error = e.where + ": needs \"plane\" and \"profile\"";
                return Solid();
            }
            if (op == "extrude")
            {
                double distance, start;
                if (!e.Field(f, "distance", distance) || !e.Field(f, "start", start, 0.0))
                    return Solid();
                const Vec3 n = plane.Normal();
                plane.origin = {plane.origin.x + n.x * start, plane.origin.y + n.y * start, plane.origin.z + n.z * start};
                tool = smooth.empty() ? kernel::Extrude(plane, profile, distance, kerr) : kernel::ExtrudeSmooth(plane, smooth, distance, kerr);
            }
            else
            {
                double angle, startAngle;
                std::array<double, 4> axis{0, 0, 0, 1};
                if (f.contains("axis"))
                {
                    const Json& a = f["axis"];
                    if (!a.is_array() || a.size() != 4)
                    {
                        error = e.where + ".axis: expected [u, v, du, dv]";
                        return Solid();
                    }
                    for (int k = 0; k < 4; ++k)
                        if (!e.Number(a[size_t(k)], axis[size_t(k)]))
                            return Solid();
                }
                if (!e.Field(f, "angle", angle, 360.0) || !e.Field(f, "start_angle", startAngle, 0.0))
                    return Solid();
                if (!smooth.empty())
                {
                    error = e.where + ": a cam sketch can only be extruded";
                    return Solid();
                }
                tool = kernel::Revolve(plane, profile, axis[0], axis[1], axis[2], axis[3], angle, kerr);
                if (tool.Valid() && std::abs(startAngle) > 1e-12)
                {
                    const Vec3 pivot = plane.ToWorld(axis[0], axis[1]);
                    const Vec3 dirEnd = plane.ToWorld(axis[0] + axis[2], axis[1] + axis[3]);
                    kernel::Rotate(tool, pivot, {dirEnd.x - pivot.x, dirEnd.y - pivot.y, dirEnd.z - pivot.z}, startAngle, kerr);
                }
            }
        }
        else if (op == "cylinder")
        {
            Vec3 base, axis;
            double r, h;
            if (!e.Vector(f, "base", base) || !e.Vector(f, "axis", axis) || !e.Field(f, "radius", r) || !e.Field(f, "height", h))
                return Solid();
            tool = kernel::Cylinder(base, axis, r, h, kerr);
        }
        else if (op == "box")
        {
            Vec3 corner, size;
            if (!e.Vector(f, "corner", corner) || !e.Vector(f, "size", size))
                return Solid();
            tool = kernel::Box(corner, size, kerr);
        }
        else if (op == "sphere")
        {
            Vec3 c;
            double r;
            if (!e.Vector(f, "center", c) || !e.Field(f, "radius", r))
                return Solid();
            tool = kernel::Sphere(c, r, kerr);
        }
        else if (op == "helix_pipe")
        {
            Vec3 base, axis;
            double coil, h, turns, wire;
            if (!e.Vector(f, "base", base) || !e.Vector(f, "axis", axis) || !e.Field(f, "coil_radius", coil) || !e.Field(f, "height", h) ||
                !e.Field(f, "turns", turns) || !e.Field(f, "wire_radius", wire))
                return Solid();
            tool = kernel::HelicalPipe(base, axis, coil, h, turns, wire, kerr);
        }
        else if (op == "pipe")
        {
            double r;
            if (!e.Field(f, "radius", r) || !f.contains("path") || !f["path"].is_array())
            {
                if (error.empty())
                    error = e.where + ": needs \"path\"";
                return Solid();
            }
            std::vector<Vec3> path;
            for (const Json& pt : f["path"])
            {
                Vec3 v;
                const Json wrapper = {{"p", pt}};
                if (!e.Vector(wrapper, "p", v))
                    return Solid();
                path.push_back(v);
            }
            tool = kernel::Pipe(path, r, kerr);
        }
        else if (op == "pattern_linear" || op == "pattern_polar")
        {
            if (!lastTool.Valid())
            {
                error = e.where + ": nothing to repeat (a pattern follows the feature it repeats)";
                return Solid();
            }
            double countD;
            if (!e.Field(f, "count", countD))
                return Solid();
            Vec3 step, point, axis;
            double angle = 0;
            if (op == "pattern_linear" ? !e.Vector(f, "step", step)
                                       : (!e.Vector(f, "axis_point", point, Vec3{}) || !e.Vector(f, "axis", axis) || !e.Field(f, "step_angle", angle)))
                return Solid();
            for (int k = 1; k < int(std::lround(countD)); ++k)
            {
                Solid copy = lastTool.Copy();
                const bool moved = op == "pattern_linear" ? kernel::Translate(copy, {step.x * k, step.y * k, step.z * k}, kerr)
                                                          : kernel::Rotate(copy, point, axis, angle * k, kerr);
                if (!moved || !Combine(part, std::move(copy), lastMode, e))
                {
                    if (error.empty())
                        error = e.where + ": " + kerr;
                    return Solid();
                }
            }
            continue;
        }
        else if (op == "rotate" || op == "translate")
        {
            if (!part.Valid())
            {
                error = e.where + ": nothing to move yet";
                return Solid();
            }
            Vec3 a, b;
            double angle = 0;
            const bool ok = op == "rotate" ? e.Vector(f, "pivot", a, Vec3{}) && e.Vector(f, "axis", b) && e.Field(f, "angle", angle)
                                           : e.Vector(f, "offset", a);
            if (!ok)
                return Solid();
            if (!(op == "rotate" ? kernel::Rotate(part, a, b, angle, kerr) : kernel::Translate(part, a, kerr)))
            {
                error = e.where + ": " + kerr;
                return Solid();
            }
            continue;
        }
        else
        {
            error = e.where + ": unknown op (extrude, revolve, cylinder, box, sphere, helix_pipe, pipe, pattern_linear, pattern_polar, rotate, translate)";
            return Solid();
        }
        if (!tool.Valid())
        {
            error = e.where + ": " + (kerr.empty() ? std::string("the kernel produced no solid") : kerr);
            return Solid();
        }
        lastTool = tool.Copy();
        lastMode = mode;
        if (!Combine(part, std::move(tool), mode, e))
            return Solid();
    }
    return part;
}

bool SketchOutline(const Json& sketch, const NameLookup& lookup, std::vector<std::array<double, 2>>& points, std::string& error)
{
    Eval e{lookup, error, "sketch"};
    Profile profile;
    std::vector<std::array<double, 2>> smooth;
    if (!MakeSketch(sketch, e, profile, smooth))
        return false;
    points = smooth;
    if (!points.empty())
        return true;
    // Exact profile: lines as their end points, arcs sampled every 2 degrees.
    for (size_t i = 0; i < profile.size(); ++i)
    {
        const auto& a = profile[i];
        const auto& b = profile[(i + 1) % profile.size()];
        points.push_back({a.u, a.v});
        if (std::abs(a.bulge) > 1e-12)
        {
            const double du = b.u - a.u, dv = b.v - a.v, chord = std::hypot(du, dv), k = a.bulge;
            const double offset = chord * (1 - k * k) / (4 * k);
            const double cu = (a.u + b.u) / 2 - dv / chord * offset, cv = (a.v + b.v) / 2 + du / chord * offset;
            const double sweep = 4 * std::atan(k), a0 = std::atan2(a.v - cv, a.u - cu), r = std::hypot(a.u - cu, a.v - cv);
            const int n = std::max(2, int(std::abs(sweep) / (2 * kDeg)));
            for (int s = 1; s < n; ++s)
                points.push_back({cu + r * std::cos(a0 + sweep * s / n), cv + r * std::sin(a0 + sweep * s / n)});
        }
    }
    return true;
}

std::vector<double> MeasureFlatFollowerLift(const std::vector<std::array<double, 2>>& outline, double baseRadius)
{
    std::vector<double> lift(720);
    for (int c = 0; c < 720; ++c)
    {
        const double a = (c / 2.0) * kDeg, ca = std::cos(a), sa = std::sin(a);
        double reach = 0.0;
        for (const auto& p : outline)
            reach = std::max(reach, -(p[0] * ca - p[1] * sa));
        lift[size_t(c)] = std::max(0.0, reach - baseRadius);
    }
    return lift;
}


namespace
{

// The rigid moves of the part ("rotate", "translate") after feature `index`, applied to points / directions.
struct Placement
{
    struct Move
    {
        bool rotate = false;
        Vec3 pivot, axis, offset;
        double angle = 0.0;
    };
    std::vector<Move> moves;

    bool Read(const Json& features, size_t index, Eval& e)
    {
        for (size_t i = index + 1; i < features.size(); ++i)
        {
            const Json& f = features[i];
            const std::string op = f.value("op", "");
            Move m;
            if (op == "rotate")
            {
                m.rotate = true;
                if (!e.Vector(f, "pivot", m.pivot, Vec3{}) || !e.Vector(f, "axis", m.axis) || !e.Field(f, "angle", m.angle))
                    return false;
            }
            else if (op == "translate")
            {
                if (!e.Vector(f, "offset", m.offset))
                    return false;
            }
            else
                continue;
            moves.push_back(m);
        }
        return true;
    }
    static Vec3 Turn(const Vec3& v, const Vec3& axis, double angleDeg)
    {
        const double n = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
        const Vec3 k{axis.x / n, axis.y / n, axis.z / n};
        const double c = std::cos(angleDeg * kDeg), s = std::sin(angleDeg * kDeg), d = k.x * v.x + k.y * v.y + k.z * v.z;
        const Vec3 x{k.y * v.z - k.z * v.y, k.z * v.x - k.x * v.z, k.x * v.y - k.y * v.x};
        return {v.x * c + x.x * s + k.x * d * (1 - c), v.y * c + x.y * s + k.y * d * (1 - c), v.z * c + x.z * s + k.z * d * (1 - c)};
    }
    Vec3 Point(Vec3 p) const
    {
        for (const Move& m : moves)
            if (m.rotate)
            {
                const Vec3 r = Turn({p.x - m.pivot.x, p.y - m.pivot.y, p.z - m.pivot.z}, m.axis, m.angle);
                p = {r.x + m.pivot.x, r.y + m.pivot.y, r.z + m.pivot.z};
            }
            else
                p = {p.x + m.offset.x, p.y + m.offset.y, p.z + m.offset.z};
        return p;
    }
    Vec3 Direction(Vec3 d) const
    {
        for (const Move& m : moves)
            if (m.rotate)
                d = Turn(d, m.axis, m.angle);
        return d;
    }
};

// The first extrusion whose sketch satisfies `accept`: its plane moved to the mid plane, its distance.
template <typename Accept>
bool FindExtrusion(const Json& features, Eval& e, Accept accept, size_t& index, Plane& plane, double& distance)
{
    if (!features.is_array())
        return false;
    for (size_t i = 0; i < features.size(); ++i)
    {
        const Json& f = features[i];
        if (f.value("op", "") != "extrude" || f.value("mode", std::string("add")) != "add" || !f.contains("profile") || !accept(f["profile"]))
            continue;
        e.where = std::format("feature {} (extrude)", i + 1);
        double start;
        if (!f.contains("plane") || !MakePlane(f["plane"], e, plane) || !e.Field(f, "distance", distance) || !e.Field(f, "start", start, 0.0))
            return false;
        const Vec3 n = plane.Normal();
        const double mid = start + distance / 2.0;
        plane.origin = {plane.origin.x + n.x * mid, plane.origin.y + n.y * mid, plane.origin.z + n.z * mid};
        index = i;
        return true;
    }
    return false;
}

Vec3 Normalized(const Vec3& v)
{
    const double n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return n > 0 ? Vec3{v.x / n, v.y / n, v.z / n} : v;
}

}  // namespace

bool FindWheel(const Json& features, const NameLookup& lookup, Wheel& wheel, std::string& error)
{
    error.clear();
    Eval e{lookup, error, ""};
    auto toothed = [](const Json& s) { return s.is_object() && (s.contains("gear") || s.contains("sprocket") || s.contains("circle")); };
    size_t index = 0;
    Plane plane;
    double distance = 0.0;
    if (!FindExtrusion(features, e, toothed, index, plane, distance))
    {
        if (error.empty())
            error = "it has no extruded gear, sprocket or circle sketch";
        return false;
    }
    const Json& sketch = features[index]["profile"];
    const std::string tool = sketch.begin().key();
    const Json& a = sketch.begin().value();
    e.where += ".profile." + tool;
    double cu = 0.0, cv = 0.0;
    wheel = Wheel();
    if (tool == "gear")
    {
        double teeth;
        if (!e.Field(a, "module", wheel.module) || !e.Field(a, "teeth", teeth) || (a.contains("center") && !e.Pair(a, "center", cu, cv)))
            return false;
        wheel.teeth = int(std::lround(teeth));
        wheel.pitchRadius = wheel.module * wheel.teeth / 2.0;
        wheel.tipRadius = wheel.pitchRadius + wheel.module;
        wheel.rootRadius = std::max(wheel.pitchRadius - 1.25 * wheel.module, 0.2 * wheel.pitchRadius);
    }
    else if (tool == "sprocket")
    {
        double teeth;
        if (!e.Field(a, "root_radius", wheel.rootRadius) || !e.Field(a, "tip_radius", wheel.tipRadius) || !e.Field(a, "teeth", teeth))
            return false;
        wheel.teeth = int(std::lround(teeth));
        wheel.pitchRadius = (wheel.rootRadius + wheel.tipRadius) / 2.0;
        wheel.module = 2.0 * wheel.pitchRadius / std::max(wheel.teeth, 1);
    }
    else
    {
        if (!e.Pair(a, "center", cu, cv) || !e.Field(a, "radius", wheel.pitchRadius))
            return false;
        wheel.rootRadius = wheel.tipRadius = wheel.pitchRadius;
    }
    Placement placement;
    if (!placement.Read(features, index, e))
        return false;
    wheel.centre = placement.Point(plane.ToWorld(cu, cv));
    wheel.axis = Normalized(placement.Direction(plane.Normal()));
    wheel.width = std::abs(distance);
    return true;
}

bool FindExtrudedProfile(const Json& features, const NameLookup& lookup, ExtrudedProfile& out, std::string& error)
{
    error.clear();
    Eval e{lookup, error, ""};
    size_t index = 0;
    Plane plane;
    double distance = 0.0;
    if (!FindExtrusion(features, e, [](const Json&) { return true; }, index, plane, distance))
    {
        if (error.empty())
            error = "it has no extruded sketch";
        return false;
    }
    std::vector<std::array<double, 2>> outline;
    if (!SketchOutline(features[index]["profile"], lookup, outline, error))
        return false;
    // Lines densified (a flat face touches a straight edge anywhere along it).
    Placement placement;
    if (!placement.Read(features, index, e))
        return false;
    out = ExtrudedProfile();
    for (size_t i = 0; i < outline.size(); ++i)
    {
        const auto& a = outline[i];
        const auto& b = outline[(i + 1) % outline.size()];
        const int n = std::max(1, int(std::ceil(std::hypot(b[0] - a[0], b[1] - a[1]) / 0.0003)));
        for (int k = 0; k < n; ++k)
            out.points.push_back(placement.Point(plane.ToWorld(a[0] + (b[0] - a[0]) * k / n, a[1] + (b[1] - a[1]) * k / n)));
    }
    out.centre = placement.Point(plane.origin);
    out.axis = Normalized(placement.Direction(plane.Normal()));
    out.width = std::abs(distance);
    return true;
}

bool FindHelix(const Json& features, const NameLookup& lookup, Helix& helix, std::string& error)
{
    error.clear();
    Eval e{lookup, error, ""};
    for (size_t i = 0; features.is_array() && i < features.size(); ++i)
    {
        const Json& f = features[i];
        if (f.value("op", "") != "helix_pipe")
            continue;
        e.where = std::format("feature {} (helix_pipe)", i + 1);
        Placement placement;
        if (!e.Vector(f, "base", helix.base) || !e.Vector(f, "axis", helix.axis) || !e.Field(f, "coil_radius", helix.coilRadius) ||
            !e.Field(f, "height", helix.height) || !e.Field(f, "turns", helix.turns) || !e.Field(f, "wire_radius", helix.wireRadius) ||
            !placement.Read(features, i, e))
            return false;
        helix.base = placement.Point(helix.base);
        helix.axis = Normalized(placement.Direction(helix.axis));
        return true;
    }
    error = "it has no helix_pipe feature (a coil spring)";
    return false;
}

}  // namespace aiad::doc
