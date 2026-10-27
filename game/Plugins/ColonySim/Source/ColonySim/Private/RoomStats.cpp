#include "ColonySim/RoomStats.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Apparel.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Filth.h"
#include "ColonySim/Floors.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

float roomStatsFloat(const Def& def, const char* path, float fallback) {
    return static_cast<float>(def.number(path, fallback));
}

// The wiki's soft log for impressiveness: values outside (-1, 1) grow slowly.
float roomStatsSoftLog(float b) {
    if (b >= 1.0f) {
        return 1.0f + std::log(b);
    }
    if (b <= -1.0f) {
        return -(1.0f + std::log(-b));
    }
    return b;
}

struct RoomStatsStage {
    float below;
    const char* label;
};

template <size_t N>
const char* roomStatsLabel(const RoomStatsStage (&stages)[N], float value, const char* last) {
    for (const RoomStatsStage& stage : stages) {
        if (value < stage.below) {
            return stage.label;
        }
    }
    return last;
}

}  // namespace

const char* beautyLabel(float beauty) {
    static const RoomStatsStage kStages[] = {{-3.5f, "hideous"}, {0.0f, "ugly"},          {2.4f, "neutral"},
                                             {5.0f, "pretty"},   {15.0f, "beautiful"},    {50.0f, "very beautiful"},
                                             {100.0f, "extremely beautiful"}};
    return roomStatsLabel(kStages, beauty, "unbelievably beautiful");
}

const char* spaceLabel(float space) {
    static const RoomStatsStage kStages[] = {{12.5f, "cramped"},        {29.0f, "rather tight"},    {55.0f, "average-sized"},
                                             {70.0f, "somewhat spacious"}, {130.0f, "quite spacious"}, {349.5f, "very spacious"}};
    return roomStatsLabel(kStages, space, "extremely spacious");
}

const char* wealthLabel(float wealth) {
    static const RoomStatsStage kStages[] = {{500.0f, "impoverished"},   {700.0f, "somewhat poor"},     {2000.0f, "mediocre"},
                                             {4000.0f, "somewhat rich"}, {10000.0f, "rich"},            {40000.0f, "luxurious"},
                                             {100000.0f, "very luxurious"}, {1000000.0f, "extremely luxurious"}};
    return roomStatsLabel(kStages, wealth, "unbelievably luxurious");
}

const char* cleanlinessLabel(float cleanliness) {
    static const RoomStatsStage kStages[] = {{-1.1f, "very dirty"}, {-0.4f, "dirty"}, {-0.05f, "slightly dirty"}, {0.4f, "clean"}};
    return roomStatsLabel(kStages, cleanliness, "sterile");
}

const char* impressivenessLabel(float impressiveness) {
    static const RoomStatsStage kStages[] = {{20.0f, "awful"},  {30.0f, "dull"},   {40.0f, "mediocre"},          {50.0f, "decent"},
                                             {65.0f, "slightly impressive"},       {85.0f, "somewhat impressive"},
                                             {120.0f, "very impressive"},          {170.0f, "extremely impressive"},
                                             {240.0f, "unbelievably impressive"}};
    return roomStatsLabel(kStages, std::floor(impressiveness), "wondrously impressive");
}

float roomImpressiveness(const RoomStatNumbers& numbers, float wealth, float beauty, float space, float cleanliness) {
    const float w = roomStatsSoftLog(wealth / numbers.wealthDivisor);
    const float b = roomStatsSoftLog(beauty / numbers.beautyDivisor);
    const float s = roomStatsSoftLog(space / numbers.spaceDivisor);
    const float c = roomStatsSoftLog(numbers.cleanlinessBase + cleanliness / numbers.cleanlinessDivisor);
    float impressive = numbers.averageWeight * (w + b + s + c) / 4.0f + numbers.minWeight * std::min({w, b, s, c});
    const float cap = numbers.spaceCap * s;
    if (impressive > cap) {
        impressive = numbers.spaceCapKeep * impressive + (1.0f - numbers.spaceCapKeep) * cap;
    }
    return impressive;
}

