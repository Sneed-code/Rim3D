#include "ColonySim/Genes.h"

#include "ColonySim/Defs.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

void genesAdd(std::vector<std::pair<std::string, float>>& list, const std::string& stat, float value, bool multiply) {
    for (auto& entry : list) {
        if (entry.first == stat) {
            entry.second = multiply ? entry.second * value : entry.second + value;
            return;
        }
    }
    list.emplace_back(stat, value);
}

float genesLookup(const std::vector<std::pair<std::string, float>>& list, const char* stat, float fallback) {
    for (const auto& entry : list) {
        if (entry.first == stat) {
            return entry.second;
        }
    }
    return fallback;
}

// The hunger rate for a metabolic efficiency, along the GeneRules curve [[x, y], ...].
float genesHungerRate(const Def& rules, float metabolism) {
    const size_t n = rules.count("hungerRateByMetabolicEfficiency");
    float beforeX = 0.0f;
    float beforeY = 1.0f;
    for (size_t i = 0; i < n; ++i) {
        const std::string point = "hungerRateByMetabolicEfficiency." + std::to_string(i);
        const float x = static_cast<float>(rules.numberAt(point.c_str(), 0, 0.0));
        const float y = static_cast<float>(rules.numberAt(point.c_str(), 1, 1.0));
        if (metabolism <= x) {
            if (i == 0 || x <= beforeX) {
                return y;
            }
            return beforeY + (y - beforeY) * (metabolism - beforeX) / (x - beforeX);
        }
        beforeX = x;
        beforeY = y;
    }
    return n == 0 ? 1.0f : beforeY;
}

}  // namespace

bool makeGenes(const DefDatabase& defs, const std::string& lineage, PawnGenes& out, std::string& error) {
    out = PawnGenes{};
    const Def* lineageDef = defs.find("Lineages", lineage);
    if (lineageDef == nullptr) {
        error = "no Lineages def '" + lineage + "'";
        return false;
    }
    out.metabolicEfficiency = static_cast<float>(lineageDef->number("metabolicEfficiency", 0.0));
    bool ok = true;
    for (size_t i = 0; i < lineageDef->count("genes"); ++i) {
        const std::string name = lineageDef->textAt("genes", i);
        const Def* gene = defs.find("Genes", name);
        if (gene == nullptr) {
            error = "lineage '" + lineage + "': no Genes def '" + name + "'";
            ok = false;
            continue;
        }
        out.genes.push_back(name);
        for (const std::string& stat : gene->keys("statOffsets")) {
            genesAdd(out.offsets, stat, static_cast<float>(gene->number(("statOffsets." + stat).c_str(), 0.0)), false);
        }
        for (const std::string& stat : gene->keys("statFactors")) {
            genesAdd(out.factors, stat, static_cast<float>(gene->number(("statFactors." + stat).c_str(), 1.0)), true);
        }
        for (const std::string& skill : gene->keys("aptitudes")) {
            const int levels = static_cast<int>(gene->integer(("aptitudes." + skill).c_str(), 0));
            bool found = false;
            for (auto& entry : out.aptitudes) {
                if (entry.first == skill) {
                    entry.second += levels;
                    found = true;
                }
            }
            if (!found) {
                out.aptitudes.emplace_back(skill, levels);
            }
        }
    }

    GeneEffects& fx = out.effects;
    fx.moveSpeed = geneStat(out, "moveSpeed", 0.0f);
    fx.comfyMinC = geneStat(out, "comfyTemperatureMin", 0.0f);
    fx.comfyMaxC = geneStat(out, "comfyTemperatureMax", 0.0f);
    fx.moodOffset = geneStat(out, "moodOffset", 0.0f);
    fx.damageTaken = geneStat(out, "damageTaken", 1.0f);
    fx.woundHealing = geneStat(out, "woundHealing", 1.0f);
    fx.miningSpeed = geneStat(out, "miningSpeed", 1.0f);
    fx.plantWorkSpeed = geneStat(out, "plantWorkSpeed", 1.0f);
    fx.foodRate = geneStat(out, "foodRate", 1.0f);
    fx.moveSpeedFactor = std::max(0.0f, geneStat(out, "moveSpeedFactor", 1.0f));
    fx.learning = std::max(0.0f, geneStat(out, "learningRate", 1.0f));
    fx.workSpeed = std::max(0.0f, geneStat(out, "workSpeed", 1.0f));
    fx.meleeDamage = std::max(0.0f, geneStat(out, "meleeDamage", 1.0f));
    fx.rangedDamage = std::max(0.0f, geneStat(out, "rangedDamage", 1.0f));
    fx.shootingAccuracy = std::max(0.05f, geneStat(out, "shootingAccuracy", 1.0f));
    fx.healthScale = std::max(0.1f, geneStat(out, "healthScale", 1.0f));
    fx.thermalDamage = std::max(0.0f, geneStat(out, "thermalDamage", 1.0f));
    fx.environmentDamage = std::max(0.0f, geneStat(out, "environmentDamage", 1.0f));
    fx.debuffFactor = std::max(0.0f, geneStat(out, "debuffFactor", 1.0f));
    fx.jumpHeight = std::max(0.0f, geneStat(out, "jumpHeight", 1.0f));
    fx.climbSpeed = std::max(0.0f, geneStat(out, "climbSpeed", 1.0f));
    fx.darkVision = geneStat(out, "darknessSlowdown", 1.0f) <= 0.0f;
    fx.darkWorkSpeed = std::max(0.0f, geneStat(out, "darkWorkSpeed", 1.0f));
    fx.sunWorkSpeed = std::max(0.0f, geneStat(out, "sunWorkSpeed", 1.0f));
    fx.sunShootingAccuracy = std::max(0.05f, geneStat(out, "sunShootingAccuracy", 1.0f));
    fx.noOutdoorsNeed = geneStat(out, "noOutdoorsNeed", 0.0f) > 0.0f;
    fx.nocturnal = geneStat(out, "nocturnal", 0.0f) > 0.0f;
    if (const Def* rules = defs.find("Rules", "GeneRules")) {
        fx.foodRate *= genesHungerRate(*rules, out.metabolicEfficiency);
        fx.darkBelow = static_cast<float>(rules->number("darkBelowGlow", fx.darkBelow));
        fx.sunAbove = static_cast<float>(rules->number("sunAboveGlow", fx.sunAbove));
    }
    return ok;
}

float geneStat(const PawnGenes& genes, const char* stat, float base) {
    return (base + genesLookup(genes.offsets, stat, 0.0f)) * genesLookup(genes.factors, stat, 1.0f);
}

float geneWorkFactor(const GeneEffects& fx, float glow, bool sunlit) {
    return fx.workSpeed * (glow < fx.darkBelow ? fx.darkWorkSpeed : 1.0f) * (sunlit ? fx.sunWorkSpeed : 1.0f);
}

int geneAptitude(const PawnGenes& genes, const std::string& skill) {
    for (const auto& entry : genes.aptitudes) {
        if (entry.first == skill) {
            return entry.second;
        }
    }
    return 0;
}

const Def* lineageGeneWith(const DefDatabase& defs, const Def& lineage, const char* path) {
    const Def* found = nullptr;
    for (size_t i = 0; i < lineage.count("genes"); ++i) {
        const Def* gene = defs.find("Genes", lineage.textAt("genes", i));
        if (gene != nullptr && gene->has(path)) {
            found = gene;
        }
    }
    return found;
}

}  // namespace colony
