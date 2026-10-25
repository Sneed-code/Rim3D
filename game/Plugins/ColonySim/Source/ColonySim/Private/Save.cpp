#include "ColonySim/Save.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Health.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Things.h"
#include "ColonySim/Version.h"
#include "ColonySim/Zones.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

// The sim builds without exceptions, so the JSON library must not throw.
#ifndef JSON_NOEXCEPTION
#define JSON_NOEXCEPTION
#endif
#include "nlohmann/json.hpp"

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.
using SaveJson = nlohmann::ordered_json;

constexpr char kSaveMagic[8] = {'R', 'I', 'M', '3', 'D', 'S', 'A', 'V'};
constexpr size_t kSaveMagicSize = sizeof kSaveMagic;
// Bump when the layout below changes, and keep reading the old versions (the archive's
// version() says which one a file is). Changing it needs a line in docs/design/saves.md.
constexpr uint32_t kSaveFormatVersion = 8;  // 2: S3's path queue, pathBudget, a hauler's path ticket; 3: S4's pawn velocity;
                                            // 4: C8's scenario and colonists; 5: F1's pawn health; 6: C1's jobs,
                                            // work priorities and zones; 7: U9's direct control; 8: C6's needs,
                                            // schedules, bed owners, the eating job's chair
// Saves from before the job system (formats 1 to 5) can't be read: their hauling placeholder has
// no counterpart in the job system (docs/design/saves.md).
constexpr uint32_t kSaveOldestFormat = 6;
constexpr const char* kSaveFormatName = "rim3d-save";
constexpr const char* kSaveThingsKind = "Things";
constexpr const char* kSaveWorkTypesKind = "WorkTypes";

uint64_t saveChecksum(const uint8_t* data, size_t size) {
    StateHasher h;
    h.add(static_cast<uint64_t>(size));
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t word = 0;
        for (size_t k = 0; k < 8; ++k) {
            word |= static_cast<uint64_t>(data[i + k]) << (8 * k);
        }
        h.add(word);
    }
    uint64_t tail = 0;
    for (size_t k = 0; i + k < size; ++k) {
        tail |= static_cast<uint64_t>(data[i + k]) << (8 * k);
    }
    h.add(tail);
    return h.value();
}

// What the save holds, as plain values. The three archives below (binary write, binary read,
// JSON write) all walk it with the same `saveXxx(ar, value)` functions, so the file layout and
// the JSON dump can't drift apart.
struct SaveThing {
    std::string def;  // defName of a Things def
    ThingCategory category = ThingCategory::Item;
};

// A pawn's health as saved: the primary state of `Health` (what is derived is worked out again on
// loading), with the injuries' defs by name.
struct SaveInjury {
    uint16_t part = 0;
    std::string hediff;  // defName of a Hediffs def
    float severity = 0.0f;
    float tendQuality = -1.0f;
};

struct SaveHealth {
    bool protagonist = false;
    RestKind rest = RestKind::Ground;
    std::vector<SaveInjury> injuries;
    std::vector<MissingPart> missing;
    float bloodLoss = 0.0f;
    bool dead = false;
    DeathCause cause = DeathCause::None;
};

// A worker's job and memory, with the work type by defName (empty for none).
struct SaveWorker {
    Job job;
    std::string workType;
    std::vector<AvoidTarget> avoid;
};

struct SaveWorkPriority {
    std::string workType;  // WorkTypes defName
    uint8_t priority = 0;
    bool disabled = false;
};

struct SaveWorkSettings {
    std::vector<SaveWorkPriority> entries;
};

struct SaveEntity {
    uint32_t id = 0;
    std::optional<SaveThing> thing;
    std::optional<Position> position;
    std::optional<Footprint> footprint;
    std::optional<StackCount> stack;
    std::optional<HitPoints> hitPoints;
    std::optional<Mover> mover;
    std::optional<Colonist> colonist;  // format 4 on
    std::optional<SaveHealth> health;  // format 5 on
    std::optional<SaveWorker> worker;
    std::optional<SaveWorkSettings> work;
    std::optional<Carrying> carrying;
    bool playerControlled = false;  // format 7 on
    std::optional<Needs> needs;        // format 8 on
    std::optional<Schedule> schedule;  // format 8 on
    std::optional<Bed> bed;            // format 8 on
};

// A stockpile zone, with its filter by defName.
struct SaveZone {
    uint32_t id = 0;
    StoragePriority priority = StoragePriority::Normal;
    std::vector<std::string> disallowed;  // Things defNames
    std::vector<CellPos> cells;
};

struct SavePending {
    uint64_t id = 0;
    Command command;
};

struct SaveData {
    std::string simVersion;
    uint64_t defsHash = 0;
    SimSettings settings;
    int64_t seconds = 0;
    uint64_t rngState = 0;
    uint64_t rngIncrement = 0;
    uint64_t groundHash = 0;
    std::optional<CellPos> stockpile;
    std::vector<uint32_t> haulers;
    uint64_t hauls = 0;
    uint64_t failedJobs = 0;
    uint64_t repaths = 0;  // format 3 on
    std::vector<std::string> setupErrors;
    uint64_t submitted = 0;
    std::vector<SavePending> pending;
    std::vector<LoggedCommand> commandLog;
    std::vector<Checkpoint> checkpoints;
    // Entity ids no thing uses at the moment, in the order the registry reuses them (the next
    // one first), so a loaded sim hands out the same ids as the one that was saved.
    std::vector<uint32_t> freeIds;
    std::vector<SaveEntity> entities;  // live things, in id order
    PathSavedState paths;              // the path queue (format 2 on)
    uint32_t nextZoneId = 1;           // format 4 on
    std::vector<SaveZone> zones;
};

// ---- Archives ------------------------------------------------------------------------------
//
// Each archive offers: value (numbers, bools, enums), text, note (JSON only), group, list,
// optional, require (checks while reading), kReading, version().

class SaveBinaryWriter {
public:
    static constexpr bool kReading = false;
    uint32_t version() const { return kSaveFormatVersion; }