RoomStatNumbers RoomStatNumbers::read(const DefDatabase& defs) {
    RoomStatNumbers n;
    const Def* def = defs.find("Rules", "RoomStatRules");
    if (def == nullptr) {
        return n;
    }
    n.beautyFullSizeCells = static_cast<int>(def->integer("beauty.fullSizeCells", n.beautyFullSizeCells));
    n.beautySmallBase = roomStatsFloat(*def, "beauty.smallBase", n.beautySmallBase);
    n.beautySmallHalf = roomStatsFloat(*def, "beauty.smallHalf", n.beautySmallHalf);
    n.spacePerCell = roomStatsFloat(*def, "space.perCell", n.spacePerCell);
    n.spaceBlockerReduction = roomStatsFloat(*def, "space.blockerReduction", n.spaceBlockerReduction);
    n.wealthDivisor = roomStatsFloat(*def, "impressiveness.wealthDivisor", n.wealthDivisor);
    n.beautyDivisor = roomStatsFloat(*def, "impressiveness.beautyDivisor", n.beautyDivisor);
    n.spaceDivisor = roomStatsFloat(*def, "impressiveness.spaceDivisor", n.spaceDivisor);
    n.cleanlinessBase = roomStatsFloat(*def, "impressiveness.cleanlinessBase", n.cleanlinessBase);
    n.cleanlinessDivisor = roomStatsFloat(*def, "impressiveness.cleanlinessDivisor", n.cleanlinessDivisor);
    n.averageWeight = roomStatsFloat(*def, "impressiveness.averageWeight", n.averageWeight);
    n.minWeight = roomStatsFloat(*def, "impressiveness.minWeight", n.minWeight);
    n.spaceCap = roomStatsFloat(*def, "impressiveness.spaceCap", n.spaceCap);
    n.spaceCapKeep = roomStatsFloat(*def, "impressiveness.spaceCapKeep", n.spaceCapKeep);
    n.workTicksFactor = roomStatsFloat(*def, "marketValue.workTicksFactor", n.workTicksFactor);
    n.ticksPerWorkSecond = roomStatsFloat(*def, "marketValue.ticksPerWorkSecond", n.ticksPerWorkSecond);
    return n;
}

BeautyNeedNumbers BeautyNeedNumbers::read(const DefDatabase& defs) {
    BeautyNeedNumbers n;
    const Def* def = defs.find("Rules", "BeautyNeedRules");
    if (def == nullptr) {
        return n;
    }
    n.present = true;
    n.sightRadius = roomStatsFloat(*def, "sightRadius", n.sightRadius);
    n.neutralMin = roomStatsFloat(*def, "neutralMin", n.neutralMin);
    n.neutralMax = roomStatsFloat(*def, "neutralMax", n.neutralMax);
    n.beautyAtNeutralMin = roomStatsFloat(*def, "beautyAtNeutralMin", n.beautyAtNeutralMin);
    n.beautyAtNeutralMax = roomStatsFloat(*def, "beautyAtNeutralMax", n.beautyAtNeutralMax);
    n.risePerHour = roomStatsFloat(*def, "risePerHour", n.risePerHour);
    n.fallPerHour = roomStatsFloat(*def, "fallPerHour", n.fallPerHour);
    n.sampleEverySeconds = std::max(1, static_cast<int>(def->integer("sampleEverySeconds", n.sampleEverySeconds)));
    return n;
}

