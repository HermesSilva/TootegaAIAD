#include "DocCommands.h"

#include <format>

namespace aiad::doc
{

using cmd::CommandOutput;

namespace
{

const Json kNumberOrFormula = {{"type", {"number", "string"}}, {"description", "a number or a formula over the parameters"}};

Json Object(Json properties, std::vector<std::string> required = {})
{
    Json schema = {{"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", false}};
    if (!required.empty())
        schema["required"] = required;
    return schema;
}

std::string Shown(const Json& v)
{
    if (v.is_number())
        return std::format("{:g}", v.get<double>());
    return v.is_string() ? "= " + v.get<std::string>() : v.dump();
}

PartInfo FromArgs(const Json& args, PartInfo base)
{
    base.name = args.value("name", base.name);
    base.body = args.value("body", base.body);
    base.material = args.value("material", base.material);
    base.role = args.value("role", base.role);
    base.sectioned = args.value("sectioned", base.sectioned);
    if (args.contains("features"))
        base.features = args["features"];
    return base;
}

// One undoable step on parts: restore every touched part to its snapshot.
struct PartsChange
{
    std::vector<std::pair<pxr::SdfPath, Json>> before, after;
};

CommandOutput Undoable(std::function<Document*()> document, PartsChange change, std::string label, std::string text)
{
    CommandOutput o;
    o.label = std::move(label);
    o.text = std::move(text);
    auto apply = [document](const std::vector<std::pair<pxr::SdfPath, Json>>& states) {
        Document* d = document();
        if (!d)
            return;
        std::string e;
        // Remove first (paths may be reused), then recreate.
        for (const auto& [path, snapshot] : states)
            if (snapshot.is_null())
                d->Restore(path, Json(), e);
        for (const auto& [path, snapshot] : states)
            if (!snapshot.is_null())
                d->Restore(path, snapshot, e);
    };
    o.undo = [apply, before = change.before] { apply(before); };
    o.redo = [apply, after = change.after] { apply(after); };
    return o;
}

pxr::SdfPath PathOf(Document& d, const PartInfo& p)
{
    std::string e;
    const auto found = d.FindPart(p.name, e);
    return found ? found->path : pxr::SdfPath();
}

}  // namespace

void RegisterDocumentCommands(cmd::CommandBus& bus, std::function<Document*()> document)
{
    auto none = [] { return CommandOutput::Fail("No document is open."); };

    // ---- Parameters -------------------------------------------------------------------------------
    bus.Register({"aiad_parameters_get", "Get parameters",
                  "The document parameters (the design's driving dimensions and their formulas) with value, unit, range and meaning. "
                  "Lengths in metres, angles in degrees.",
                  Object(Json::object()), {true, false, true}, [document, none](const Json&) {
                      Document* d = document();
                      if (!d)
                          return none();
                      CommandOutput o;
                      Json list = Json::array();
                      std::string text = "| Parameter | Definition | Value | Unit | Range | Meaning |\n|---|---|---|---|---|---|\n";
                      for (const Parameter& p : d->Parameters())
                      {
                          double v = 0;
                          std::string e;
                          const bool ok = d->Value(p.name, v, e);
                          list.push_back({{"name", p.name}, {"definition", p.value}, {"value", ok ? Json(v) : Json()}, {"unit", p.unit}, {"description", p.description}});
                          text += std::format("| {} | {} | {} | {} | {} | {} |\n", p.name, Shown(p.value), ok ? std::format("{:.6g}", v) : "error: " + e, p.unit,
                                              p.minimum || p.maximum ? std::format("{:g}..{:g}", p.minimum.value_or(-1e300), p.maximum.value_or(1e300)) : "",
                                              p.description);
                      }
                      o.result = {{"parameters", list}};
                      o.text = text;
                      return o;
                  }});

    bus.Register({"aiad_parameters_set", "Set parameters",
                  "Sets parameter values (numbers or formulas). Every part that depends on them is regenerated, and so is the derived physics. "
                  "Atomic: if a part cannot be built, nothing changes. One undoable step.",
                  Object({{"values", {{"type", "object"}, {"additionalProperties", kNumberOrFormula}}}}, {"values"}), {},
                  [document, none](const Json& args) {
                      Document* d = document();
                      if (!d)
                          return none();
                      Json before = Json::object();
                      for (const auto& [name, _] : args["values"].items())
                          if (auto p = d->FindParameter(name))
                              before[name] = p->value;
                      std::string error;
                      std::vector<std::string> regenerated;
                      if (!d->SetParameters(args["values"], error, &regenerated))
                          return CommandOutput::Fail(error);
                      CommandOutput o;
                      std::string label;
                      for (const auto& [name, value] : args["values"].items())
                          label += (label.empty() ? "" : ", ") + name + " = " + Shown(value);
                      o.label = "Set " + label;
                      std::string parts;
                      for (const std::string& n : regenerated)
                          parts += (parts.empty() ? "" : ", ") + n;
                      o.text = std::format("Set {}. Regenerated {} part(s){}{}.", label, regenerated.size(), parts.empty() ? "" : ": ", parts);
                      o.result = {{"regenerated", regenerated}};
                      o.undo = [document, before] {
                          std::string e;
                          if (Document* doc = document())
                              doc->SetParameters(before, e);
                      };
                      o.redo = [document, after = args["values"]] {
                          std::string e;
                          if (Document* doc = document())
                              doc->SetParameters(after, e);
                      };
                      return o;
                  }});

    bus.Register({"aiad_parameter_define", "Define parameter", "Creates a parameter or replaces its definition (value or formula, unit, range, meaning).",
                  Object({{"name", {{"type", "string"}}},
                          {"value", kNumberOrFormula},
                          {"unit", {{"type", "string"}}},
                          {"description", {{"type", "string"}}},
                          {"min", {{"type", "number"}}},
                          {"max", {{"type", "number"}}}},
                         {"name", "value"}),
                  {}, [document, none](const Json& args) {
                      Document* d = document();
                      if (!d)
                          return none();
                      Parameter p{args["name"], args["value"], args.value("unit", ""), args.value("description", "")};
                      if (args.contains("min"))
                          p.minimum = args["min"].get<double>();
                      if (args.contains("max"))
                          p.maximum = args["max"].get<double>();
                      const std::optional<Parameter> before = d->FindParameter(p.name);
                      std::string error;
                      if (!d->DefineParameter(p, error))
                          return CommandOutput::Fail(error);
                      CommandOutput o;
                      o.label = "Define " + p.name;
                      o.text = std::format("Parameter {} = {}.", p.name, Shown(p.value));
                      o.undo = [document, before, name = p.name] {
                          std::string e;
                          if (Document* doc = document())
                              before ? (void)doc->DefineParameter(*before, e) : (void)doc->RemoveParameter(name, e);
                      };
                      o.redo = [document, p] {
                          std::string e;
                          if (Document* doc = document())
                              doc->DefineParameter(p, e);
                      };
                      return o;
                  }});

    // ---- Materials --------------------------------------------------------------------------------
    bus.Register({"aiad_materials_list", "List materials",
                  "The material table: density (drives the masses), shear modulus (springs), remanence (magnets), relative permeability "
                  "(magnetic cores), colour, metallic, roughness, emission.",
                  Object(Json::object()), {true, false, true}, [document, none](const Json&) {
                      Document* d = document();
                      if (!d)
                          return none();
                      CommandOutput o;
                      std::string text = "| Material | Density [kg/m^3] | Shear modulus [GPa] | Remanence [T] | Rel. permeability | Colour | Metallic | "
                                         "Roughness |\n|---|---|---|---|---|---|---|---|\n";
                      Json list = Json::array();
                      for (const Material& m : d->Materials())
                      {
                          list.push_back({{"name", m.name},
                                          {"density", m.density},
                                          {"shear_modulus", m.shearModulus},
                                          {"remanence", m.remanence},
                                          {"relative_permeability", m.relativePermeability}});
                          auto given = [](double v, double scale) { return v > 0.0 ? std::format("{:g}", v * scale) : std::string("-"); };
                          text += std::format("| {} | {:g} | {} | {} | {} | {:.2f} {:.2f} {:.2f} | {:g} | {:g} |\n", m.name, m.density, given(m.shearModulus, 1e-9),
                                              given(m.remanence, 1.0), given(m.relativePermeability, 1.0), m.colour[0], m.colour[1], m.colour[2], m.metallic,
                                              m.roughness);
                      }
                      o.result = {{"materials", list}};
                      o.text = text;
                      return o;
                  }});
    bus.Register({"aiad_material_define", "Define material", "Creates or replaces a material. Parts using it get its density (mass) and appearance.",
                  Object({{"name", {{"type", "string"}}},
                          {"density", {{"type", "number"}, {"minimum", 0}}},
                          {"colour", {{"type", "array"}, {"items", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}}}},
                          {"metallic", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                          {"roughness", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                          {"emission", {{"type", "number"}, {"minimum", 0}}},
                          {"shear_modulus", {{"type", "number"}, {"minimum", 0}, {"description", "[Pa], for springs (steel 79.3e9)"}}},
                          {"remanence", {{"type", "number"}, {"minimum", 0}, {"description", "[T], for permanent magnets (ferrite 0.4)"}}},
                          {"relative_permeability", {{"type", "number"}, {"minimum", 0}, {"description", "for magnetic cores"}}}},
                         {"name", "density"}),
                  {}, [document, none](const Json& args) {
                      Document* d = document();
                      if (!d)
                          return none();
                      Material m;
                      m.name = args["name"];
                      m.density = args["density"];
                      if (args.contains("colour") && args["colour"].size() == 3)
                          m.colour = {args["colour"][0], args["colour"][1], args["colour"][2]};
                      m.metallic = args.value("metallic", 0.0);
                      m.roughness = args.value("roughness", 0.5);
                      m.emission = args.value("emission", 0.0);
                      m.shearModulus = args.value("shear_modulus", 0.0);
                      m.remanence = args.value("remanence", 0.0);
                      m.relativePermeability = args.value("relative_permeability", 0.0);
                      const std::optional<Material> before = d->FindMaterial(m.name);
                      std::string error;
                      if (!d->DefineMaterial(m, error))
                          return CommandOutput::Fail(error);
                      CommandOutput o;
                      o.label = "Material " + m.name;
                      o.text = std::format("Material {} ({:g} kg/m^3).", m.name, m.density);
                      if (before)
                      {
                          o.undo = [document, before] {
                              std::string e;
                              if (Document* doc = document())
                                  doc->DefineMaterial(*before, e);
                          };
                          o.redo = [document, m] {
                              std::string e;
                              if (Document* doc = document())
                                  doc->DefineMaterial(m, e);
                          };
                      }
                      return o;
                  }});

    // ---- Parts ------------------------------------------------------------------------------------
    bus.Register({"aiad_parts_list", "List parts", "Every part of the document: name, rigid body, material, role, mass, volume, and its regeneration error if any.",
                  Object(Json::object()), {true, false, true}, [document, none](const Json&) {
                      Document* d = document();
                      if (!d)
                          return none();
                      CommandOutput o;
                      Json list = Json::array();
                      std::string text = "| Part | Body | Material | Role | Features | Mass [g] | Volume [cm^3] |\n|---|---|---|---|---|---|---|\n";
                      for (const PartInfo& p : d->Parts())
                      {
                          kernel::MassProperties mp;
                          std::string e;
                          d->Mass(p.path, mp, e);
                          list.push_back({{"name", p.name}, {"path", p.path.GetString()}, {"body", p.body}, {"material", p.material}, {"role", p.role},
                                          {"features", p.features.size()}, {"mass_kg", mp.mass}, {"volume_cm3", mp.volume * 1e6}, {"error", p.error}});
                          text += std::format("| {} | {} | {} | {} | {} | {:.1f} | {:.2f} |{}\n", p.name, p.body, p.material, p.role, p.features.size(), mp.mass * 1e3,
                                              mp.volume * 1e6, p.error.empty() ? "" : " ERROR: " + p.error);
                      }
                      o.result = {{"parts", list}};
                      o.text = text;
                      return o;
                  }});

    bus.Register({"aiad_part_get", "Get part", "One part's full definition (its feature history as JSON) and its mass properties.",
                  Object({{"part", {{"type", "string"}, {"description", "name, unique name fragment or path"}}}}, {"part"}), {true, false, true},
                  [document, none](const Json& args) {
                      Document* d = document();
                      if (!d)
                          return none();
                      std::string error;
                      const auto p = d->FindPart(args["part"], error);
                      if (!p)
                          return CommandOutput::Fail(error);
                      kernel::MassProperties mp;
                      d->Mass(p->path, mp, error);
                      CommandOutput o;
                      o.result = {{"name", p->name},         {"path", p->path.GetString()}, {"body", p->body},
                                  {"material", p->material}, {"role", p->role},             {"sectioned", p->sectioned},
                                  {"features", p->features}, {"error", p->error},
                                  {"mass", {{"mass_kg", mp.mass}, {"volume_m3", mp.volume}, {"centroid", {mp.centroid.x, mp.centroid.y, mp.centroid.z}}, {"inertia", mp.inertia}}}};
                      o.text = std::format("Part {} ({} body, {}, role '{}'), mass {:.1f} g, volume {:.2f} cm^3. Features:\n```json\n{}\n```", p->name, p->body,
                                           p->material, p->role, mp.mass * 1e3, mp.volume * 1e6, p->features.dump(1));
                      return o;
                  }});

    const Json partProperties = {{"name", {{"type", "string"}}},
                                 {"body", {{"type", "string"}, {"description", "rigid body (e.g. Frame, Crankshaft, Rod, Piston)"}}},
                                 {"material", {{"type", "string"}}},
                                 {"role", {{"type", "string"}, {"description", "what the simulation reads it as (optional)"}}},
                                 {"sectioned", {{"type", "boolean"}, {"description", "cut by the section view"}}},
                                 {"features", {{"type", "array"}, {"description", "feature history (see the document format)"}}}};
    bus.Register({"aiad_part_create", "Create part",
                  "Creates a part from its feature history: sketches (vertex lists with arcs, or circle/rect/polygon/rounded_rect/sprocket/hull/cam tools) "
                  "on planes, extruded or revolved; cylinders, boxes, spheres, helical and path pipes; add/cut/intersect; patterns. Numbers may be "
                  "formulas over the parameters. One undoable step.",
                  Object(partProperties, {"name", "body", "material", "features"}), {}, [document, none](const Json& args) {
                      Document* d = document();
                      if (!d)
                          return none();
                      const PartInfo p = FromArgs(args, PartInfo{});
                      std::string error;
                      if (!d->CreatePart(p, error))
                          return CommandOutput::Fail("Part not created: " + error);
                      const pxr::SdfPath path = PathOf(*d, p);
                      kernel::MassProperties mp;
                      d->Mass(path, mp, error);
                      return Undoable(document, {{{path, Json()}}, {{path, d->Snapshot(path)}}}, "Create " + p.name,
                                      std::format("Created {} ({:.1f} g, {:.2f} cm^3).", p.name, mp.mass * 1e3, mp.volume * 1e6));
                  }});

    Json updateProperties = partProperties;
    updateProperties["part"] = {{"type", "string"}, {"description", "the part to change (name or path)"}};
    bus.Register({"aiad_part_update", "Update part",
                  "Changes a part: its features (the whole history), material, role, section flag, name or body. Regenerated at once; one undoable step.",
                  Object(updateProperties, {"part"}), {}, [document, none](const Json& args) {
                      Document* d = document();
                      if (!d)
                          return none();
                      std::string error;
                      const auto p = d->FindPart(args["part"], error);
                      if (!p)
                          return CommandOutput::Fail(error);
                      const Json before = d->Snapshot(p->path);
                      const PartInfo changed = FromArgs(args, *p);
                      if (!d->UpdatePart(p->path, changed, error))
                          return CommandOutput::Fail("Part not changed: " + error);
                      const pxr::SdfPath after = PathOf(*d, changed);
                      PartsChange change{{{after, Json()}, {p->path, before}}, {{p->path, Json()}, {after, d->Snapshot(after)}}};
                      if (after == p->path)
                          change = {{{p->path, before}}, {{p->path, d->Snapshot(p->path)}}};
                      return Undoable(document, change, "Update " + changed.name, "Updated " + changed.name + ".");
                  }});

    bus.Register({"aiad_part_delete", "Delete part", "Deletes a part. Undoable.", Object({{"part", {{"type", "string"}}}}, {"part"}), {false, true, false},
                  [document, none](const Json& args) {
                      Document* d = document();
                      if (!d)
                          return none();
                      std::string error;
                      const auto p = d->FindPart(args["part"], error);
                      if (!p)
                          return CommandOutput::Fail(error);
                      const Json before = d->Snapshot(p->path);
                      if (!d->DeletePart(p->path, error))
                          return CommandOutput::Fail(error);
                      return Undoable(document, {{{p->path, before}}, {{p->path, Json()}}}, "Delete " + p->name, "Deleted " + p->name + ".");
                  }});

    bus.Register({"aiad_feature_update", "Update feature",
                  "Changes fields of one feature of a part (index from 1), e.g. {\"distance\": \"0.012\"} or a new profile. One undoable step.",
                  Object({{"part", {{"type", "string"}}}, {"index", {{"type", "integer"}, {"minimum", 1}}}, {"set", {{"type", "object"}}}}, {"part", "index", "set"}), {},
                  [document, none](const Json& args) {
                      Document* d = document();
                      if (!d)
                          return none();
                      std::string error;
                      const auto p = d->FindPart(args["part"], error);
                      if (!p)
                          return CommandOutput::Fail(error);
                      const size_t index = args["index"].get<size_t>();
                      if (index > p->features.size())
                          return CommandOutput::Fail(std::format("{} has {} feature(s)", p->name, p->features.size()));
                      PartInfo changed = *p;
                      for (const auto& [k, v] : args["set"].items())
                          changed.features[index - 1][k] = v;
                      const Json before = d->Snapshot(p->path);
                      if (!d->UpdatePart(p->path, changed, error))
                          return CommandOutput::Fail("Feature not changed: " + error);
                      return Undoable(document, {{{p->path, before}}, {{p->path, d->Snapshot(p->path)}}}, std::format("Edit {} feature {}", p->name, index),
                                      std::format("Updated feature {} of {}.", index, p->name));
                  }});

    bus.Register({"aiad_body_define", "Define body",
                  "Defines a rigid body and its rest placement (origin, numbers or formulas). Parts of a body move together. "
                  "fixed: true makes it part of the ground (it does not move; joints to it hold the other bodies).",
                  Object({{"body", {{"type", "string"}}}, {"origin", {{"type", "array"}, {"items", kNumberOrFormula}}}, {"fixed", {{"type", "boolean"}}}},
                         {"body", "origin"}),
                  {}, [document, none](const Json& args) {
                      Document* d = document();
                      if (!d)
                          return none();
                      const std::string body = args["body"];
                      const bool existed = bool(d->Stage()->GetPrimAtPath(d->BodyPath(body)));
                      const Json before = d->BodyOriginDefinition(body);
                      const bool fixedBefore = d->BodyFixed(body);
                      const std::optional<bool> fixed = args.contains("fixed") ? std::optional<bool>(args["fixed"].get<bool>()) : std::nullopt;
                      std::string error;
                      if (!d->DefineBody(body, args["origin"], error, fixed))
                          return CommandOutput::Fail(error);
                      CommandOutput o;
                      o.label = "Body " + body;
                      o.text = "Body " + body + " at " + args["origin"].dump() + (d->BodyFixed(body) ? " (fixed: part of the ground)." : ".");
                      if (existed)
                      {
                          o.undo = [document, body, before, fixedBefore] {
                              std::string e;
                              if (Document* doc = document())
                                  doc->DefineBody(body, before, e, fixedBefore);
                          };
                          o.redo = [document, body, after = args["origin"], fixed] {
                              std::string e;
                              if (Document* doc = document())
                                  doc->DefineBody(body, after, e, fixed);
                          };
                      }
                      return o;
                  }});

    // ---- Joints: the mechanism (the simulation is built from them) ----
    auto jointJson = [](const JointInfo& j) {
        return Json{{"name", j.name}, {"type", j.type}, {"body0", j.body0}, {"body1", j.body1.empty() ? Json("ground") : Json(j.body1)},
                    {"point", j.point}, {"axis", j.axis}, {"motor", j.motor}, {"limits", j.limits}};
    };
    // Restores a joint to a definition (null = deleted).
    auto setJoint = [document](const std::string& name, const std::optional<JointInfo>& state) {
        Document* d = document();
        if (!d)
            return;
        std::string e;
        if (state)
            d->DefineJoint(*state, e);
        else if (d->FindJoint(name))
            d->DeleteJoint(name, e);
    };
    bus.Register({"aiad_joints_list", "List joints",
                  "The joints of the mechanism: type, the two bodies (body1 'ground' = fixed to the world), point and axis (formulas), motor.",
                  Object(Json::object()), {true, false, true}, [document, none, jointJson](const Json&) {
                      Document* d = document();
                      if (!d)
                          return none();
                      CommandOutput o;
                      o.result = Json::array();
                      std::string text = "| Joint | Type | Body 0 | Body 1 | Point | Axis | Motor | Limits |\n|---|---|---|---|---|---|---|---|\n";
                      for (const JointInfo& j : d->Joints())
                      {
                          o.result.push_back(jointJson(j));
                          text += std::format("| {} | {} | {} | {} | {} | {} | {} | {} |\n", j.name, j.type, j.body0, j.body1.empty() ? "ground" : j.body1,
                                              j.point.dump(), j.axis.dump(), j.motor.is_null() ? "-" : j.motor.dump(), j.limits.is_null() ? "-" : j.limits.dump());
                      }
                      o.text = d->Joints().empty() ? "No joint: every body is free (aiad_joint_define connects two bodies)." : text;
                      return o;
                  }});
    bus.Register({"aiad_joint_define", "Define joint",
                  "Creates or replaces a joint between two bodies: revolute (rotation about the axis through the point), prismatic (sliding along "
                  "the axis), cylindrical (both), spherical (ball at the point), planar (plane through the point, axis = normal), fixed. "
                  "body1 omitted or 'ground' = fixed to the world. Point and axis in the model frame, numbers or formulas. Optional motor: "
                  "{\"mode\": \"speed\" (rad/s or m/s) | \"torque\" (N m or N), \"value\": number or formula}; a torque motor may have "
                  "\"free_speed\" (DC-motor curve: torque falls to 0 at that speed, e.g. a starter) and \"duration\" [s]. Optional limits "
                  "[lower, upper] (rad or m from the drawn position; null = no stop on that side): seats and end stops, e.g. a valve "
                  "[null, 0] closes on its seat.",
                  Object({{"name", {{"type", "string"}}},
                          {"type", {{"type", "string"}, {"enum", {"revolute", "prismatic", "cylindrical", "spherical", "planar", "fixed"}}}},
                          {"body0", {{"type", "string"}}},
                          {"body1", {{"type", "string"}}},
                          {"point", {{"type", "array"}, {"items", kNumberOrFormula}}},
                          {"axis", {{"type", "array"}, {"items", kNumberOrFormula}}},
                          {"motor", {{"type", {"object", "null"}}}},
                          {"limits", {{"type", {"array", "null"}}, {"items", {{"type", {"number", "string", "null"}}}}}}},
                         {"name", "type", "body0", "point"}),
                  {}, [document, none, setJoint](const Json& args) {
                      Document* d = document();
                      if (!d)
                          return none();
                      JointInfo j;
                      j.name = args["name"];
                      j.type = args["type"];
                      j.body0 = args["body0"];
                      j.body1 = args.value("body1", "");
                      if (j.body1 == "ground" || j.body1 == "world")
                          j.body1.clear();
                      j.point = args["point"];
                      j.axis = args.value("axis", Json::array({0, 0, 1}));
                      j.motor = args.value("motor", Json());
                      j.limits = args.value("limits", Json());
                      const std::optional<JointInfo> before = d->FindJoint(j.name);
                      std::string error;
                      if (!d->DefineJoint(j, error))
                          return CommandOutput::Fail("Joint " + j.name + ": " + error);
                      CommandOutput o;
                      o.label = "Joint " + j.name;
                      o.text = std::format("{} joint {} between {} and {} at {}.", j.type, j.name, j.body0, j.body1.empty() ? "the ground" : j.body1, j.point.dump());
                      o.undo = [setJoint, name = j.name, before] { setJoint(name, before); };
                      o.redo = [setJoint, name = j.name, after = std::optional<JointInfo>(j)] { setJoint(name, after); };
                      return o;
                  }});
    bus.Register({"aiad_joint_delete", "Delete joint", "Removes a joint (undoable).", Object({{"name", {{"type", "string"}}}}, {"name"}), {false, true, false},
                  [document, none, setJoint](const Json& args) {
                      Document* d = document();
                      if (!d)
                          return none();
                      const std::string name = args["name"];
                      const std::optional<JointInfo> before = d->FindJoint(name);
                      std::string error;
                      if (!d->DeleteJoint(name, error))
                          return CommandOutput::Fail(error);
                      CommandOutput o;
                      o.label = "Delete joint " + name;
                      o.text = "Deleted joint " + name + ".";
                      o.undo = [setJoint, name, before] { setJoint(name, before); };
                      o.redo = [setJoint, name] { setJoint(name, std::nullopt); };
                      return o;
                  }});
    // ---- Elements: what else the machine is made of, tied to its joints and drawn parts ------------------
    auto setElement = [document](const std::string& name, const std::optional<ElementInfo>& state) {
        Document* d = document();
        if (!d)
            return;
        std::string e;
        if (state)
            d->DefineElement(*state, e);
        else if (d->FindElement(name))
            d->DeleteElement(name, e);
    };
    bus.Register({"aiad_elements_list", "List elements",
                  "The elements of the machine besides bodies and joints: gears, belts, racks, cam/pad contacts, springs, loads, chambers, "
                  "magnetos, with their definitions.",
                  Object(Json::object()), {true, false, true}, [document, none](const Json&) {
                      Document* d = document();
                      if (!d)
                          return none();
                      CommandOutput o;
                      o.result = Json::array();
                      std::string text = "| Element | Type | Definition |\n|---|---|---|\n";
                      for (const ElementInfo& e : d->Elements())
                      {
                          o.result.push_back({{"name", e.name}, {"type", e.type}, {"definition", e.definition}});
                          text += std::format("| {} | {} | {} |\n", e.name, e.type, e.definition.dump());
                      }
                      o.text = d->Elements().empty() ? "No element (aiad_element_define adds gears, belts, contacts, springs, loads, chambers, magnetos)."
                                                     : text;
                      return o;
                  }});
    bus.Register({"aiad_element_define", "Define element",
                  "Creates or replaces an element of the machine, tied to its joints (by name), drawn parts (by name) and bodies; numbers may be "
                  "formulas, points and directions are in the model frame. The simulation reads what it needs from the drawn parts (teeth, "
                  "profiles, coil geometry, materials) and the simulator criticises what does not fit. Types and fields:\n"
                  "- gear {joint0, joint1, part0, part1, internal?}: the drawn gears (sketch tool gear or sprocket) on the two joints mesh; ratio "
                  "from their teeth, direction from the mesh (internal: ring gear).\n"
                  "- belt {joint0, joint1, part0, part1, belt?}: a belt or chain around two drawn wheels (sprockets or pulleys); same direction.\n"
                  "- rack {joint0 (pinion, revolute), joint1 (rack, prismatic), part0 (pinion), part1?}.\n"
                  "- contact {part, body, point, normal, radius, stiffness?}: the drawn profile of `part` (an extruded sketch: cam lobe, rocker "
                  "pad) pushes the flat face of `body` (tappet, valve tip): face centre `point` and `normal` (toward the profile) as drawn, face "
                  "`radius`.\n"
                  "- spring {part, body0, point0, body1, point1, free_length, active_turns?, damping?}: the drawn coil spring (helix_pipe; its "
                  "material needs shear_modulus) between two bodies; stiffness from wire, coil and turns; its body is stretched in the view.\n"
                  "- load {joint, constant?, linear?, quadratic?}: resisting torque or force -(c0 sign + c1 rate + c2 rate|rate|) (friction, "
                  "brake, propeller).\n"
                  "- chamber {piston (joint), cylinder (bore part), head (part), intake [{joint, part}], exhaust [{joint, part}], ignition? "
                  "(magneto element), crank? (joint), intake_pressure?, intake_temperature?, exhaust_pressure?, exhaust_temperature?, "
                  "air_fuel_ratio?, burn_duration? (deg)}: 0D gas cycle; bore, chamber volume and valve sizes measured on the parts.\n"
                  "- magneto {rotor (joint), magnet (part), shoes [parts], core [parts], coil?, points_open (deg), points_close?, primary_turns?, "
                  "secondary_turns?}: field from the drawn magnet and core (MFEM), breaker points, spark to the chamber that names it.",
                  Object({{"name", {{"type", "string"}}},
                          {"type", {{"type", "string"}, {"enum", {"gear", "belt", "rack", "contact", "spring", "load", "chamber", "magneto"}}}},
                          {"definition", {{"type", "object"}, {"description", "the fields of the type"}}}},
                         {"name", "type", "definition"}),
                  {}, [document, none, setElement](const Json& args) {
                      Document* d = document();
                      if (!d)
                          return none();
                      const ElementInfo element{args["name"].get<std::string>(), args["type"].get<std::string>(), args["definition"]};
                      const std::optional<ElementInfo> before = d->FindElement(element.name);
                      std::string error;
                      if (!d->DefineElement(element, error))
                          return CommandOutput::Fail(std::format("{} {}: {}", element.type, element.name, error));
                      CommandOutput o;
                      o.label = std::format("{} {}", element.type, element.name);
                      o.text = std::format("Defined {} '{}'. aiad_mechanism_check says whether the drawing supports it.", element.type, element.name);
                      o.undo = [setElement, name = element.name, before] { setElement(name, before); };
                      o.redo = [setElement, name = element.name, after = std::optional<ElementInfo>(element)] { setElement(name, after); };
                      return o;
                  }});
    bus.Register({"aiad_element_delete", "Delete element", "Removes an element of the machine (undoable).", Object({{"name", {{"type", "string"}}}}, {"name"}),
                  {false, true, false}, [document, none, setElement](const Json& args) {
                      Document* d = document();
                      if (!d)
                          return none();
                      const std::string name = args["name"];
                      const std::optional<ElementInfo> before = d->FindElement(name);
                      std::string error;
                      if (!d->DeleteElement(name, error))
                          return CommandOutput::Fail(error);
                      CommandOutput o;
                      o.label = "Delete element " + name;
                      o.text = "Deleted element " + name + ".";
                      o.undo = [setElement, name, before] { setElement(name, before); };
                      o.redo = [setElement, name] { setElement(name, std::nullopt); };
                      return o;
                  }});
    bus.Register({"aiad_doc_orient", "Orient model", "Orientation of the whole model in the world: rotation about an axis through the origin.",
                  Object({{"axis", {{"type", "array"}, {"items", {{"type", "number"}}}}}, {"angle", {{"type", "number"}}}}, {"axis", "angle"}), {false, false, true},
                  [document, none](const Json& args) {
                      Document* d = document();
                      if (!d)
                          return none();
                      std::string error;
                      if (args["axis"].size() != 3 || !d->SetOrientation({args["axis"][0], args["axis"][1], args["axis"][2]}, args["angle"], error))
                          return CommandOutput::Fail(error.empty() ? "axis must be [x, y, z]" : error);
                      CommandOutput o;
                      o.text = "Model oriented.";
                      return o;
                  }});

    bus.Register({"aiad_doc_save", "Save document", "Saves the document (USD) to its file, or to `path`.", Object({{"path", {{"type", "string"}}}}),
                  {false, false, true}, [document, none](const Json& args) {
                      Document* d = document();
                      if (!d)
                          return none();
                      const std::string path = args.value("path", d->Path());
                      if (path.empty())
                          return CommandOutput::Fail("The document has no file yet: give a path.");
                      std::string error;
                      if (!d->Save(path, error))
                          return CommandOutput::Fail(error);
                      CommandOutput o;
                      o.text = "Saved " + path + ".";
                      return o;
                  }});
}

bool RunScript(cmd::CommandBus& bus, const Json& script, std::string& error, int* steps)
{
    if (!script.is_array())
    {
        error = "a command script is a JSON list of {\"command\", \"args\"}";
        return false;
    }
    int n = 0;
    for (const Json& step : script)
    {
        ++n;
        if (step.contains("comment") && !step.contains("command"))
            continue;
        const cmd::CommandOutput out = bus.Execute(step.value("command", ""), step.value("args", Json::object()), "script");
        if (!out.ok)
        {
            error = std::format("step {} ({}{}): {}", n, step.value("command", "?"),
                                step.contains("args") && step["args"].contains("name") ? " " + step["args"]["name"].get<std::string>() : "", out.error);
            if (steps)
                *steps = n;
            return false;
        }
    }
    if (steps)
        *steps = n;
    return true;
}

}  // namespace aiad::doc