    template <typename T>
    void value(const char*, T& v) {
        put(v);
    }
    void text(const char*, std::string& s) {
        put(static_cast<uint32_t>(s.size()));
        bytes_.insert(bytes_.end(), s.begin(), s.end());
    }
    void note(const char*, const char*) {}
    template <typename F>
    void group(const char*, F fn) {
        fn();
    }
    template <typename T, typename F>
    void list(const char*, std::vector<T>& items, F fn) {
        put(static_cast<uint32_t>(items.size()));
        for (T& item : items) {
            fn(item);
        }
    }
    template <typename T, typename F>
    void optional(const char*, std::optional<T>& item, F fn) {
        put(item.has_value());
        if (item) {
            fn(*item);
        }
    }
    void require(bool, const char*) {}

    std::vector<uint8_t>& bytes() { return bytes_; }

private:
    template <typename T>
    void put(T v) {
        if constexpr (std::is_enum_v<T>) {
            put(static_cast<std::underlying_type_t<T>>(v));
        } else if constexpr (std::is_same_v<T, bool>) {
            bytes_.push_back(v ? 1 : 0);
        } else if constexpr (std::is_floating_point_v<T>) {
            using Bits = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
            Bits bits = 0;
            std::memcpy(&bits, &v, sizeof bits);
            put(bits);
        } else {
            static_assert(std::is_integral_v<T>, "save numbers, bools and enums");
            const auto bits = static_cast<std::make_unsigned_t<T>>(v);
            for (size_t k = 0; k < sizeof(T); ++k) {
                bytes_.push_back(static_cast<uint8_t>((static_cast<uint64_t>(bits) >> (8 * k)) & 0xFF));
            }
        }
    }

    std::vector<uint8_t> bytes_;
};

class SaveBinaryReader {
public:
    static constexpr bool kReading = true;

    SaveBinaryReader(const uint8_t* data, size_t size, uint32_t version) : data_(data), size_(size), version_(version) {}

    uint32_t version() const { return version_; }
    bool failed() const { return !error_.empty(); }
    const std::string& error() const { return error_; }
    bool atEnd() const { return pos_ == size_; }
    void fail(const char* message) {
        if (error_.empty()) {
            error_ = message;
        }
    }

    template <typename T>
    void value(const char*, T& v) {
        get(v);
    }
    void text(const char*, std::string& s) {
        uint32_t length = 0;
        get(length);
        if (failed() || length > size_ - pos_) {
            fail("the save file is cut short");
            s.clear();
            return;
        }
        s.assign(reinterpret_cast<const char*>(data_ + pos_), length);
        pos_ += length;
    }
    void note(const char*, const char*) {}
    template <typename F>
    void group(const char*, F fn) {
        fn();
    }
    template <typename T, typename F>
    void list(const char*, std::vector<T>& items, F fn) {
        uint32_t count = 0;
        get(count);
        // Every element takes at least a byte, so a bigger count is a damaged file; checking
        // keeps a bad count from asking for gigabytes.
        if (failed() || count > size_ - pos_) {
            fail("the save file is cut short");
            items.clear();
            return;
        }
        items.clear();
        items.resize(count);
        for (T& item : items) {
            fn(item);
            if (failed()) {
                return;
            }
        }
    }
    template <typename T, typename F>
    void optional(const char*, std::optional<T>& item, F fn) {
        bool present = false;
        get(present);
        item.reset();
        if (present && !failed()) {
            item.emplace();
            fn(*item);
        }
    }
    void require(bool ok, const char* message) {
        if (!ok) {
            fail(message);
        }
    }

private:
    template <typename T>
    void get(T& v) {
        if constexpr (std::is_enum_v<T>) {
            std::underlying_type_t<T> raw{};
            get(raw);
            v = static_cast<T>(raw);
        } else if constexpr (std::is_same_v<T, bool>) {
            uint8_t raw = 0;
            get(raw);
            if (raw > 1) {
                fail("the save file is damaged (a yes/no value is neither)");
            }
            v = raw == 1;
        } else if constexpr (std::is_floating_point_v<T>) {
            using Bits = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
            Bits bits = 0;
            get(bits);
            std::memcpy(&v, &bits, sizeof bits);
        } else {
            static_assert(std::is_integral_v<T>, "save numbers, bools and enums");
            if (sizeof(T) > size_ - pos_) {
                fail("the save file is cut short");
                pos_ = size_;
                v = T{};
                return;
            }
            uint64_t bits = 0;
            for (size_t k = 0; k < sizeof(T); ++k) {
                bits |= static_cast<uint64_t>(data_[pos_ + k]) << (8 * k);
            }
            pos_ += sizeof(T);
            v = static_cast<T>(static_cast<std::make_unsigned_t<T>>(bits));
        }
    }

    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    uint32_t version_;
    std::string error_;
};

class SaveJsonWriter {
public:
    static constexpr bool kReading = false;

    explicit SaveJsonWriter(SaveJson& root) : cur_(&root) {}
    uint32_t version() const { return kSaveFormatVersion; }

    template <typename T>
    void value(const char* name, T& v) {
        if constexpr (std::is_enum_v<T>) {
            slot(name) = static_cast<std::underlying_type_t<T>>(v);
        } else {
            slot(name) = v;
        }
    }
    void text(const char* name, std::string& s) { slot(name) = s; }
    void note(const char* name, const char* s) { slot(name) = s; }
    template <typename F>
    void group(const char* name, F fn) {
        SaveJson* saved = cur_;
        cur_ = &slot(name);
        if (name != nullptr) {
            *cur_ = SaveJson::object();
        }
        fn();
        cur_ = saved;
    }
    template <typename T, typename F>
    void list(const char* name, std::vector<T>& items, F fn) {
        SaveJson& array = slot(name);
        array = SaveJson::array();
        SaveJson* saved = cur_;
        for (T& item : items) {
            array.push_back(nullptr);
            cur_ = &array.back();
            fn(item);
        }
        cur_ = saved;
    }
    template <typename T, typename F>
    void optional(const char* name, std::optional<T>& item, F fn) {
        if (item) {
            group(name, [&] { fn(*item); });
        }
    }
    void require(bool, const char*) {}

private:
    SaveJson& slot(const char* name) { return name != nullptr ? (*cur_)[name] : *cur_; }

    SaveJson* cur_;
};

// ---- What the save holds, field by field ------------------------------------------------

template <typename Ar>
void saveCell(Ar& ar, CellPos& c) {
    ar.value("x", c.x);
    ar.value("y", c.y);
    ar.value("level", c.level);
}

