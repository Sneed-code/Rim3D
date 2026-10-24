#pragma once

#include "ColonySim/Api.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace colony {

// Compact id of one def, handed out at load time. Valid only for the database that made it.
// Saves store defName, never this number.
struct DefId {
    uint32_t value = UINT32_MAX;
    bool valid() const { return value != UINT32_MAX; }
    friend bool operator==(DefId a, DefId b) { return a.value == b.value; }
    friend bool operator!=(DefId a, DefId b) { return a.value != b.value; }
};

// One content def (a JSON object from data/defs/<Kind>/*.json) with `parent` already merged in
// and the tuning multipliers applied. Fields are read by dotted path, e.g. "stats.maxHitPoints".
class COLONY_API Def {
public:
    Def();
    ~Def();
    Def(Def&&) noexcept;
    Def& operator=(Def&&) noexcept;
    Def(const Def&) = delete;
    Def& operator=(const Def&) = delete;

    DefId id() const { return id_; }
    const std::string& kind() const { return kind_; }
    const std::string& defName() const { return defName_; }
    const std::string& label() const { return label_; }  // defName if the def has no label

    bool has(const char* path) const;
    double number(const char* path, double fallback = 0.0) const;
    int64_t integer(const char* path, int64_t fallback = 0) const;
    bool flag(const char* path, bool fallback = false) const;
    std::string text(const char* path, const std::string& fallback = std::string()) const;
    // Number of entries in an array or object field; 0 if absent.
    size_t count(const char* path) const;
    // Key names of an object field, sorted; empty if absent.
    std::vector<std::string> keys(const char* path) const;
    // Text of the i-th array element / number of the i-th array element.
    std::string textAt(const char* path, size_t i, const std::string& fallback = std::string()) const;
    double numberAt(const char* path, size_t i, double fallback = 0.0) const;

private:
    friend class DefDatabase;
    struct Data;
    DefId id_;
    std::string kind_;
    std::string defName_;
    std::string label_;
    std::unique_ptr<Data> data_;
};

// One JSON file's text, tagged with its def kind (the folder it sits in) and a name for errors.
struct DefSource {
    std::string kind;
    std::string name;
    std::string text;
};

// Every def of the game, loaded once at sim start.
//
// Rules (the same as tools/check_defs.py): each file is a JSON array of objects; each has a
// non-empty `defName`, unique within its kind; `parent` names a def of the same kind, no cycles.
// `abstract: true` defs exist only to be inherited and get no id. A child's fields override the
// parent's; objects merge key by key, arrays and scalars replace.
//
// Ids are dense (0..size()-1) and follow load order: kinds sorted by name, then files sorted by
// name, then position in the file. Same files, same ids.
class COLONY_API DefDatabase {
public:
    DefDatabase();
    ~DefDatabase();
    DefDatabase(DefDatabase&&) noexcept;
    DefDatabase& operator=(DefDatabase&&) noexcept;

    // Builds from file texts. `tuningText` may be empty. On failure returns false, fills `errors`
    // with one line per problem, and leaves `out` empty.
    static bool build(const std::vector<DefSource>& sources, const std::string& tuningText,
                      DefDatabase& out, std::vector<std::string>& errors);
    // Reads <dataRoot>/defs/<Kind>/*.json and <dataRoot>/tuning.json (optional) and builds.
    static bool loadDirectory(const std::string& dataRoot, DefDatabase& out,
                              std::vector<std::string>& errors);

    size_t size() const { return defs_.size(); }
    const Def& get(DefId id) const { return defs_[id.value]; }
    // Null when the kind or name is unknown (abstract defs are unknown too).
    const Def* find(const std::string& kind, const std::string& defName) const;
    DefId idOf(const std::string& kind, const std::string& defName) const;
    // All ids of a kind, in id order.
    std::vector<DefId> idsOfKind(const std::string& kind) const;
    std::vector<std::string> kinds() const;

    // Named global multiplier from tuning.json "values" (1.0 when not set).
    double tuning(const std::string& name) const;

private:
    struct Lookup;
    std::vector<Def> defs_;
    std::unique_ptr<Lookup> lookup_;
};

}  // namespace colony