RoomStats::RoomStats(Map& map, Things& things, Rooms& rooms, const Floors& floors, const Filth& filth,
                     const Plants& plants, const DefDatabase& defs)
    : map_(map),
      things_(things),
      rooms_(rooms),
      floors_(floors),
      filth_(filth),
      plants_(plants),
      defs_(defs),
      numbers_(RoomStatNumbers::read(defs)),
      need_(BeautyNeedNumbers::read(defs)) {
    const size_t n = defs.size();
    beautyIndoors_.assign(n, 0.0f);
    beautyOutdoors_.assign(n, 0.0f);
    cleanliness_.assign(n, 0.0f);
    baseValue_.assign(n, 0.0f);
    stuffBeauty_.assign(n, 1.0f);
    stuffBeautyOffset_.assign(n, 0.0f);
    art_.assign(n, 0);
    stuffCount_.assign(n, 0.0f);
    standable_.assign(n, 0);
    impassable_.assign(n, 0);
    for (const char* kind : {"Things", "Terrain"}) {
        for (const DefId id : defs.idsOfKind(kind)) {
            const Def& def = defs.get(id);
            const size_t i = id.value;
            beautyIndoors_[i] = roomStatsFloat(def, "stats.beauty", 0.0f);
            beautyOutdoors_[i] = roomStatsFloat(def, "stats.beautyOutdoors", beautyIndoors_[i]);
            cleanliness_[i] = roomStatsFloat(def, "stats.cleanliness", 0.0f);
            stuffBeauty_[i] = roomStatsFloat(def, "stuffProps.statFactors.beauty", 1.0f);
            stuffBeautyOffset_[i] = roomStatsFloat(def, "stuffProps.statOffsets.beauty", 0.0f);
            art_[i] = def.flag("building.art") ? 1 : 0;
            stuffCount_[i] = roomStatsFloat(def, "costStuffCount", 0.0f);
            const std::string passability = def.text("passability");
            standable_[i] = passability == "Standable" ? 1 : 0;
            impassable_[i] = passability == "Impassable" ? 1 : 0;
            // Market value: the stat if the def has one, else its materials (not the stuff) plus
            // 0.0036 for each RimWorld tick of work.
            float value = 0.0f;
            if (def.has("stats.marketValue")) {
                value = roomStatsFloat(def, "stats.marketValue", 0.0f);
            } else {
                for (const std::string& name : def.keys("costs")) {
                    const Def* material = defs.find("Things", name);
                    if (material != nullptr) {
                        value += roomStatsFloat(def, ("costs." + name).c_str(), 0.0f) *
                                 roomStatsFloat(*material, "stats.marketValue", 0.0f);
                    }
                }
                const float work = def.has("stats.workToBuild") ? roomStatsFloat(def, "stats.workToBuild", 0.0f)
                                                                : roomStatsFloat(def, "workToBuild", 0.0f);
                value += work * numbers_.ticksPerWorkSecond * numbers_.workTicksFactor;
            }
            baseValue_[i] = value;
        }
    }
    if (const Def* rules = defs.find("Rules", "QualityRules")) {
        const std::string defaultLevel = rules->text("defaultLevel", "Normal");
        for (size_t i = 0; i < rules->count("levels"); ++i) {
            const std::string path = "levels." + std::to_string(i);
            qualityBeauty_.push_back(roomStatsFloat(*rules, (path + ".beautyOffset").c_str(), 1.0f));
            qualityValue_.push_back(roomStatsFloat(*rules, (path + ".marketValueFactor").c_str(), 1.0f));
            if (rules->text((path + ".def").c_str()) == defaultLevel) {
                defaultQuality_ = static_cast<int>(i);
            }
        }
    }
    if (qualityBeauty_.empty()) {
        qualityBeauty_.push_back(1.0f);
        qualityValue_.push_back(1.0f);
        defaultQuality_ = 0;
    }
    defaultQuality_ = std::clamp(defaultQuality_, 0, static_cast<int>(qualityBeauty_.size()) - 1);
    groundDef_[static_cast<size_t>(Terrain::Water)] = defs.idOf("Terrain", "WaterShallow");
    groundDef_[static_cast<size_t>(Terrain::Sand)] = defs.idOf("Terrain", "Sand");
    groundDef_[static_cast<size_t>(Terrain::Soil)] = defs.idOf("Terrain", "Soil");
    groundDef_[static_cast<size_t>(Terrain::Rock)] = DefId{};
    groundDef_[static_cast<size_t>(Terrain::RoughStone)] = defs.idOf("Terrain", "Granite_Rough");
    const float r = need_.sightRadius;
    const int reach = static_cast<int>(std::ceil(r));
    for (int dy = -reach; dy <= reach; ++dy) {
        for (int dx = -reach; dx <= reach; ++dx) {
            if (static_cast<float>(dx * dx + dy * dy) <= r * r) {
                sightOffsets_.emplace_back(dx, dy);
            }
        }
    }
}

float RoomStats::statBeauty(DefId def, bool outdoors) const {
    if (!def.valid()) {
        return 0.0f;
    }
    return outdoors ? beautyOutdoors_[def.value] : beautyIndoors_[def.value];
}

float RoomStats::marketValueOf(Entity building) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(building) || !reg.all_of<Thing>(building)) {
        return 0.0f;
    }
    const DefId def = reg.get<Thing>(building).def;
    float value = baseValue_[def.value];
    if (const BuiltOf* made = reg.try_get<BuiltOf>(building); made != nullptr && made->stuff.valid()) {
        value += stuffCount_[def.value] * roomStatsFloat(defs_.get(made->stuff), "stats.marketValue", 0.0f);
    }
    if (art_[def.value] != 0) {
        const Quality* quality = reg.try_get<Quality>(building);
        const size_t level = quality != nullptr ? std::min(static_cast<size_t>(quality->level), qualityValue_.size() - 1)
                                                : static_cast<size_t>(defaultQuality_);
        value *= qualityValue_[level];
    }
    return value;
}

