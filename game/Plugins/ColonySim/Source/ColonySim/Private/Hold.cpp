#include "ColonySim/Hold.h"

#include <string>

namespace colony {

namespace {

HoldPoint holdPointAt(const Def& def, const std::string& path, const HoldPoint& fallback) {
    if (def.count(path.c_str()) < 3) {
        return fallback;
    }
    HoldPoint p;
    p.x = static_cast<float>(def.numberAt(path.c_str(), 0));
    p.y = static_cast<float>(def.numberAt(path.c_str(), 1));
    p.z = static_cast<float>(def.numberAt(path.c_str(), 2));
    return p;
}

HoldGrip holdGripFrom(const std::string& text) {
    if (text == "None") return HoldGrip::None;
    if (text == "Gun") return HoldGrip::Gun;
    if (text == "Bow") return HoldGrip::Bow;
    if (text == "Shield") return HoldGrip::Shield;
    if (text == "Pole") return HoldGrip::Pole;
    return HoldGrip::Blade;
}

// Fills `out` from a HoldStyles def: hands, hand, grip, attack style and the style's default grip points.
bool holdFromStyle(const DefDatabase& defs, const std::string& name, WeaponHold& out) {
    const Def* style = defs.find("HoldStyles", name);
    if (style == nullptr) {
        return false;
    }
    out.style = style->id();
    out.styleName = style->defName();
    out.attackStyle = style->text("attackStyle");
    out.hands = static_cast<int>(style->integer("hands", 1));
    out.hand = style->text("hand", "Right") == "Left" ? HoldHand::Left : HoldHand::Right;
    out.grip = holdGripFrom(style->text("grip", "Blade"));
    out.main = holdPointAt(*style, "main", HoldPoint{});
    out.hasOff = style->count("off") >= 3;
    out.off = holdPointAt(*style, "off", HoldPoint{});
    out.rake = static_cast<float>(style->number("rake", 0.0));
    return true;
}

}  // namespace

bool holdFor(const DefDatabase& defs, DefId weapon, WeaponHold& out) {
    out = WeaponHold{};
    const Def* def = weapon.valid() && weapon.value < defs.size() ? &defs.get(weapon) : nullptr;
    if (def != nullptr && def->kind() != "Things") {
        def = nullptr;
    }
    // A weapon's own style, else one by its kind; nothing in the hands is fists.
    std::string styleName;
    if (def != nullptr) {
        styleName = def->text("hold.style");
        if (styleName.empty() || defs.find("HoldStyles", styleName) == nullptr) {
            const bool twoHanded = def->flag("twoHanded");
            if (def->has("verbs")) {
                styleName = twoHanded ? "Hold_Rifle" : "Hold_Pistol";
            } else if (def->count("tools") > 0 && !def->has("race")) {
                styleName = twoHanded ? "Hold_TwoHandBlade" : "Hold_OneHandBlade";
            } else {
                def = nullptr;
            }
        }
    }
    if (def == nullptr) {
        styleName = "Hold_Unarmed";
    }
    if (!holdFromStyle(defs, styleName, out)) {
        return false;
    }
    if (def != nullptr) {
        out.main = holdPointAt(*def, "hold.main", out.main);
        if (def->count("hold.off") >= 3) {
            out.off = holdPointAt(*def, "hold.off", out.off);
            out.hasOff = true;
        }
        out.rake = static_cast<float>(def->number("hold.rake", out.rake));
    }
    // One-handed styles never put the other hand on (a def's stray off point is ignored).
    if (out.hands < 2) {
        out.hasOff = false;
    }
    return true;
}

bool shieldHoldFor(const DefDatabase& defs, DefId shield, WeaponHold& out) {
    out = WeaponHold{};
    const Def* def = shield.valid() && shield.value < defs.size() ? &defs.get(shield) : nullptr;
    if (def == nullptr || !def->has("gear.shield")) {
        return false;
    }
    const std::string styleName = def->text("hold.style", "Hold_Shield");
    if (!holdFromStyle(defs, styleName, out)) {
        return false;
    }
    out.main = holdPointAt(*def, "hold.main", out.main);
    out.hasOff = false;
    return true;
}

const char* holdGripName(HoldGrip grip) {
    switch (grip) {
        case HoldGrip::None: return "None";
        case HoldGrip::Blade: return "Blade";
        case HoldGrip::Gun: return "Gun";
        case HoldGrip::Bow: return "Bow";
        case HoldGrip::Shield: return "Shield";
        case HoldGrip::Pole: return "Pole";
    }
    return "Blade";
}

}  // namespace colony
