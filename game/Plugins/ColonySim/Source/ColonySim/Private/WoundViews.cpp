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

const char* woundRegionName(WoundRegion region) {
    switch (region) {
        case WoundRegion::Torso: return "Torso";
        case WoundRegion::Head: return "Head";
        case WoundRegion::LeftArm: return "LeftArm";
        case WoundRegion::RightArm: return "RightArm";
        case WoundRegion::LeftLeg: return "LeftLeg";
        case WoundRegion::RightLeg: return "RightLeg";
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
            part.region = WoundRegion::Head;
        } else if (regionName[i] == "Arm") {
            part.region = side[i] > 0 ? WoundRegion::RightArm : WoundRegion::LeftArm;
        } else if (regionName[i] == "Leg") {
            part.region = side[i] > 0 ? WoundRegion::RightLeg : WoundRegion::LeftLeg;
        } else {
            part.region = WoundRegion::Torso;
        }
    }
    return bodies_.emplace(pawnDef.value, std::move(parts)).first->second;
}

bool WoundViews::diseaseShows(const Disease& disease) const {
    if (!disease.def.valid() || disease.def.value >= defs_.size()) {
        return false;
    }
    // The stage it is in (the last whose minSeverity it has reached) decides, as for the health panel.
    const Def& def = defs_.get(disease.def);
    int stage = -1;
    for (size_t i = 0; i < def.count("stages"); ++i) {
        if (disease.severity >= def.number(("stages." + std::to_string(i) + ".minSeverity").c_str())) {
            stage = static_cast<int>(i);
        }
    }
    return stage < 0 || def.flag(("stages." + std::to_string(stage) + ".visible").c_str(), true);
}

void WoundViews::append(Entity pawn, DefId pawnDef, const Health& health, std::vector<WoundView>& out) {
    if (health.injuries.empty() && health.missing.empty() && health.diseases.empty()) {
        return;
    }
    const std::vector<PartData>& parts = body(pawnDef);
    const size_t firstOfPawn = out.size();
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
    // A wound that has turned septic (H2: a Disease with the wound's part) is drawn as an infection once its stage is
    // visible: the worst-looking wound on that part, or a new one if the wound itself is gone.
    for (const Disease& disease : health.diseases) {
        if (disease.part < 0 || static_cast<size_t>(disease.part) >= parts.size() || parts[static_cast<size_t>(disease.part)].inside ||
            !diseaseShows(disease)) {
            continue;
        }
        const uint16_t part = static_cast<uint16_t>(disease.part);
        WoundView* host = nullptr;
        for (size_t i = firstOfPawn; i < out.size(); ++i) {
            WoundView& wound = out[i];
            if (wound.part == part && wound.kind != WoundKind::Scar && wound.kind != WoundKind::Missing &&
                (host == nullptr || wound.severity > host->severity)) {
                host = &wound;
            }
        }
        if (host == nullptr) {
            const PartData& data = parts[part];
            WoundView wound;
            wound.part = part;
            wound.partDef = data.def;
            wound.region = data.region;
            wound.severity = std::clamp(disease.severity, 0.0f, 1.0f);
            wound.tended = disease.tendQuality >= 0.0f;
            wound.tendQuality = disease.tendQuality;
            out.push_back(wound);
            host = &out.back();
        }
        host->kind = WoundKind::Infection;
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