template <typename Ar>
void saveRect(Ar& ar, CellRect& r) {
    ar.value("minX", r.minX);
    ar.value("minY", r.minY);
    ar.value("sizeX", r.sizeX);
    ar.value("sizeY", r.sizeY);
    ar.value("level", r.level);
    ar.require(r.sizeX >= 0 && r.sizeY >= 0 && r.sizeX <= 4096 && r.sizeY <= 4096, "the save file has a bad area");
}

template <typename Ar>
void saveCommand(Ar& ar, Command& command) {
    static_assert(std::variant_size_v<Command> == 14, "save every command type here");
    uint8_t type = static_cast<uint8_t>(command.index());
    ar.value("type", type);
    if constexpr (Ar::kReading) {
        switch (type) {
            case 0: command = SpawnThingCommand{}; break;
            case 1: command = DestroyThingCommand{}; break;
            case 2: command = DamagePawnCommand{}; break;
            case 3: command = TendPawnCommand{}; break;
            case 4: command = SetWorkPriorityCommand{}; break;
            case 5: command = CreateStockpileCommand{}; break;
            case 6: command = RemoveZoneCellsCommand{}; break;
            case 7: command = SetStockpilePriorityCommand{}; break;
            case 8: command = SetStockpileAllowedCommand{}; break;
            case 9: command = OrderHaulCommand{}; break;
            case 10: command = ControlPawnCommand{}; break;
            case 11: command = DirectMoveCommand{}; break;
            case 12: command = SetScheduleCommand{}; break;
            case 13: command = AssignBedCommand{}; break;
            default: ar.require(false, "the save file names a command this build doesn't know"); return;
        }
        // A format older than the command that was added in it can't contain it.
        ar.require((type < 10 || ar.version() >= 7) && (type < 12 || ar.version() >= 8),
                   "the save file names a command this build doesn't know");
    }
    ar.note("name", commandName(command));
    if (auto* spawn = std::get_if<SpawnThingCommand>(&command)) {
        ar.text("def", spawn->def);
        ar.group("cell", [&] { saveCell(ar, spawn->cell); });
        ar.value("count", spawn->count);
        ar.value("rot", spawn->rot);
        ar.require(static_cast<int>(spawn->rot) <= static_cast<int>(Rot4::West), "the save file has a bad rotation");
    } else if (auto* destroy = std::get_if<DestroyThingCommand>(&command)) {
        ar.value("thing", destroy->thing);
    } else if (auto* damage = std::get_if<DamagePawnCommand>(&command)) {
        ar.value("pawn", damage->pawn);
        ar.text("damageType", damage->damageType);
        ar.value("amount", damage->amount);
        ar.value("part", damage->part);
    } else if (auto* tend = std::get_if<TendPawnCommand>(&command)) {
        ar.value("pawn", tend->pawn);
        ar.value("quality", tend->quality);
    } else if (auto* work = std::get_if<SetWorkPriorityCommand>(&command)) {
        ar.value("pawn", work->pawn);
        ar.text("workType", work->workType);
        ar.value("priority", work->priority);
    } else if (auto* create = std::get_if<CreateStockpileCommand>(&command)) {
        ar.group("area", [&] { saveRect(ar, create->area); });
        ar.value("priority", create->priority);
        ar.require(create->priority <= StoragePriority::Critical, "the save file has a bad storage priority");
    } else if (auto* remove = std::get_if<RemoveZoneCellsCommand>(&command)) {
        ar.group("area", [&] { saveRect(ar, remove->area); });
    } else if (auto* priority = std::get_if<SetStockpilePriorityCommand>(&command)) {
        ar.value("zone", priority->zone);
        ar.value("priority", priority->priority);
        ar.require(priority->priority <= StoragePriority::Critical, "the save file has a bad storage priority");
    } else if (auto* allowed = std::get_if<SetStockpileAllowedCommand>(&command)) {
        ar.value("zone", allowed->zone);
        ar.text("def", allowed->def);
        ar.value("allowed", allowed->allowed);
    } else if (auto* haul = std::get_if<OrderHaulCommand>(&command)) {
        ar.value("pawn", haul->pawn);
        ar.value("thing", haul->thing);
    } else if (auto* control = std::get_if<ControlPawnCommand>(&command)) {
        ar.value("pawn", control->pawn);
    } else if (auto* move = std::get_if<DirectMoveCommand>(&command)) {
        ar.value("pawn", move->pawn);
        ar.value("x", move->x);
        ar.value("y", move->y);
    } else if (auto* schedule = std::get_if<SetScheduleCommand>(&command)) {
        ar.value("pawn", schedule->pawn);
        ar.value("hour", schedule->hour);
        ar.value("kind", schedule->kind);
        ar.require(schedule->kind <= ScheduleKind::Sleep, "the save file has a bad schedule kind");
    } else if (auto* assign = std::get_if<AssignBedCommand>(&command)) {
        ar.value("bed", assign->bed);
        ar.value("pawn", assign->pawn);
    }
}

template <typename Ar>
void saveSettings(Ar& ar, SimSettings& s) {
    ar.value("seed", s.seed);
    ar.value("mapSize", s.mapSize);
    ar.value("pawns", s.pawns);
    ar.value("items", s.items);
    ar.value("flatMap", s.flatMap);
    ar.text("pawnDef", s.pawnDef);
    ar.text("haulItemDef", s.haulItemDef);
    ar.value("checkpointEvery", s.checkpointEvery);
    if (ar.version() >= 2) {
        ar.value("pathBudget", s.pathBudget);
    }
    if (ar.version() >= 4) {
        ar.text("scenario", s.scenario);
    }
}

template <typename Ar>
void saveColonist(Ar& ar, Colonist& c) {
    ar.text("firstName", c.firstName);
    ar.text("nickName", c.nickName);
    ar.text("lastName", c.lastName);
    ar.value("female", c.female);
    ar.value("age", c.age);
    ar.text("childhood", c.childhood);
    ar.text("adulthood", c.adulthood);
    ar.list("traits", c.traits, [&](TraitRecord& t) {
        ar.text("def", t.def);
        ar.value("degree", t.degree);
    });
    ar.list("skills", c.skills, [&](SkillRecord& s) {
        ar.text("def", s.def);
        ar.value("level", s.level);
        ar.value("passion", s.passion);
        ar.value("xp", s.xp);
        ar.value("disabled", s.disabled);
        ar.require(static_cast<int>(s.passion) <= static_cast<int>(Passion::Major), "the save file has a bad passion");
    });
    ar.list("disabledWorkTags", c.disabledWorkTags, [&](std::string& tag) { ar.text(nullptr, tag); });
    ar.value("protagonist", c.protagonist);
}

