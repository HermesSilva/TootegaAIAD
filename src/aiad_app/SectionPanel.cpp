#include "SectionPanel.h"

#include "Session.h"

#include <pxr/base/gf/range3d.h>
#include <pxr/base/gf/rotation.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace aiad::app
{

namespace
{

using pxr::GfVec3d;

// The plane being dragged in the panel (orientation or offset): drawn at once, cut and recorded when let go.
std::string g_active, g_previewName;
Json g_preview;
bool g_panelOpen = false, g_openRequest = false;
int g_drag = 0;   // in the view: 1 turning the plane (its arrow's tip), 2 moving it along its normal (its centre)

struct Entry
{
    std::string name;
    Json definition;
};

// The document's planes; a drawing without any shows the cut of its `sectioned` parts as "Section 1" (editing it makes it
// a plane of the document).
std::vector<Entry> Sections(Session& session, bool& implicit)
{
    std::vector<Entry> list;
    implicit = false;
    if (!session.document || !session.scene)
        return list;
    for (const doc::ElementInfo& s : session.document->Sections())
        list.push_back({s.name, s.definition});
    if (list.empty() && !session.scene->Sections().empty())
    {
        const SectionPlane& p = session.scene->Sections().front();
        list.push_back({"Section 1", Json{{"point", {p.point.x, p.point.y, p.point.z}}, {"normal", {p.normal.x, p.normal.y, p.normal.z}}, {"parts", p.targets}}});
        implicit = true;
    }
    for (Entry& e : list)
        if (e.name == g_previewName && g_preview.is_object())
            e.definition = g_preview;
    return list;
}

std::string NextName(const std::vector<Entry>& list)
{
    for (int n = 1;; ++n)
    {
        const std::string name = std::format("Section {}", n);
        if (std::none_of(list.begin(), list.end(), [&](const Entry& e) { return e.name == name; }))
            return name;
    }
}

GfVec3d Vector(const Json& d, const char* key, GfVec3d fallback)
{
    if (!d.contains(key) || !d[key].is_array() || d[key].size() != 3)
        return fallback;
    return GfVec3d(d[key][0].get<double>(), d[key][1].get<double>(), d[key][2].get<double>());
}

Json Array(const GfVec3d& v)
{
    auto clean = [](double x) { return std::abs(x) < 1e-12 ? 0.0 : x; };
    return Json::array({clean(v[0]), clean(v[1]), clean(v[2])});
}

bool Listed(const Json& d, const char* key, const std::string& name)
{
    for (const Json& n : d.value(key, Json::array()))
        if (n.is_string() && n.get<std::string>() == name)
            return true;
    return false;
}

void Unlist(Json& d, const char* key, const std::string& name)
{
    if (!d.contains(key))
        return;
    Json kept = Json::array();
    for (const Json& n : d[key])
        if (!(n.is_string() && n.get<std::string>() == name))
            kept.push_back(n);
    if (kept.empty())
        d.erase(key);
    else
        d[key] = kept;
}

void List(Json& d, const char* key, const std::string& name)
{
    if (!Listed(d, key, name))
        d[key].push_back(name);
}

bool Cuts(const Json& d, const std::string& part, const std::string& body)
{
    const bool only = d.contains("parts") || d.contains("bodies");
    if (only && !Listed(d, "parts", part) && !Listed(d, "bodies", body))
        return false;
    return !Listed(d, "exclude", part) && !Listed(d, "exclude", body);
}

void Define(const RunCommand& command, const std::string& name, const Json& d)
{
    command("aiad_section_define", {{"name", name}, {"definition", d}});
}

// While a plane is dragged the view cuts with it at once; letting go records it (one undoable step).
void Preview(Session& session, const std::string& name, const Json& d)
{
    g_previewName = name, g_preview = d;
    session.scene->SetPreview(name, d);
}

void Commit(Session& session, const RunCommand& command, const std::string& name, const Json& d)
{
    Define(command, name, d);
    g_previewName.clear(), g_preview = Json();
    if (session.scene)
        session.scene->SetPreview(std::string(), Json());
}

// Where a plane is drawn: around the parts it alone cuts, else around the model.
pxr::GfRange3d PlaneBox(Session& session, const Json& d)
{
    const DocumentScene& scene = *session.scene;
    if (d.contains("parts") || d.contains("bodies"))
    {
        pxr::GfRange3d box;
        for (size_t i = 0; i < scene.Parts().size(); ++i)
            if (Listed(d, "parts", scene.Parts()[i].name) || Listed(d, "bodies", scene.Parts()[i].body))
                box.UnionWith(scene.PartBounds(int(i)));
        if (!box.IsEmpty())
            return box;
    }
    return scene.ModelBounds();
}

// Two directions across the plane.
void Across(const GfVec3d& n, GfVec3d& u, GfVec3d& v)
{
    const GfVec3d a = std::abs(n[2]) < 0.9 ? GfVec3d(0, 0, 1) : GfVec3d(1, 0, 0);
    u = pxr::GfCross(n, a).GetNormalized();
    v = pxr::GfCross(n, u);
}

}  // namespace

void DrawSectionsPanel(Session& session, bool& open, int selected, const GfVec3d& facing, const RunCommand& command)
{
    if (g_openRequest)
        open = true, g_openRequest = false;
    g_panelOpen = open;
    if (!open)
        return;
    ImGui::SetNextWindowSize(ImVec2(440.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Sections", &open))
    {
        ImGui::End();
        return;
    }
    if (!session.scene || !session.document)
    {
        ImGui::TextDisabled("No document open.");
        ImGui::End();
        return;
    }
    DocumentScene& scene = *session.scene;
    bool implicit = false;
    const std::vector<Entry> list = Sections(session, implicit);
    const pxr::GfRange3d box = scene.ModelBounds();
    const bool hasPart = selected >= 0 && selected < int(scene.Parts().size());
    const std::string partName = hasPart ? scene.Parts()[size_t(selected)].name : std::string();
    const std::string bodyName = hasPart ? scene.Parts()[size_t(selected)].body : std::string();

    // New planes: along an axis of the model or facing the view, through the selected part (or the model's centre).
    ImGui::TextDisabled("New plane through %s:", hasPart ? partName.c_str() : "the model's centre");
    const GfVec3d centre = hasPart ? scene.PartCentre(selected) : (box.IsEmpty() ? GfVec3d(0.0) : box.GetMidpoint());
    for (const auto& [label, normal] : {std::pair{"X", GfVec3d(1, 0, 0)}, std::pair{"Y", GfVec3d(0, 1, 0)}, std::pair{"Z", GfVec3d(0, 0, 1)},
                                        std::pair{"Facing the view", facing}})
    {
        if (ImGui::Button(label))
        {
            g_active = NextName(list);
            Define(command, g_active, Json{{"point", Array(centre)}, {"normal", Array(normal.GetNormalized())}});
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::Separator();
    if (list.empty())
        ImGui::TextDisabled("No section plane: the view shows the parts whole.");
    if (implicit)
        ImGui::TextWrapped("From the drawing: the parts marked 'sectioned', cut at y = 0. Editing it makes it a section plane of the document.");
    if (std::none_of(list.begin(), list.end(), [](const Entry& e) { return e.name == g_active; }))
        g_active = list.empty() ? std::string() : list.front().name;
    for (const Entry& e : list)
    {
        ImGui::PushID(e.name.c_str());
        bool on = e.definition.value("enabled", true);
        if (ImGui::Checkbox("##on", &on))
        {
            Json d = e.definition;
            d["enabled"] = on;
            Define(command, e.name, d);
        }
        ImGui::SameLine();
        if (ImGui::Selectable(e.name.c_str(), g_active == e.name))
            g_active = e.name;
        ImGui::PopID();
    }
    auto it = std::find_if(list.begin(), list.end(), [](const Entry& e) { return e.name == g_active; });
    if (it == list.end())
    {
        ImGui::End();
        return;
    }

    // The plane being edited.
    ImGui::SeparatorText(it->name.c_str());
    Json d = it->definition;
    GfVec3d n = Vector(d, "normal", GfVec3d(0, -1, 0)).GetNormalized(), p = Vector(d, "point", GfVec3d(0.0));
    auto commit = [&](const Json& next) { Commit(session, command, it->name, next); };
    auto preview = [&](const Json& next) { Preview(session, it->name, next); };
    // Orientation: along an axis, facing the view, flipped, or turned and tilted by hand.
    ImGui::TextDisabled("Normal (the side it points to is cut away)");
    for (const auto& [label, normal] : {std::pair{"X##n", GfVec3d(1, 0, 0)}, std::pair{"Y##n", GfVec3d(0, 1, 0)}, std::pair{"Z##n", GfVec3d(0, 0, 1)},
                                        std::pair{"Facing the view##n", facing}, std::pair{"Flip##n", -n}})
    {
        if (ImGui::Button(label))
        {
            d["normal"] = Array(normal.GetNormalized());
            commit(d);
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    float turn = float(std::atan2(n[1], n[0]) * 180.0 / std::numbers::pi), tilt = float(std::asin(std::clamp(n[2], -1.0, 1.0)) * 180.0 / std::numbers::pi);
    bool turned = ImGui::SliderFloat("Turn", &turn, -180.0f, 180.0f, "%.1f deg");
    const bool turnDone = ImGui::IsItemDeactivatedAfterEdit();
    turned = ImGui::SliderFloat("Tilt", &tilt, -90.0f, 90.0f, "%.1f deg") || turned;
    const bool tiltDone = ImGui::IsItemDeactivatedAfterEdit();
    if (turned)
    {
        const double a = turn * std::numbers::pi / 180.0, b = tilt * std::numbers::pi / 180.0;
        n = GfVec3d(std::cos(b) * std::cos(a), std::cos(b) * std::sin(a), std::sin(b));
        d["normal"] = Array(n);
        preview(d);
    }
    // Offset along the normal, across the model.
    double lo = 0.0, hi = 0.0;
    if (!box.IsEmpty())
    {
        lo = 1e30, hi = -1e30;
        for (int c = 0; c < 8; ++c)
        {
            const double s = pxr::GfDot(box.GetCorner(size_t(c)), n);
            lo = std::min(lo, s), hi = std::max(hi, s);
        }
    }
    const double offset = pxr::GfDot(p, n);
    float mm = float(offset * 1e3);
    const bool moved = ImGui::SliderFloat("Offset", &mm, float(std::min(lo, offset) * 1e3), float(std::max(hi, offset) * 1e3), "%.2f mm");
    const bool moveDone = ImGui::IsItemDeactivatedAfterEdit();
    if (moved)
    {
        p += n * (mm * 1e-3 - offset);
        d["point"] = Array(p);
        preview(d);
    }
    if (turnDone || tiltDone || moveDone)
        commit(d);
    ImGui::BeginDisabled(!hasPart);
    if (ImGui::Button("Through the selected part"))
    {
        d["point"] = Array(scene.PartCentre(selected));
        commit(d);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool fill = d.value("fill", true);
    if (ImGui::Checkbox("Fill the cut", &fill))
    {
        d["fill"] = fill;
        commit(d);
    }

    // What it cuts: every part, or only some; parts or bodies kept whole.
    const bool only = d.contains("parts") || d.contains("bodies");
    ImGui::TextDisabled("Cuts");
    ImGui::SameLine();
    if (ImGui::RadioButton("every part", !only) && only)
    {
        d.erase("parts"), d.erase("bodies");
        commit(d);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!hasPart && !only);
    if (ImGui::RadioButton("only these", only) && !only)
    {
        d["parts"] = Json::array({partName});
        Unlist(d, "exclude", partName);
        commit(d);
    }
    ImGui::EndDisabled();
    auto names = [&](const char* key, const char* title) {
        if (!d.contains(key))
            return;
        ImGui::TextDisabled("%s", title);
        for (const Json& name : d[key])
        {
            const std::string s = name.get<std::string>();
            ImGui::PushID((std::string(key) + s).c_str());
            if (ImGui::SmallButton("x"))
            {
                Json next = d;
                Unlist(next, key, s);
                if (std::string(key) != "exclude" && !next.contains("parts") && !next.contains("bodies"))
                    next["parts"] = Json::array();   // "only these" with none left: cuts nothing until one is added
                commit(next);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(s.c_str());
            ImGui::PopID();
        }
    };
    names("parts", "Parts:");
    names("bodies", "Bodies:");
    names("exclude", "Kept whole:");
    ImGui::BeginDisabled(!hasPart);
    const std::string addPart = std::format("{} {}", only ? "Cut" : "Keep whole:", hasPart ? partName : "(select a part)");
    if (ImGui::Button(addPart.c_str()))
    {
        if (only)
            List(d, "parts", partName);
        else
            List(d, "exclude", partName);
        commit(d);
    }
    ImGui::SameLine();
    const std::string addBody = std::format("{} body {}", only ? "Cut" : "Keep whole:", hasPart ? bodyName : "");
    if (ImGui::Button(addBody.c_str()))
    {
        if (only)
            List(d, "bodies", bodyName);
        else
            List(d, "exclude", bodyName);
        commit(d);
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (ImGui::Button("Delete plane"))
    {
        if (implicit)
        {
            d["enabled"] = false;   // the drawing's default cut, switched off
            commit(d);
        }
        else
            command("aiad_section_delete", {{"name", it->name}});
    }
    ImGui::TextWrapped("In the view: drag the tip of the plane's arrow to turn it in 3D, drag its centre to move it in depth. "
                       "Moving parts are cut where they stand; while the machine runs, the cut moves with them.");
    ImGui::End();
}

bool DrawSectionPlanes(Session& session, const Project& project, const GfVec3d& right, const GfVec3d& up, bool hovered, const RunCommand& command)
{
    if (!g_panelOpen || !session.scene || !session.scene->SectionView())
    {
        g_drag = 0;
        return false;
    }
    bool implicit = false;
    const std::vector<Entry> list = Sections(session, implicit);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    bool onHandle = g_drag != 0;
    for (const Entry& e : list)
    {
        if (!e.definition.value("enabled", true))
            continue;
        const bool active = e.name == g_active;
        const pxr::GfRange3d box = PlaneBox(session, e.definition);
        if (box.IsEmpty())
            continue;
        const double half = 0.55 * std::max({box.GetSize()[0], box.GetSize()[1], box.GetSize()[2], 1e-6}), arrow = half * 0.5;
        GfVec3d n = Vector(e.definition, "normal", GfVec3d(0, -1, 0)).GetNormalized(), p = Vector(e.definition, "point", GfVec3d(0.0));
        GfVec3d u, v;
        Across(n, u, v);
        const GfVec3d m = box.GetMidpoint(), c = m - n * pxr::GfDot(m - p, n);
        ImVec2 q[4];
        bool seen = true;
        const GfVec3d corners[4] = {c + (u + v) * half, c + (u - v) * half, c - (u + v) * half, c - (u - v) * half};
        for (int k = 0; k < 4; ++k)
            seen = project(corners[k], q[k]) && seen;
        ImVec2 a, b;
        if (!seen || !project(c, a) || !project(c + n * arrow, b))
            continue;
        const ImU32 line = active ? IM_COL32(255, 176, 0, 230) : IM_COL32(200, 205, 215, 140);
        if (active)
            draw->AddQuadFilled(q[0], q[1], q[2], q[3], IM_COL32(255, 176, 0, 26));
        draw->AddQuad(q[0], q[1], q[2], q[3], line, active ? 2.0f : 1.0f);
        draw->AddLine(a, b, line, 2.0f);
        draw->AddText(ImVec2(b.x + 9.0f, b.y - 8.0f), line, e.name.c_str());
        // Handles: the arrow's tip turns the plane (as a trackball), its centre moves it along the normal.
        auto onIt = [&](ImVec2 at) { return (mouse.x - at.x) * (mouse.x - at.x) + (mouse.y - at.y) * (mouse.y - at.y) < 81.0f; };
        const bool overTip = hovered && onIt(b), overCentre = hovered && !overTip && onIt(a);
        draw->AddCircleFilled(b, overTip || (active && g_drag == 1) ? 7.0f : 5.0f, line);
        draw->AddRectFilled(ImVec2(a.x - 5, a.y - 5), ImVec2(a.x + 5, a.y + 5), overCentre || (active && g_drag == 2) ? IM_COL32(255, 220, 120, 255) : line);
        if (overTip || overCentre)
        {
            onHandle = true;
            ImGui::SetTooltip("%s", overTip ? "Drag: turn the plane in 3D" : "Drag: move the plane in depth");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                g_active = e.name, g_drag = overTip ? 1 : 2;
        }
        if (!active || g_drag == 0)
            continue;
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        Json d = e.definition;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (delta.x != 0.0f || delta.y != 0.0f)
            {
                if (g_drag == 1)
                {
                    n = pxr::GfRotation(up, delta.x * 0.4).TransformDir(n);
                    n = pxr::GfRotation(right, delta.y * 0.4).TransformDir(n).GetNormalized();
                    d["normal"] = Array(n);
                }
                else
                {
                    const float sx = b.x - a.x, sy = b.y - a.y, s2 = std::max(sx * sx + sy * sy, 1.0f);
                    p += n * (arrow * double(delta.x * sx + delta.y * sy) / double(s2));
                    d["point"] = Array(p);
                }
                Preview(session, e.name, d);
            }
        }
        else
        {
            if (g_previewName == e.name)
                Commit(session, command, e.name, g_preview);
            g_drag = 0;
        }
    }
    return onHandle;
}
void SectionPartItems(Session& session, int part, const GfVec3d& facing, const RunCommand& command)
{
    if (!session.scene || part < 0 || part >= int(session.scene->Parts().size()))
        return;
    const ScenePart& sp = session.scene->Parts()[size_t(part)];
    bool implicit = false;
    const std::vector<Entry> list = Sections(session, implicit);
    ImGui::SeparatorText("Section");
    const Json at = Array(session.scene->PartCentre(part)), normal = Array(facing.GetNormalized());
    if (ImGui::MenuItem("Cut only this part (facing the view)"))
        Define(command, g_active = NextName(list), Json{{"point", at}, {"normal", normal}, {"parts", Json::array({sp.name})}}), g_openRequest = true;
    if (ImGui::MenuItem("Cut everything through this part (facing the view)"))
        Define(command, g_active = NextName(list), Json{{"point", at}, {"normal", normal}}), g_openRequest = true;
    for (const Entry& e : list)
    {
        if (!e.definition.value("enabled", true))
            continue;
        Json d = e.definition;
        const bool only = d.contains("parts") || d.contains("bodies");
        if (Cuts(d, sp.name, sp.body))
        {
            if (ImGui::MenuItem(std::format("Keep it whole in {}", e.name).c_str()))
            {
                if (only && Listed(d, "parts", sp.name))
                    Unlist(d, "parts", sp.name);
                else
                    List(d, "exclude", sp.name);
                if (only && !d.contains("parts") && !d.contains("bodies"))
                    d["parts"] = Json::array();
                Define(command, e.name, d);
            }
        }
        else if (ImGui::MenuItem(std::format("Cut it by {}", e.name).c_str()))
        {
            Unlist(d, "exclude", sp.name);
            Unlist(d, "exclude", sp.body);
            if (only)
                List(d, "parts", sp.name);
            Define(command, e.name, d);
        }
    }
}

}  // namespace aiad::app
