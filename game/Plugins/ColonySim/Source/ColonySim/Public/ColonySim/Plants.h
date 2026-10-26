#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace colony {

class Map;

// Growth and age are whole numbers so the layer hashes and saves the same on every platform.
inline constexpr uint32_t kPlantFullGrowth = 1000000;  // growth in millionths: 1,000,000 is fully grown

// One plant as saved: the cell's index in the map, its def and how far it has come.
struct PlantRecord {
    int cell = 0;
    DefId def;
    uint32_t growth = 0;    // millionths, 0 to kPlantFullGrowth
    uint32_t ageHours = 0;  // game hours since it sprouted
};

// The plant layer: at most one plant per cell, kept as flat arrays (type, growth, age) because a
// 1 km map holds hundreds of thousands of them (docs/design/plants.md). Plants are not things.
//
// Made from the Things defs of category Plant and the biome def: map generation scatters wild
// plants by fertility, and growHour() grows them. Jobs that cut or harvest plants (C3) and
// buildings that clear them (C4) change the layer through place(), remove() and setGrowth().
class COLONY_API Plants {
public:
    // Fills the layer for a new map. With no plant defs, no terrain defs or no biome of this name
    // the layer is empty. `seed` makes generation repeatable; growth uses no randomness.
    Plants(const Map& map, const DefDatabase& defs, uint64_t seed, const std::string& biome);

    // Fertility of the ground in a cell (1.0 is plain soil; 0 where nothing can grow).
    float fertility(CellPos c) const;

    bool has(CellPos c) const { return kind_[indexOf(c)] != 0; }
    // The plant's def, or an invalid DefId if the cell has none.
    DefId defAt(CellPos c) const;
    // 0 to 1; 0 if the cell has no plant.
    float growth(CellPos c) const { return static_cast<float>(growthPpm(c)) / static_cast<float>(kPlantFullGrowth); }
    uint32_t growthPpm(CellPos c) const { return growth_[indexOf(c)]; }
    uint32_t ageHours(CellPos c) const { return age_[indexOf(c)]; }
    bool mature(CellPos c) const { return has(c) && growthPpm(c) >= kPlantFullGrowth; }
    // Hours a plant of this def lives (grow days times its lifespan factor, in game hours).
    uint32_t lifespanHours(DefId def) const;

    // Puts a plant in an empty cell with fertility above 0. Returns false otherwise (or for a def
    // that is not a plant). `growth` is clamped to 0..1.
    bool place(CellPos c, DefId def, float growth = 0.0f);
    void remove(CellPos c);
    // Sets the growth of the plant in the cell (e.g. a harvested berry bush goes back to
    // `plant.harvestAfterGrowth`). Does nothing if the cell has no plant.
    void setGrowth(CellPos c, float growth);

    // One game hour has passed, and it began at `hourOfDay` (0..23): every plant gets an hour
    // older, plants past their lifespan die, and in the growing hours the rest grow by
    // 1 / (growDays x 24) of full growth times their fertility rate.
    void growHour(int hourOfDay);
    // Whether plants grow in the hour that begins at `hourOfDay` (0..23), and the data's growth multiplier.
    bool growsAtHour(int hourOfDay) const { return hourOfDay >= growStartHour_ && hourOfDay < growEndHour_; }
    double growthTuning() const { return growthTuning_; }
    // Scales a plant's growth in a cell (by the cell's index) by a factor of 0 to 1; farming uses it for
    // the light and temperature of sown crops. Not saved: whoever installs it does so again on loading.
    void setGrowthModifier(std::function<double(size_t)> modifier) { modifier_ = std::move(modifier); }
    // Plants in a cell for which this says true grow in every hour, not only the growing hours (a crop under a
    // sun lamp). Not saved.
    void setGrowsAnytime(std::function<bool(size_t)> anytime) { anytime_ = std::move(anytime); }
    // A cell whose fertility is not the ground's: a plant pot or hydroponics basin gives its own. The function
    // takes the cell's index and returns the fertility, or a negative number for none. Not saved.
    void setFertilityOverride(std::function<float(size_t)> override) { fertilityOverride_ = std::move(override); }

    size_t count() const { return count_; }
    size_t countOf(DefId def) const;
    // Fingerprint of the layer; kept up to date as plants change, so asking costs nothing.
    uint64_t hash() const { return hash_ ^ static_cast<uint64_t>(count_); }

    // For saves: every plant in cell order, and a way to put them back.
    std::vector<PlantRecord> records() const;
    // Replaces the layer. Returns false (leaving it empty) if a record is off the map, repeated
    // or names something that is not a plant.
    bool restore(const std::vector<PlantRecord>& records);

private:
    struct Kind {
        DefId def;
        double growthPerHour = 0.0;  // millionths per growing hour at fertility rate 1
        float fertilityMin = 0.0f;
        float fertilitySensitivity = 0.0f;
        uint32_t lifespanHours = 0;
        double commonality = 0.0;  // weight in the biome's wild plants; 0 if absent
        bool isTree = false;
    };

    size_t indexOf(CellPos c) const;
    float fertilityAtIndex(size_t i) const;
    int kindOfDef(DefId def) const;
    uint64_t cellHash(size_t i) const;
    void set(size_t i, uint16_t kind, uint32_t growth, uint32_t age);
    void generate(uint64_t seed, double density);

    const Map& map_;
    std::vector<Kind> kinds_;
    std::vector<int> kindOfDef_;  // by DefId.value: index into kinds_, or -1
    double growthTuning_ = 1.0;
    int growStartHour_ = 6;
    int growEndHour_ = 19;
    std::vector<float> fertility_;
    std::vector<uint16_t> kind_;  // 0: no plant, otherwise index into kinds_ plus 1
    std::vector<uint32_t> growth_;
    std::vector<uint32_t> age_;
    size_t count_ = 0;
    uint64_t hash_ = 0;
    std::function<double(size_t)> modifier_;
    std::function<bool(size_t)> anytime_;
    std::function<float(size_t)> fertilityOverride_;
};

}  // namespace colony