template <typename Ar>
void saveEntity(Ar& ar, SaveEntity& e) {
    ar.value("id", e.id);
    ar.optional("thing", e.thing, [&](SaveThing& t) {
        ar.text("def", t.def);
        ar.value("category", t.category);
        ar.require(static_cast<int>(t.category) <= static_cast<int>(ThingCategory::Pawn),
                   "the save file has a bad thing category");
    });
    ar.optional("position", e.position, [&](Position& p) {
        ar.value("x", p.x);
        ar.value("y", p.y);
        ar.value("level", p.level);
    });
    ar.optional("footprint", e.footprint, [&](Footprint& f) {
        ar.value("sizeX", f.sizeX);
        ar.value("sizeY", f.sizeY);
        ar.value("blocksMovement", f.blocksMovement);
        ar.value("isDoor", f.isDoor);
        ar.value("obstacleDm", f.obstacleDm);
        ar.group("origin", [&] { saveCell(ar, f.origin); });
        ar.value("rotation", f.rotation);
        ar.require(f.sizeX >= 1 && f.sizeX <= 256 && f.sizeY >= 1 && f.sizeY <= 256,
                   "the save file has a building of impossible size");
        ar.require(static_cast<int>(f.rotation) <= static_cast<int>(Rot4::West), "the save file has a bad rotation");
    });
    ar.optional("stackCount", e.stack, [&](StackCount& s) { ar.value("count", s.count); });
    ar.optional("hitPoints", e.hitPoints, [&](HitPoints& hp) {
        ar.value("current", hp.current);
        ar.value("max", hp.max);
    });
    ar.optional("mover", e.mover, [&](Mover& m) {
        ar.value("metresPerStep", m.metresPerStep);
        ar.list("path", m.path, [&](CellPos& c) { saveCell(ar, c); });
        uint64_t next = m.next;
        ar.value("next", next);
        m.next = static_cast<size_t>(next);
        ar.require(m.next <= m.path.size(), "the save file has a pawn walking past the end of its path");
        if (ar.version() >= 3) {
            ar.value("vx", m.vx);
            ar.value("vy", m.vy);
            ar.value("fromX", m.fromX);
            ar.value("fromY", m.fromY);
            ar.value("blockedSteps", m.blockedSteps);
            ar.value("ghostSteps", m.ghostSteps);
            ar.value("heldSteps", m.heldSteps);
            ar.value("gaveUp", m.gaveUp);
        } else if (e.position) {
            // Older saves: a pawn mid-walk sets off along the line from where it stands.
            m.fromX = e.position->x;
            m.fromY = e.position->y;
        }
    });
    ar.optional("worker", e.worker, [&](SaveWorker& w) {
        Job& job = w.job;
        ar.value("job", job.type);
        ar.require(job.type <= JobType::LayDown, "the save file has a bad job");
        ar.value("thing", job.thing);
        ar.group("cell", [&] { saveCell(ar, job.cell); });
        ar.value("count", job.count);
        if (ar.version() >= 8) {
            ar.value("chair", job.chair);
        }
        ar.text("workType", w.workType);
        ar.value("forced", job.forced);
        ar.value("step", job.step);
        ar.require(job.step < std::max(1, jobStepCount(job.type)), "the save file has a job past its last step");
        ar.value("walk", job.walk);
        ar.require(job.walk <= 2, "the save file has a bad walk state");
        ar.value("repaths", job.repaths);
        ar.value("startedAt", job.startedAt);
        ar.value("until", job.until);
        ar.value("pathSlot", job.path.slot);
        ar.value("pathSerial", job.path.serial);
        ar.list("avoid", w.avoid, [&](AvoidTarget& a) {
            ar.value("thing", a.target.thing);
            ar.group("cell", [&] { saveCell(ar, a.target.cell); });
            ar.value("until", a.until);
        });
        ar.require(w.avoid.size() <= JobSystem::kMaxAvoid, "the save file has a worker with too long a memory");
    });
    if (ar.version() >= 4) {
        ar.optional("colonist", e.colonist, [&](Colonist& c) { saveColonist(ar, c); });
    }
    if (ar.version() >= 5) {
        ar.optional("health", e.health, [&](SaveHealth& h) {
            ar.value("protagonist", h.protagonist);
            ar.value("rest", h.rest);
            ar.require(static_cast<int>(h.rest) <= static_cast<int>(RestKind::HospitalBed), "the save file has a bad rest kind");
            ar.list("injuries", h.injuries, [&](SaveInjury& i) {
                ar.value("part", i.part);
                ar.text("hediff", i.hediff);
                ar.value("severity", i.severity);
                ar.value("tendQuality", i.tendQuality);
                ar.require(i.severity >= 0.0f && i.severity < 1.0e6f, "the save file has an impossible injury");
                ar.require(i.tendQuality >= -1.0f && i.tendQuality < 1.0e3f, "the save file has an impossible tend quality");
            });
            ar.list("missing", h.missing, [&](MissingPart& m) {
                ar.value("part", m.part);
                ar.value("tendQuality", m.tendQuality);
                ar.require(m.tendQuality >= -1.0f && m.tendQuality < 1.0e3f, "the save file has an impossible tend quality");
            });
            ar.value("bloodLoss", h.bloodLoss);
            ar.require(h.bloodLoss >= 0.0f && h.bloodLoss <= 1000.0f, "the save file has impossible blood loss");
            ar.value("dead", h.dead);
            ar.value("cause", h.cause);
            ar.require(static_cast<int>(h.cause) <= static_cast<int>(DeathCause::Starvation), "the save file has a bad cause of death");
        });
    }
    ar.optional("work", e.work, [&](SaveWorkSettings& w) {
        ar.list("entries", w.entries, [&](SaveWorkPriority& p) {
            ar.text("workType", p.workType);
            ar.value("priority", p.priority);
            ar.value("disabled", p.disabled);
            ar.require(p.priority <= JobSystem::kMaxPriority, "the save file has a bad work priority");
        });
    });
    ar.optional("carrying", e.carrying, [&](Carrying& c) { ar.value("thing", c.thing); });
    if (ar.version() >= 7) {
        ar.value("playerControlled", e.playerControlled);
    }
    if (ar.version() >= 8) {
        ar.optional("needs", e.needs, [&](Needs& n) {
            ar.value("food", n.food);
            ar.value("rest", n.rest);
            ar.value("malnutrition", n.malnutrition);
            ar.value("foodSearchAfter", n.foodSearchAfter);
            ar.require(n.food >= 0.0f && n.food <= 1000.0f && n.rest >= 0.0f && n.rest <= 1000.0f &&
                           n.malnutrition >= 0.0f && n.malnutrition <= 1000.0f,
                       "the save file has an impossible need level");
        });
        ar.optional("schedule", e.schedule, [&](Schedule& s) {
            std::vector<ScheduleKind> hours(s.hours.begin(), s.hours.end());
            ar.list("hours", hours, [&](ScheduleKind& kind) {
                ar.value(nullptr, kind);
                ar.require(kind <= ScheduleKind::Sleep, "the save file has a bad schedule kind");
            });
            ar.require(hours.size() == s.hours.size(), "the save file has a schedule that isn't 24 hours");
            if (hours.size() == s.hours.size()) {
                std::copy(hours.begin(), hours.end(), s.hours.begin());
            }
        });
        ar.optional("bed", e.bed, [&](Bed& b) {
            ar.list("owners", b.owners, [&](Entity& owner) { ar.value(nullptr, owner); });
        });
    }
}