float RoomStats::buildingBeauty(Entity building, bool outdoors) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(building) || !reg.all_of<Thing>(building)) {
        return 0.0f;
    }
    const DefId def = reg.get<Thing>(building).def;
    float beauty = statBeauty(def, outdoors);
    if (const BuiltOf* made = reg.try_get<BuiltOf>(building); made != nullptr && made->stuff.valid()) {
        beauty = beauty * stuffBeauty_[made->stuff.value] + stuffBeautyOffset_[made->stuff.value];
    }
    if (art_[def.value] != 0) {
        const Quality* quality = reg.try_get<Quality>(building);
        const size_t level = quality != nullptr ? std::min(static_cast<size_t>(quality->level), qualityBeauty_.size() - 1)
                                                : static_cast<size_t>(defaultQuality_);
        beauty *= qualityBeauty_[level];
    }
    return beauty;
}

float RoomStats::marketValueOfFloor(DefId floor) const {
    return floor.valid() ? baseValue_[floor.value] : 0.0f;
}

float RoomStats::cellBeauty(CellPos c, bool withGround, std::vector<Entity>* counted) {
    if (!map_.inBounds(c)) {
        return 0.0f;
    }
    const bool outdoors = !rooms_.indoors(c);
    const entt::registry& reg = things_.registry();
    float total = 0.0f;
    bool walled = false;
    const Entity building = map_.buildingAt(c);
    if (building != kNoEntity && reg.valid(building) && !reg.all_of<ConstructionSite>(building)) {
        const DefId def = reg.get<Thing>(building).def;
        walled = impassable_[def.value] != 0;
        if (counted == nullptr || std::find(counted->begin(), counted->end(), building) == counted->end()) {
            total += buildingBeauty(building, outdoors);
            if (counted != nullptr) {
                counted->push_back(building);
            }
        }
    }
    for (const Entity item : things_.itemsAt(c)) {
        total += statBeauty(reg.get<Thing>(item).def, outdoors);
    }
    total += statBeauty(plants_.defAt(c), outdoors);
    total += filth_.beautyAt(c, outdoors);
    if (withGround && !walled) {
        const DefId floor = floors_.at(c);
        total += statBeauty(floor.valid() ? floor : groundDef_[static_cast<size_t>(map_.terrain(c))], outdoors);
    }
    return total;
}

float RoomStats::cellCleanliness(CellPos c) {
    if (!map_.inBounds(c)) {
        return 0.0f;
    }
    const entt::registry& reg = things_.registry();
    float total = filth_.cleanlinessAt(c);
    const Entity building = map_.buildingAt(c);
    if (building != kNoEntity && reg.valid(building) && !reg.all_of<ConstructionSite>(building)) {
        total += cleanliness_[reg.get<Thing>(building).def.value];
    }
    const DefId floor = floors_.at(c);
    const DefId ground = floor.valid() ? floor : groundDef_[static_cast<size_t>(map_.terrain(c))];
    if (ground.valid()) {
        total += cleanliness_[ground.value];
    }
    return total;
}

float RoomStats::perceivedBeauty(CellPos from) {
    float sum = 0.0f;
    int count = 0;
    std::vector<Entity> counted;
    const Viewpoint eye = Viewpoint::at(from);
    for (const auto& [dx, dy] : sightOffsets_) {
        const CellPos c{from.x + dx, from.y + dy, from.level};
        if (!map_.inBounds(c) || map_.terrain(c) == Terrain::Rock) {
            continue;
        }
        if ((dx != 0 || dy != 0) && !map_.cover().lineOfSight(eye, Viewpoint::at(c))) {
            continue;
        }
        sum += cellBeauty(c, true, &counted);
        ++count;
    }
    return count > 0 ? sum / static_cast<float>(count) : 0.0f;
}

