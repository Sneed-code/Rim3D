#include "WoundViews.h"

#include <algorithm>
#include <string>

namespace colony {

namespace {

constexpr const char* kWoundBodyPartsKind = "BodyParts";

bool woundKindFromName(const std::string& name, WoundKind& out) {
    for (const WoundKind kind : {WoundKind::Cut, WoundKind::Bruise, WoundKind::Burn, WoundKind::Bite, WoundKind::Gunshot,
                                 WoundKind::Infection, WoundKind::Scar, WoundKind::Missing}) {
        if (name == woundKindName(kind)) {
            out = kind;
            return true;
        }
    }
    return false;
}

bool woundLabelHas(const std::string& label, const char* word) {
    return label.find(word) != std::string::npos;
}

}  // namespace

const char* woundKindName(WoundKind kind) {
    switch (kind) {
        case WoundKind::Cut: return "Cut";
        case WoundKind::Bruise: return "Bruise";
        case WoundKind::Burn: return "Burn";
        case WoundKind::Bite: return "Bite";
        case WoundKind::Gunshot: return "Gunshot";
        case WoundKind::Infection: return "Infection";
        case WoundKind::Scar: return "Scar";
        case WoundKind::Missing: return "Missing";
    }
    return "Cut";
}

const char* bodyRegionName(BodyRegion region) {
    switch (region) {
        case BodyRegion::Torso: return "Torso";
        case BodyRegion::Head: return "Head";
        case BodyRegion::LeftArm: return "LeftArm";
        case BodyRegion::RightArm: return "RightArm";
        case BodyRegion::LeftLeg: return "LeftLeg";
        case BodyRegion::RightLeg: return "RightLeg";
    }
    return "Torso";
}

WoundViews::WoundViews(const DefDatabase& defs, const HealthSystem& health) : defs_(defs), health_(health) {
    hediffs_.assign(defs.size(), HediffData{});
}

const WoundViews::HediffData& WoundViews::hediff(DefId id) {
    static const HediffData none;
    if (!id.valid() || id.value >= hediffs_.size()) {
        return none;
    }
    HediffData& data = hediffs_[id.value];
    if (!data.known) {
        const Def& def = defs_.get(id);
        data.known = true;
        data.bleedRate = static_cast<float>(def.number("injuryProps.bleedRate"));
        // Scars are drawn as scars unless the def says otherwise; everything else defaults to a cut.
        data.kind = def.text("hediffClass") == "Scar" ? WoundKind::Scar : WoundKind::Cut;
        woundKindFromName(def.text("woundKind"), data.kind);
    }
    return data;
}

const std::vector<WoundViews::PartData>& WoundViews::body(DefId pawnDef) {
    auto found = bodies_.find(pawnDef.value);
    if (found != bodies_.end()) {
        return found->second;
    }
    const std::vector<BodyPartInfo> infos = health_.bodyParts(pawnDef);
    std::vector<PartData> parts(infos.size());
    // A part's region is the first `skinRegion` found on it or its parents; an arm or leg is left or right by the first
    // label going up that says so (no side: left). Parents come before children in the body, so one pass works.
    std::vector<std::string> regionName(infos.size());
    std::vector<int> side(infos.size(), 0);  // 0 none, -1 left, 1 right
    for (size_t i = 0; i < infos.size(); ++i) {
        const BodyPartInfo& info = infos[i];
        const Def* def = defs_.find(kWoundBodyPartsKind, info.def);
        PartData& part = parts[i];
        part.inside = info.inside;
        part.def = def != nullptr ? def->id() : DefId{};
        part.bleedRate = def != nullptr ? static_cast<float>(def->number("bleedRate", 1.0)) : 1.0f;
        const size_t parent = info.parent >= 0 ? static_cast<size_t>(info.parent) : infos.size();
        regionName[i] = def != nullptr ? def->text("skinRegion") : std::string();
        if (regionName[i].empty() && parent < i) {
            regionName[i] = regionName[parent];
        }
        side[i] = woundLabelHas(info.label, "left") ? -1 : woundLabelHas(info.label, "right") ? 1 : 0;
        if (side[i] == 0 && parent < i) {
            side[i] = side[parent];
        }
        if (regionName[i] == "Head") {
            part.region = BodyRegion::Head;
        } else if (regionName[i] == "Arm") {
            part.region = side[i] > 0 ? BodyRegion::RightArm : BodyRegion::LeftArm;
        } else if (regionName[i] == "Leg") {
            part.region = side[i] > 0 ? BodyRegion::RightLeg : BodyRegion::LeftLeg;
        } else {
            part.region = BodyRegion::Torso;
        }
    }
    return bodies_.emplace(pawnDef.value, std::move(parts)).first->second;
}

void WoundViews::append(Entity pawn, DefId pawnDef, const Health& health, std::vector<WoundView>& out) {
    if (health.injuries.empty() && health.missing.empty()) {
        return;
    }
    const std::vector<PartData>& parts = body(pawnDef);
    for (const Injury& injury : health.injuries) {
        if (injury.part >= parts.size() || parts[injury.part].inside || injury.severity <= 0.0f) {
            continue;
        }
        const PartData& part = parts[injury.part];
        const HediffData& data = hediff(injury.hediff);
        WoundView wound;
        wound.part = injury.part;
        wound.partDef = part.def;
        wound.region = part.region;
        wound.kind = data.kind;
        const float maxHp = health_.partMaxHitPoints(pawn, injury.part);
        wound.severity = maxHp > 0.0f ? std::clamp(injury.severity / maxHp, 0.0f, 1.0f) : 1.0f;
        wound.tended = injury.tended();
        wound.tendQuality = injury.tendQuality;
        wound.bleeding = !injury.tended() && data.bleedRate * part.bleedRate > 0.0f;
        out.push_back(wound);
    }
    for (const MissingPart& missing : health.missing) {
        if (missing.part >= parts.size() || parts[missing.part].inside) {
            continue;
        }
        const PartData& part = parts[missing.part];
        WoundView wound;
        wound.part = missing.part;
        wound.partDef = part.def;
        wound.region = part.region;
        wound.kind = WoundKind::Missing;
        wound.severity = 1.0f;
        wound.tended = missing.tended();
        wound.tendQuality = missing.tendQuality;
        wound.bleeding = !missing.tended() && part.bleedRate > 0.0f;
        out.push_back(wound);
    }
}

}  // namespace colony