template <typename Ar>
void saveZone(Ar& ar, SaveZone& z) {
    ar.value("id", z.id);
    ar.value("priority", z.priority);
    ar.require(z.priority <= StoragePriority::Critical, "the save file has a bad storage priority");
    ar.list("disallowed", z.disallowed, [&](std::string& def) { ar.text(nullptr, def); });
    ar.list("cells", z.cells, [&](CellPos& c) { saveCell(ar, c); });
}

template <typename Ar>
void savePaths(Ar& ar, PathSavedState& p) {
    ar.value("nextSerial", p.nextSerial);
    ar.value("updates", p.updates);
    ar.list("requests", p.requests, [&](PathSavedRequest& r) {
        ar.value("serial", r.serial);
        ar.value("status", r.status);
        ar.require(static_cast<int>(r.status) <= static_cast<int>(PathStatus::Failed),
                   "the save file has a bad path status");
        ar.group("start", [&] { saveCell(ar, r.start); });
        ar.group("goal", [&] { saveCell(ar, r.goal); });
        ar.value("askedAt", r.askedAt);
        ar.list("path", r.path, [&](CellPos& c) { saveCell(ar, c); });
    });
    ar.list("freeSlots", p.freeSlots, [&](uint32_t& slot) { ar.value(nullptr, slot); });
    ar.list("queue", p.queue, [&](uint32_t& slot) { ar.value(nullptr, slot); });
    ar.value("activeWork", p.activeWork);
}

template <typename Ar>
void saveData(Ar& ar, SaveData& d) {
    ar.text("simVersion", d.simVersion);
    ar.value("defsHash", d.defsHash);
    ar.group("settings", [&] { saveSettings(ar, d.settings); });
    ar.value("seconds", d.seconds);
    ar.value("rngState", d.rngState);
    ar.value("rngIncrement", d.rngIncrement);
    ar.value("groundHash", d.groundHash);
    ar.optional("stockpile", d.stockpile, [&](CellPos& c) { saveCell(ar, c); });
    ar.list("haulers", d.haulers, [&](uint32_t& id) { ar.value(nullptr, id); });
    ar.value("hauls", d.hauls);
    ar.value("failedJobs", d.failedJobs);
    if (ar.version() >= 3) {
        ar.value("repaths", d.repaths);
    }
    ar.list("setupErrors", d.setupErrors, [&](std::string& s) { ar.text(nullptr, s); });
    ar.value("submitted", d.submitted);
    ar.list("pending", d.pending, [&](SavePending& p) {
        ar.value("id", p.id);
        saveCommand(ar, p.command);
    });
    ar.list("commandLog", d.commandLog, [&](LoggedCommand& logged) {
        ar.value("step", logged.step);
        saveCommand(ar, logged.command);
    });
    ar.list("checkpoints", d.checkpoints, [&](Checkpoint& c) {
        ar.value("step", c.step);
        ar.value("hash", c.hash);
    });
    ar.list("freeIds", d.freeIds, [&](uint32_t& id) { ar.value(nullptr, id); });
    ar.list("entities", d.entities, [&](SaveEntity& e) { saveEntity(ar, e); });
    if (ar.version() >= 2) {
        ar.group("paths", [&] { savePaths(ar, d.paths); });
    }
    ar.value("nextZoneId", d.nextZoneId);
    ar.list("zones", d.zones, [&](SaveZone& z) { saveZone(ar, z); });
}

// The file: magic, format version, the data, then a checksum of everything before it.
std::vector<uint8_t> saveEncode(SaveData& data) {
    SaveBinaryWriter writer;
    std::vector<uint8_t>& bytes = writer.bytes();
    bytes.insert(bytes.end(), kSaveMagic, kSaveMagic + kSaveMagicSize);
    uint32_t version = kSaveFormatVersion;
    writer.value(nullptr, version);
    saveData(writer, data);
    uint64_t checksum = saveChecksum(bytes.data(), bytes.size());
    writer.value(nullptr, checksum);
    return std::move(bytes);
}

bool saveDecode(const std::vector<uint8_t>& bytes, SaveData& out, std::string& error) {
    const size_t headerSize = kSaveMagicSize + sizeof(uint32_t);
    if (bytes.size() < headerSize + sizeof(uint64_t) || std::memcmp(bytes.data(), kSaveMagic, kSaveMagicSize) != 0) {
        error = "this is not a Rim3D save file";
        return false;
    }
    uint32_t version = 0;
    for (size_t k = 0; k < 4; ++k) {
        version |= static_cast<uint32_t>(bytes[kSaveMagicSize + k]) << (8 * k);
    }
    if (version == 0 || version > kSaveFormatVersion) {
        error = "this save is format " + std::to_string(version) + ", made by a newer version of the game (this one reads up to " +
                std::to_string(kSaveFormatVersion) + ")";
        return false;
    }
    if (version < kSaveOldestFormat) {
        error = "this save is format " + std::to_string(version) + ", made before the job system; this version reads format " +
                std::to_string(kSaveOldestFormat) + " and later";
        return false;
    }
    const size_t bodyEnd = bytes.size() - sizeof(uint64_t);
    uint64_t stored = 0;
    for (size_t k = 0; k < 8; ++k) {
        stored |= static_cast<uint64_t>(bytes[bodyEnd + k]) << (8 * k);
    }
    if (saveChecksum(bytes.data(), bodyEnd) != stored) {
        error = "the save file is damaged (its checksum doesn't match)";
        return false;
    }
    SaveBinaryReader reader(bytes.data() + headerSize, bodyEnd - headerSize, version);
    out = SaveData();
    saveData(reader, out);
    if (reader.failed()) {
        error = reader.error();
        return false;
    }
    if (!reader.atEnd()) {
        error = "the save file has extra data after the end";
        return false;
    }
    return true;
}