float RoomStats::needTarget(float perceivedBeauty) const {
    const float span = need_.beautyAtNeutralMax - need_.beautyAtNeutralMin;
    if (!(span > 0.0f)) {
        return need_.neutralMin;
    }
    const float level = need_.neutralMin + (perceivedBeauty - need_.beautyAtNeutralMin) *
                                               ((need_.neutralMax - need_.neutralMin) / span);
    return std::clamp(level, 0.0f, 1.0f);
}

void RoomStats::computeRoom(const Room& room, RoomStatValues& out) {
    const entt::registry& reg = things_.registry();
    const std::span<const CellPos> cells = rooms_.cellsOf(room);
    std::vector<Entity> counted;
    std::vector<Entity> valued;
    std::vector<int> ring;
    float beautySum = 0.0f;
    float cleanSum = 0.0f;
    float space = 0.0f;
    float wealth = 0.0f;
    for (const CellPos& c : cells) {
        beautySum += cellBeauty(c, true, &counted);
        cleanSum += cellCleanliness(c);
        space += numbers_.spacePerCell;
        const Entity building = map_.buildingAt(c);
        if (building != kNoEntity && reg.valid(building) && !reg.all_of<ConstructionSite>(building)) {
            const DefId def = reg.get<Thing>(building).def;
            if (!standable_[def.value]) {
                space -= numbers_.spaceBlockerReduction;
            }
            if (std::find(valued.begin(), valued.end(), building) == valued.end()) {
                valued.push_back(building);
                wealth += marketValueOf(building);
            }
        }
        wealth += marketValueOfFloor(floors_.at(c));
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const CellPos n{c.x + dx, c.y + dy, c.level};
                if ((dx != 0 || dy != 0) && map_.inBounds(n) && rooms_.roomAt(n) != room.id) {
                    ring.push_back(map_.index(n));
                }
            }
        }
    }
    std::sort(ring.begin(), ring.end());
    ring.erase(std::unique(ring.begin(), ring.end()), ring.end());
    for (const int index : ring) {
        const CellPos c = map_.cellAt(index, room.bounds.level);
        beautySum += cellBeauty(c, false, &counted);
        const Entity building = map_.buildingAt(c);
        if (building != kNoEntity && reg.valid(building) && !reg.all_of<ConstructionSite>(building) &&
            std::find(valued.begin(), valued.end(), building) == valued.end()) {
            valued.push_back(building);
            wealth += marketValueOf(building);
        }
    }
    const float size = static_cast<float>(cells.size());
    const float weighted = size > static_cast<float>(numbers_.beautyFullSizeCells)
                               ? size
                               : numbers_.beautySmallBase + size * numbers_.beautySmallHalf;
    out.beauty = beautySum / std::max(1.0f, weighted);
    out.space = space;
    out.wealth = wealth;
    out.cleanliness = cleanSum / std::max(1.0f, size);

    out.impressiveness = roomImpressiveness(numbers_, wealth, out.beauty, space, out.cleanliness);
}

uint64_t RoomStats::version() const {
    uint64_t v = rooms_.rebuilds();
    for (const uint64_t part : {map_.buildingChanges(), floors_.changes(), filth_.changes(), things_.itemChanges(),
                                plants_.hash(), static_cast<uint64_t>(rooms_.rooms().size())}) {
        v = splitMix64(v ^ part);
    }
    return v;
}

void RoomStats::refresh() {
    rooms_.update();
    const uint64_t now = version();
    if (built_ && now == seenVersion_) {
        return;
    }
    built_ = true;
    seenVersion_ = now;
    const std::vector<Room>& rooms = rooms_.rooms();
    stats_.assign(rooms.size(), RoomStatValues{});
    hasStats_.assign(rooms.size(), 0);
    for (const Room& room : rooms) {
        if (room.enclosed() && !room.outdoors() && room.cellCount > 0) {
            computeRoom(room, stats_[room.id]);
            hasStats_[room.id] = 1;
        }
    }
}

const RoomStatValues* RoomStats::statsOf(RoomId room) const {
    return room < hasStats_.size() && hasStats_[room] != 0 ? &stats_[room] : nullptr;
}

const RoomStatValues* RoomStats::statsAt(CellPos c) {
    const RoomId room = rooms_.roomAt(c);
    return room == kNoRoom ? nullptr : statsOf(room);
}

void RoomStats::step(int64_t now) {
    refresh();
    updateNeeds(now);
}