std::string saveJsonText(SaveData& data) {
    SaveJson root = SaveJson::object();
    root["format"] = kSaveFormatName;
    root["formatVersion"] = kSaveFormatVersion;
    SaveJsonWriter writer(root);
    saveData(writer, data);
    return root.dump(1) + "\n";
}

}  // namespace

// Reads and restores the sim's private state. A friend of Sim.
class SimSaveAccess {
public:
    static SaveData collect(const Sim& sim);
    static bool restore(Sim& sim, SaveData& data, std::string& error);
    // A sim with the saved settings and a regenerated map, ready for restore.
    static std::unique_ptr<Sim> makeEmpty(const SaveData& data, std::shared_ptr<const DefDatabase> defs,
                                          std::string& error);
};

std::unique_ptr<Sim> SimSaveAccess::makeEmpty(const SaveData& data, std::shared_ptr<const DefDatabase> defs,
                                              std::string& error) {
    auto sim = std::unique_ptr<Sim>(new Sim(data.settings, std::move(defs), Sim::EmptyStart{}));
    if (sim->groundHash_ != data.groundHash) {
        error = "the map generator gives a different map for this seed than when the save was made";
        return nullptr;
    }
    return sim;
}

SaveData SimSaveAccess::collect(const Sim& sim) {
    SaveData d;
    d.simVersion = kSimVersion;
    d.defsHash = sim.defs_->contentHash();
    d.settings = sim.settings_;
    d.seconds = sim.time_.seconds;
    d.rngState = sim.rng_.state();
    d.rngIncrement = sim.rng_.increment();
    d.groundHash = sim.groundHash_;
    d.stockpile = sim.stockpileCell_;
    for (const Entity pawn : sim.haulers_) {
        d.haulers.push_back(entt::to_integral(pawn));
    }
    d.hauls = sim.stats_.hauls;
    d.failedJobs = sim.stats_.failedJobs;
    d.repaths = sim.stats_.repaths;
    d.setupErrors = sim.setupErrors_;
    d.submitted = sim.submitted_;
    for (const auto& [id, command] : sim.pending_) {
        d.pending.push_back(SavePending{id, command});
    }
    d.commandLog = sim.commandLog_;
    d.checkpoints = sim.checkpoints_;

    const entt::registry& reg = sim.things_->registry();
    const auto* entities = reg.storage<Entity>();
    const size_t live = entities->free_list();
    for (size_t i = live; i < entities->size(); ++i) {
        d.freeIds.push_back(entt::to_integral(entities->data()[i]));
    }
    std::vector<Entity> ids(entities->data(), entities->data() + live);
    std::sort(ids.begin(), ids.end());
    d.entities.reserve(ids.size());
    for (const Entity e : ids) {
        SaveEntity out;
        out.id = entt::to_integral(e);
        if (const Thing* thing = reg.try_get<Thing>(e)) {
            out.thing = SaveThing{sim.defs_->get(thing->def).defName(), thing->category};
        }
        if (const Position* position = reg.try_get<Position>(e)) {
            out.position = *position;
        }
        if (const Footprint* footprint = reg.try_get<Footprint>(e)) {
            out.footprint = *footprint;
        }
        if (const StackCount* stack = reg.try_get<StackCount>(e)) {
            out.stack = *stack;
        }
        if (const HitPoints* hitPoints = reg.try_get<HitPoints>(e)) {
            out.hitPoints = *hitPoints;
        }
        if (const Mover* mover = reg.try_get<Mover>(e)) {
            out.mover = *mover;
        }
        if (const Worker* worker = reg.try_get<Worker>(e)) {
            SaveWorker w;
            w.job = worker->job;
            w.workType = worker->job.workType.valid() ? sim.defs_->get(worker->job.workType).defName() : std::string();
            w.avoid = worker->avoid;
            out.worker = std::move(w);
        }
        if (const WorkSettings* work = reg.try_get<WorkSettings>(e)) {
            SaveWorkSettings w;
            for (const WorkPriority& entry : work->entries) {
                w.entries.push_back(SaveWorkPriority{sim.defs_->get(entry.workType).defName(), entry.priority, entry.disabled});
            }
            out.work = std::move(w);
        }
        if (const Carrying* carrying = reg.try_get<Carrying>(e)) {
            out.carrying = *carrying;
        }
        if (const Needs* needs = reg.try_get<Needs>(e)) {
            out.needs = *needs;
        }
        if (const Schedule* schedule = reg.try_get<Schedule>(e)) {
            out.schedule = *schedule;
        }
        if (const Bed* bed = reg.try_get<Bed>(e)) {
            out.bed = *bed;
        }
        if (const Colonist* colonist = reg.try_get<Colonist>(e)) {
            out.colonist = *colonist;
        }
        out.playerControlled = reg.all_of<PlayerControl>(e);
        if (const Health* health = reg.try_get<Health>(e)) {
            SaveHealth saved;
            saved.protagonist = health->protagonist;
            saved.rest = health->rest;
            for (const Injury& injury : health->injuries) {
                saved.injuries.push_back(SaveInjury{injury.part, sim.defs_->get(injury.hediff).defName(), injury.severity,
                                                    injury.tendQuality});
            }
            saved.missing = health->missing;
            saved.bloodLoss = health->bloodLoss;
            saved.dead = health->dead;
            saved.cause = health->cause;
            out.health = std::move(saved);
        }
        d.entities.push_back(std::move(out));
    }
    d.paths = sim.pathfinder_.saveState();
    d.nextZoneId = sim.zones_->nextId();
    for (const StockpileZone& zone : sim.zones_->stockpiles()) {
        SaveZone z;
        z.id = zone.id;
        z.priority = zone.priority;
        for (const DefId def : zone.disallowed) {
            z.disallowed.push_back(sim.defs_->get(def).defName());
        }
        z.cells = zone.cells;
        d.zones.push_back(std::move(z));
    }
    return d;
}

bool SimSaveAccess::restore(Sim& sim, SaveData& d, std::string& error) {
    using Traits = entt::entt_traits<Entity>;
    Things& things = *sim.things_;
    entt::registry& reg = things.registry();

    // The ids: every slot from 0 up is either a live thing or a free id.
    const size_t slots = d.entities.size() + d.freeIds.size();
    std::vector<uint32_t> idOfSlot(slots, 0);
    std::vector<uint8_t> slotUsed(slots, 0);
    std::vector<uint8_t> slotFree(slots, 0);
    const auto claim = [&](uint32_t id, bool isFree) {
        const size_t slot = static_cast<size_t>(Traits::to_entity(static_cast<Entity>(id)));
        if (slot >= slots || slotUsed[slot] != 0) {
            return false;
        }
        slotUsed[slot] = 1;
        slotFree[slot] = isFree ? 1 : 0;
        idOfSlot[slot] = id;
        return true;
    };
    for (const SaveEntity& e : d.entities) {
        if (!claim(e.id, false)) {
            error = "the save file's thing ids don't add up";
            return false;
        }
    }
    for (const uint32_t id : d.freeIds) {
        if (!claim(id, true)) {
            error = "the save file's thing ids don't add up";
            return false;
        }
    }
    // Free ids are made one version back and destroyed again, which brings them to the saved
    // version, and destroying them last-first puts them back in the saved reuse order.
    for (size_t slot = 0; slot < slots; ++slot) {
        Entity hint = static_cast<Entity>(idOfSlot[slot]);
        if (slotFree[slot] != 0) {
            hint = Traits::construct(Traits::to_entity(hint), static_cast<Traits::version_type>(Traits::to_version(hint) - 1));
        }
        if (reg.create(hint) != hint) {
            error = "could not restore the thing ids";
            return false;
        }
    }
    for (size_t i = d.freeIds.size(); i-- > 0;) {
        const Entity saved = static_cast<Entity>(d.freeIds[i]);
        reg.destroy(Traits::construct(Traits::to_entity(saved), static_cast<Traits::version_type>(Traits::to_version(saved) - 1)));
    }
    const auto* entities = std::as_const(reg).storage<Entity>();
    bool idsRestored = entities->free_list() == d.entities.size() && entities->size() == slots;
    for (size_t i = 0; idsRestored && i < d.freeIds.size(); ++i) {
        idsRestored = entt::to_integral(entities->data()[d.entities.size() + i]) == d.freeIds[i];
    }
    if (!idsRestored) {
        error = "could not restore the thing ids";
        return false;
    }

    // Components. A thing's def is looked up by name; the rest is copied back.
    for (SaveEntity& e : d.entities) {
        const Entity entity = static_cast<Entity>(e.id);
        if (e.thing) {
            const Def* def = sim.defs_->find(kSaveThingsKind, e.thing->def);
            if (def == nullptr) {
                error = "the save has a thing made from '" + e.thing->def + "', which the data files no longer have";
                return false;
            }
            reg.emplace<Thing>(entity, Thing{def->id(), e.thing->category});
        }
        if (e.position) {
            if (!sim.map_.inBounds(cellOf(*e.position))) {
                error = "the save has a thing outside the map";
                return false;
            }
            reg.emplace<Position>(entity, *e.position);
        }
        if (e.footprint) {
            reg.emplace<Footprint>(entity, *e.footprint);
        }
        if (e.stack) {
            reg.emplace<StackCount>(entity, *e.stack);
        }
        if (e.hitPoints) {
            reg.emplace<HitPoints>(entity, *e.hitPoints);
        }
        if (e.mover) {
            reg.emplace<Mover>(entity, std::move(*e.mover));
        }
        if (e.worker) {
            Worker worker;
            worker.job = e.worker->job;
            worker.job.workType = DefId{};
            if (!e.worker->workType.empty()) {
                // A work type the data files dropped leaves the job without one; it runs on.
                worker.job.workType = sim.defs_->idOf(kSaveWorkTypesKind, e.worker->workType);
            }
            worker.avoid = std::move(e.worker->avoid);
            reg.emplace<Worker>(entity, std::move(worker));
        }
        if (e.work) {
            // Matched to today's work types by name: one the save doesn't know gets the default
            // priority, one the data files dropped is forgotten.
            WorkSettings work = sim.jobs_->defaultWorkSettings();
            for (WorkPriority& entry : work.entries) {
                for (const SaveWorkPriority& saved : e.work->entries) {
                    if (saved.workType == sim.defs_->get(entry.workType).defName()) {
                        entry.priority = saved.priority;
                        entry.disabled = saved.disabled;
                    }
                }
            }
            reg.emplace<WorkSettings>(entity, std::move(work));
        }
        if (e.carrying) {
            reg.emplace<Carrying>(entity, *e.carrying);
        }
        if (e.needs) {
            reg.emplace<Needs>(entity, *e.needs);
        }
        if (e.schedule) {
            reg.emplace<Schedule>(entity, *e.schedule);
        }
        if (e.bed) {
            reg.emplace<Bed>(entity, *e.bed);
        }
        if (e.colonist) {
            reg.emplace<Colonist>(entity, std::move(*e.colonist));
        }
        if (e.health) {
            Health health;
            health.protagonist = e.health->protagonist;
            health.rest = e.health->rest;
            for (const SaveInjury& injury : e.health->injuries) {
                const Def* hediff = sim.defs_->find("Hediffs", injury.hediff);
                if (hediff == nullptr) {
                    error = "the save has an injury of '" + injury.hediff + "', which the data files no longer have";
                    return false;
                }
                health.injuries.push_back(Injury{injury.part, hediff->id(), injury.severity, injury.tendQuality});
            }
            health.missing = e.health->missing;
            health.bloodLoss = e.health->bloodLoss;
            health.dead = e.health->dead;
            health.cause = e.health->cause;
            const int parts = e.thing ? sim.health_->partCount(reg.get<Thing>(entity).def) : 0;
            for (const Injury& injury : health.injuries) {
                if (injury.part >= parts) {
                    error = "the save has an injury on a body part its pawn doesn't have";
                    return false;
                }
            }
            for (const MissingPart& missing : health.missing) {
                if (missing.part >= parts) {
                    error = "the save has a lost body part its pawn doesn't have";
                    return false;
                }
            }
            reg.emplace<Health>(entity, std::move(health));
        } else if (e.thing && e.thing->category == ThingCategory::Pawn &&
                   sim.defs_->get(reg.get<Thing>(entity).def).has("race.body")) {
            reg.emplace<Health>(entity);  // saved before pawns had health
        }
        if (e.playerControlled) {
            reg.emplace<PlayerControl>(entity);
        }
    }
    sim.health_->refreshAll();
    for (const SaveEntity& e : d.entities) {
        if (e.position && !things.relink(static_cast<Entity>(e.id))) {
            error = "the save has a building that no longer fits on the map";
            return false;
        }
    }

    // Everything a thing names must exist.
    const auto exists = [&](Entity e) { return e == kNoEntity || reg.valid(e); };
    for (const SaveEntity& e : d.entities) {
        if ((e.worker && (!exists(e.worker->job.thing) || !exists(e.worker->job.chair))) ||
            (e.carrying && !exists(e.carrying->thing))) {
            error = "the save has a thing that names another thing that doesn't exist";
            return false;
        }
        if (e.bed) {
            for (const Entity owner : e.bed->owners) {
                if (!reg.valid(owner)) {
                    error = "the save has a bed owned by a pawn that doesn't exist";
                    return false;
                }
            }
        }
    }
    // At most one controlled pawn, and it must be one a command could have taken over.
    int controlled = 0;
    for (const SaveEntity& e : d.entities) {
        if (e.playerControlled) {
            ++controlled;
            if (!e.position || !e.mover || !e.colonist || !e.worker) {
                error = "the save has a controlled pawn that isn't a colonist on the map";
                return false;
            }
        }
    }
    if (controlled > 1) {
        error = "the save has more than one controlled pawn";
        return false;
    }
    sim.haulers_.clear();
    for (const uint32_t id : d.haulers) {
        const Entity pawn = static_cast<Entity>(id);
        if (!reg.valid(pawn) || !reg.all_of<Position, Mover, Worker>(pawn)) {
            error = "the save lists a hauler that isn't a pawn who works";
            return false;
        }
        sim.haulers_.push_back(pawn);
    }
    // Zones, then the path queue once every building is back, then the jobs that wait on it.
    std::vector<StockpileZone> zones;
    for (SaveZone& z : d.zones) {
        StockpileZone zone;
        zone.id = z.id;
        zone.priority = z.priority;
        for (const std::string& name : z.disallowed) {
            const DefId def = sim.defs_->idOf(kSaveThingsKind, name);
            if (def.valid()) {  // a def the data files dropped can't be stored anyway
                zone.disallowed.push_back(def);
            }
        }
        zone.cells = std::move(z.cells);
        zones.push_back(std::move(zone));
    }
    if (!sim.zones_->restore(std::move(zones), d.nextZoneId, error)) {
        return false;
    }
    if (!sim.pathfinder_.loadState(std::move(d.paths), error)) {
        return false;
    }
    if (!sim.jobs_->afterLoad(error)) {
        return false;
    }
    if (d.stockpile && !sim.map_.inBounds(*d.stockpile)) {
        error = "the save has a stockpile outside the map";
        return false;
    }
    for (const SavePending& p : d.pending) {
        if (p.id == 0 || p.id > d.submitted) {
            error = "the save has a queued command with an impossible number";
            return false;
        }
    }

    sim.time_.seconds = d.seconds;
    sim.rng_.restore(d.rngState, d.rngIncrement);
    sim.stockpileCell_ = d.stockpile;
    sim.stats_.hauls = d.hauls;
    sim.stats_.failedJobs = d.failedJobs;
    sim.stats_.repaths = d.repaths;
    sim.setupErrors_ = d.setupErrors;
    sim.submitted_ = d.submitted;
    sim.pending_.clear();
    for (SavePending& p : d.pending) {
        sim.pending_.emplace_back(p.id, std::move(p.command));
    }
    sim.commandLog_ = std::move(d.commandLog);
    sim.checkpoints_ = std::move(d.checkpoints);
    sim.events_.clear();
    return true;
}