void RoomStats::updateNeeds(int64_t now) {
    if (!need_.present) {
        return;
    }
    entt::registry& reg = things_.registry();
    std::vector<Entity> pawns;
    for (const auto [entity, needs, pos] : reg.view<const Needs, const Position>().each()) {
        (void)needs;
        (void)pos;
        pawns.push_back(entity);
    }
    std::sort(pawns.begin(), pawns.end());
    const float rise = need_.risePerHour / static_cast<float>(kSecondsPerHour);
    const float fall = need_.fallPerHour / static_cast<float>(kSecondsPerHour);
    for (const Entity pawn : pawns) {
        const Health* health = reg.try_get<Health>(pawn);
        if (health != nullptr && health->dead) {
            continue;
        }
        const uint32_t id = entt::to_integral(pawn);
        const bool known = needs_.count(id) > 0;
        BeautyNeed& need = needs_[id];
        const bool sample = !known || need.sampledAt < 0 ||
                            (now + static_cast<int64_t>(id)) % need_.sampleEverySeconds == 0;
        if (sample && need.sampledAt != now) {
            need.perceived = perceivedBeauty(cellOf(reg.get<Position>(pawn)));
            if (need.sampledAt < 0) {
                need.level = needTarget(need.perceived);  // a new colonist starts at what it sees
            }
            need.sampledAt = now;
        }
        const Worker* worker = reg.try_get<Worker>(pawn);
        if (worker != nullptr && jobIsLyingDown(worker->job)) {
            continue;  // the need only changes while awake
        }
        const float target = needTarget(need.perceived);
        if (target > need.level) {
            need.level = std::min(target, need.level + rise);
        } else {
            need.level = std::max(target, need.level - fall);
        }
    }
    for (auto it = needs_.begin(); it != needs_.end();) {
        it = reg.valid(static_cast<Entity>(it->first)) ? std::next(it) : needs_.erase(it);
    }
}

const BeautyNeed* RoomStats::beautyNeed(Entity pawn) const {
    const auto it = needs_.find(entt::to_integral(pawn));
    return it == needs_.end() ? nullptr : &it->second;
}

bool RoomStats::conditionValue(Entity pawn, const std::string& condition, double& value, RoomRole& role) {
    const entt::registry& reg = things_.registry();
    const Position* pos = reg.valid(pawn) ? reg.try_get<Position>(pawn) : nullptr;
    role = RoomRole::None;
    if (pos == nullptr) {
        return false;
    }
    if (condition == "beautyNeed") {
        const BeautyNeed* need = beautyNeed(pawn);
        if (need == nullptr) {
            return false;
        }
        value = need->level;
        return true;
    }
    const Room* room = rooms_.roomOf(cellOf(*pos));
    if (room == nullptr || !room->enclosed() || room->outdoors()) {
        return false;
    }
    const RoomStatValues* stats = statsOf(room->id);
    if (stats == nullptr) {
        return false;
    }
    role = room->role;
    if (condition == "roomSpace") {
        value = stats->space;
        return true;
    }
    if (condition == "roomImpressiveness") {
        value = std::floor(stats->impressiveness);
        return true;
    }
    if (condition == "roomCleanliness") {
        value = stats->cleanliness;
        return true;
    }
    if (condition == "sleepSharedRoom") {
        value = room->beds;
        return room->role == RoomRole::Barracks && room->beds >= 3;
    }
    return false;
}

void RoomStats::hashState(StateHasher& h) const {
    h.add(static_cast<uint64_t>(needs_.size()));
    for (const auto& [id, need] : needs_) {
        h.add(id);
        h.add(need.level);
        h.add(need.perceived);
        h.add(need.sampledAt);
    }
}

RoomStatsState RoomStats::state() const {
    RoomStatsState out;
    for (const auto& [id, need] : needs_) {
        out.needs.push_back(RoomStatsState::Need{id, need.level, need.perceived, need.sampledAt});
    }
    return out;
}

bool RoomStats::restore(const RoomStatsState& state, std::string& error) {
    needs_.clear();
    built_ = false;
    for (const RoomStatsState::Need& n : state.needs) {
        if (!things_.registry().valid(static_cast<Entity>(n.pawn)) || !(n.level >= 0.0f && n.level <= 1.0f)) {
            error = "the save file has a beauty need that does not fit";
            needs_.clear();
            return false;
        }
        needs_[n.pawn] = BeautyNeed{n.level, n.perceived, n.sampledAt};
    }
    return true;
}

}  // namespace colony