std::vector<uint8_t> saveSim(const Sim& sim) {
    SaveData data = SimSaveAccess::collect(sim);
    return saveEncode(data);
}

SaveLoadResult loadSim(const std::vector<uint8_t>& bytes, std::shared_ptr<const DefDatabase> defs) {
    SaveLoadResult result;
    SaveData data;
    if (!saveDecode(bytes, data, result.error)) {
        return result;
    }
    const SimSettings& s = data.settings;
    if (s.mapSize <= 0 || s.mapSize > 4096 || s.pawns < 0 || s.items < 0 || s.checkpointEvery < 0 || s.pathBudget <= 0 ||
        data.seconds < 0) {
        result.error = "the save file has impossible settings";
        return result;
    }
    const uint64_t defsHash = defs ? defs->contentHash() : DefDatabase().contentHash();
    if (data.defsHash != defsHash) {
        result.warnings.push_back("the data files differ from the ones this save was made with");
    }
    if (data.simVersion != kSimVersion) {
        result.warnings.push_back("this save was made by sim " + data.simVersion + ", this is " + kSimVersion);
    }

    std::unique_ptr<Sim> sim = SimSaveAccess::makeEmpty(data, std::move(defs), result.error);
    if (!sim || !SimSaveAccess::restore(*sim, data, result.error)) {
        return result;
    }
    result.sim = std::move(sim);
    return result;
}

std::string saveToJson(const Sim& sim) {
    SaveData data = SimSaveAccess::collect(sim);
    return saveJsonText(data);
}

bool saveFileToJson(const std::vector<uint8_t>& bytes, std::string& json, std::string& error) {
    SaveData data;
    if (!saveDecode(bytes, data, error)) {
        return false;
    }
    json = saveJsonText(data);
    return true;
}

bool readSaveFile(const std::string& path, std::vector<uint8_t>& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = path + ": cannot read file";
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool writeSaveFile(const std::string& path, const std::vector<uint8_t>& bytes, std::string& error) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        error = path + ": cannot write file";
        return false;
    }
    return true;
}

}  // namespace colony
