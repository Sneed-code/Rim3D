#include "ColonySim/Defs.h"

#include "ColonySim/Hash.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>
#include <utility>

// The sim builds without exceptions, so the JSON library must not throw.
#ifndef JSON_NOEXCEPTION
#define JSON_NOEXCEPTION
#endif
#include "nlohmann/json.hpp"

namespace colony {

using Json = nlohmann::json;

struct Def::Data {
    Json json;
};

struct DefDatabase::Lookup {
    std::map<std::string, std::unordered_map<std::string, uint32_t>> byName;
    std::map<std::string, std::vector<DefId>> byKind;
    std::unordered_map<std::string, double> values;
};

namespace {

const Json* defsFindPath(const Json& root, const char* path) {
    const Json* cur = &root;
    const char* p = path;
    while (*p != '\0') {
        const char* end = p;
        while (*end != '\0' && *end != '.') ++end;
        if (cur->is_array()) {
            // A number picks an array element ("degreeDatas.0.label").
            size_t index = 0;
            if (p == end) return nullptr;
            for (const char* d = p; d != end; ++d) {
                if (*d < '0' || *d > '9') return nullptr;
                index = index * 10 + static_cast<size_t>(*d - '0');
            }
            if (index >= cur->size()) return nullptr;
            cur = &(*cur)[index];
            p = (*end == '.') ? end + 1 : end;
            continue;
        }
        if (!cur->is_object()) return nullptr;
        const auto it = cur->find(std::string(p, end));
        if (it == cur->end()) return nullptr;
        cur = &*it;
        p = (*end == '.') ? end + 1 : end;
    }
    return cur;
}

void defsMerge(Json& dst, const Json& src) {
    if (dst.is_object() && src.is_object()) {
        for (auto it = src.begin(); it != src.end(); ++it) defsMerge(dst[it.key()], it.value());
    } else {
        dst = src;
    }
}

struct DefsRaw {
    Json json;
    std::string source;
    size_t index = 0;
};

// Multiplies the number at `path` (dotted) inside `def`. Integers stay integers (rounded).
void defsMultiply(Json& def, const std::string& path, double factor) {
    Json* cur = &def;
    size_t p = 0;
    while (true) {
        const size_t dot = path.find('.', p);
        const std::string part = path.substr(p, dot == std::string::npos ? std::string::npos : dot - p);
        if (!cur->is_object()) return;
        const auto it = cur->find(part);
        if (it == cur->end()) return;
        cur = &*it;
        if (dot == std::string::npos) break;
        p = dot + 1;
    }
    if (cur->is_number_integer()) {
        *cur = static_cast<int64_t>(std::llround(cur->get<double>() * factor));
    } else if (cur->is_number()) {
        *cur = cur->get<double>() * factor;
    }
}

}  // namespace

Def::Def() = default;
Def::~Def() = default;
Def::Def(Def&&) noexcept = default;
Def& Def::operator=(Def&&) noexcept = default;

bool Def::has(const char* path) const { return data_ && defsFindPath(data_->json, path) != nullptr; }

double Def::number(const char* path, double fallback) const {
    const Json* j = data_ ? defsFindPath(data_->json, path) : nullptr;
    return (j != nullptr && j->is_number()) ? j->get<double>() : fallback;
}

int64_t Def::integer(const char* path, int64_t fallback) const {
    const Json* j = data_ ? defsFindPath(data_->json, path) : nullptr;
    if (j == nullptr || !j->is_number()) return fallback;
    return j->is_number_integer() ? j->get<int64_t>() : static_cast<int64_t>(std::llround(j->get<double>()));
}

bool Def::flag(const char* path, bool fallback) const {
    const Json* j = data_ ? defsFindPath(data_->json, path) : nullptr;
    return (j != nullptr && j->is_boolean()) ? j->get<bool>() : fallback;
}

std::string Def::text(const char* path, const std::string& fallback) const {
    const Json* j = data_ ? defsFindPath(data_->json, path) : nullptr;
    return (j != nullptr && j->is_string()) ? j->get<std::string>() : fallback;
}

size_t Def::count(const char* path) const {
    const Json* j = data_ ? defsFindPath(data_->json, path) : nullptr;
    return (j != nullptr && (j->is_array() || j->is_object())) ? j->size() : 0;
}

std::vector<std::string> Def::keys(const char* path) const {
    std::vector<std::string> out;
    const Json* j = data_ ? defsFindPath(data_->json, path) : nullptr;
    if (j != nullptr && j->is_object()) {
        for (auto it = j->begin(); it != j->end(); ++it) out.push_back(it.key());
    }
    return out;
}

std::string Def::textAt(const char* path, size_t i, const std::string& fallback) const {
    const Json* j = data_ ? defsFindPath(data_->json, path) : nullptr;
    if (j == nullptr || !j->is_array() || i >= j->size()) return fallback;
    const Json& e = (*j)[i];
    return e.is_string() ? e.get<std::string>() : fallback;
}

double Def::numberAt(const char* path, size_t i, double fallback) const {
    const Json* j = data_ ? defsFindPath(data_->json, path) : nullptr;
    if (j == nullptr || !j->is_array() || i >= j->size()) return fallback;
    const Json& e = (*j)[i];
    return e.is_number() ? e.get<double>() : fallback;
}

namespace {

// Content hash of a database with no defs and no tuning values (see DefDatabase::build).
uint64_t defsEmptyHash() {
    StateHasher h;
    h.add(static_cast<uint64_t>(0));
    return h.value();
}

void defsHashText(StateHasher& h, const std::string& text) { h.add(text.data(), text.size()); }

}  // namespace

DefDatabase::DefDatabase() : lookup_(new Lookup()), contentHash_(defsEmptyHash()) {}
DefDatabase::~DefDatabase() = default;
DefDatabase::DefDatabase(DefDatabase&&) noexcept = default;
DefDatabase& DefDatabase::operator=(DefDatabase&&) noexcept = default;

bool DefDatabase::build(const std::vector<DefSource>& sourcesIn, const std::string& tuningText,
                        DefDatabase& out, std::vector<std::string>& errors) {
    out = DefDatabase();
    const size_t errorsBefore = errors.size();

    // Tuning: {"defs": {Kind: {"dotted.path": multiplier}}, "values": {name: number}}.
    Json tuning = Json::object();
    if (!tuningText.empty()) {
        tuning = Json::parse(tuningText, nullptr, false);
        if (tuning.is_discarded() || !tuning.is_object()) {
            errors.push_back("tuning.json: must be a JSON object");
            tuning = Json::object();
        }
    }
    std::map<std::string, std::vector<std::pair<std::string, double>>> tuneDefs;
    std::unordered_map<std::string, double> tuneValues;
    if (const auto it = tuning.find("defs"); it != tuning.end()) {
        if (!it->is_object()) errors.push_back("tuning.json: \"defs\" must be an object");
        else {
            for (auto k = it->begin(); k != it->end(); ++k) {
                if (!k.value().is_object()) {
                    errors.push_back("tuning.json: defs." + k.key() + " must be an object");
                    continue;
                }
                for (auto f = k.value().begin(); f != k.value().end(); ++f) {
                    if (!f.value().is_number()) {
                        errors.push_back("tuning.json: defs." + k.key() + "." + f.key() + " must be a number");
                        continue;
                    }
                    tuneDefs[k.key()].emplace_back(f.key(), f.value().get<double>());
                }
            }
        }
    }
    if (const auto it = tuning.find("values"); it != tuning.end()) {
        if (!it->is_object()) errors.push_back("tuning.json: \"values\" must be an object");
        else {
            for (auto k = it->begin(); k != it->end(); ++k) {
                if (!k.value().is_number()) {
                    errors.push_back("tuning.json: values." + k.key() + " must be a number");
                    continue;
                }
                tuneValues[k.key()] = k.value().get<double>();
            }
        }
    }

    // Read every file. Sorted so ids don't depend on the order the caller listed files.
    std::vector<const DefSource*> sources;
    for (const DefSource& s : sourcesIn) sources.push_back(&s);
    std::stable_sort(sources.begin(), sources.end(), [](const DefSource* a, const DefSource* b) {
        return a->kind != b->kind ? a->kind < b->kind : a->name < b->name;
    });

    std::map<std::string, std::vector<DefsRaw>> raws;                          // per kind, load order
    std::map<std::string, std::unordered_map<std::string, size_t>> rawByName;  // per kind
    for (const DefSource* s : sources) {
        Json file = Json::parse(s->text, nullptr, false);
        if (file.is_discarded()) {
            errors.push_back(s->name + ": invalid JSON");
            continue;
        }
        if (!file.is_array()) {
            errors.push_back(s->name + ": must be a JSON array of defs");
            continue;
        }
        for (size_t i = 0; i < file.size(); ++i) {
            Json& entry = file[i];
            const auto nameIt = entry.is_object() ? entry.find("defName") : entry.end();
            if (!entry.is_object() || nameIt == entry.end() || !nameIt->is_string() ||
                nameIt->get<std::string>().empty()) {
                errors.push_back(s->name + ": entry " + std::to_string(i) + " has no defName");
                continue;
            }
            const std::string name = nameIt->get<std::string>();
            auto& names = rawByName[s->kind];
            const auto dup = names.find(name);
            if (dup != names.end()) {
                errors.push_back(s->name + ": duplicate " + s->kind + " defName '" + name + "' (also in " +
                                 raws[s->kind][dup->second].source + ")");
                continue;
            }
            names[name] = raws[s->kind].size();
            raws[s->kind].push_back(DefsRaw{std::move(entry), s->name, i});
        }
    }

    // Resolve parents, give ids to the concrete defs, apply tuning.
    for (auto& [kind, list] : raws) {
        const auto& names = rawByName[kind];
        for (const DefsRaw& raw : list) {
            const std::string defName = raw.json["defName"].get<std::string>();

            // Chain from this def up to its root.
            std::vector<const DefsRaw*> chain{&raw};
            bool ok = true;
            while (true) {
                const auto pIt = chain.back()->json.find("parent");
                if (pIt == chain.back()->json.end() || pIt->is_null()) break;
                const std::string parent = pIt->is_string() ? pIt->get<std::string>() : std::string();
                const auto found = names.find(parent);
                if (found == names.end()) {
                    errors.push_back(raw.source + ": " + kind + " '" + defName + "' has unknown parent '" + parent + "'");
                    ok = false;
                    break;
                }
                const DefsRaw* next = &list[found->second];
                if (std::find(chain.begin(), chain.end(), next) != chain.end()) {
                    errors.push_back(raw.source + ": " + kind + " '" + defName + "' has a parent cycle through '" + parent + "'");
                    ok = false;
                    break;
                }
                chain.push_back(next);
            }
            if (!ok) continue;

            const Json* abstractFlag = defsFindPath(raw.json, "abstract");
            if (abstractFlag != nullptr && abstractFlag->is_boolean() && abstractFlag->get<bool>()) continue;

            Json merged = Json::object();
            for (auto c = chain.rbegin(); c != chain.rend(); ++c) {
                Json layer = (*c)->json;
                layer.erase("parent");
                layer.erase("abstract");
                defsMerge(merged, layer);
            }
            if (const auto t = tuneDefs.find(kind); t != tuneDefs.end()) {
                for (const auto& [path, factor] : t->second) defsMultiply(merged, path, factor);
            }

            Def def;
            def.id_.value = static_cast<uint32_t>(out.defs_.size());
            def.kind_ = kind;
            def.defName_ = defName;
            const auto label = merged.find("label");
            def.label_ = (label != merged.end() && label->is_string()) ? label->get<std::string>() : defName;
            def.data_.reset(new Def::Data{std::move(merged)});
            out.lookup_->byName[kind][defName] = def.id_.value;
            out.lookup_->byKind[kind].push_back(def.id_);
            out.defs_.push_back(std::move(def));
        }
    }
    out.lookup_->values = std::move(tuneValues);

    if (errors.size() != errorsBefore) {
        out = DefDatabase();
        return false;
    }
    // Fingerprint: defs in id order, each as kind, defName and its merged JSON (objects dump
    // with sorted keys), then the tuning values sorted by name.
    StateHasher hash;
    hash.add(static_cast<uint64_t>(out.defs_.size()));
    for (const Def& def : out.defs_) {
        defsHashText(hash, def.kind_);
        defsHashText(hash, def.defName_);
        defsHashText(hash, def.data_->json.dump());
    }
    std::vector<std::pair<std::string, double>> sortedValues(out.lookup_->values.begin(), out.lookup_->values.end());
    std::sort(sortedValues.begin(), sortedValues.end());
    for (const auto& [name, value] : sortedValues) {
        defsHashText(hash, name);
        defsHashText(hash, Json(value).dump());
    }
    out.contentHash_ = hash.value();  // equals defsEmptyHash() when there is nothing
    return true;
}

bool DefDatabase::loadDirectory(const std::string& dataRoot, DefDatabase& out, std::vector<std::string>& errors) {
    namespace fs = std::filesystem;
    const auto readFile = [](const fs::path& p, std::string& text) {
        std::ifstream in(p, std::ios::binary);
        if (!in) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        text = ss.str();
        return true;
    };

    std::error_code ec;
    const fs::path defsRoot = fs::path(dataRoot) / "defs";
    std::vector<DefSource> sources;
    fs::recursive_directory_iterator it(defsRoot, ec), end;
    if (ec) {
        errors.push_back(defsRoot.generic_string() + ": cannot read the defs folder");
        out = DefDatabase();
        return false;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || it->path().extension() != ".json") continue;
        const fs::path rel = fs::relative(it->path(), defsRoot, ec);
        if (rel.begin() == rel.end() || std::next(rel.begin()) == rel.end()) continue;  // a file straight in defs/ has no kind
        DefSource s;
        s.kind = rel.begin()->generic_string();
        s.name = "defs/" + rel.generic_string();
        if (!readFile(it->path(), s.text)) {
            errors.push_back(s.name + ": cannot read file");
            continue;
        }
        sources.push_back(std::move(s));
    }
    std::string tuningText;
    const fs::path tuningPath = fs::path(dataRoot) / "tuning.json";
    if (fs::exists(tuningPath, ec) && !readFile(tuningPath, tuningText)) {
        errors.push_back("tuning.json: cannot read file");
    }
    const size_t before = errors.size();
    std::vector<std::string> buildErrors;
    const bool built = build(sources, tuningText, out, buildErrors);
    errors.insert(errors.end(), buildErrors.begin(), buildErrors.end());
    return built && errors.size() == before;
}

const Def* DefDatabase::find(const std::string& kind, const std::string& defName) const {
    const DefId id = idOf(kind, defName);
    return id.valid() ? &defs_[id.value] : nullptr;
}

DefId DefDatabase::idOf(const std::string& kind, const std::string& defName) const {
    const auto k = lookup_->byName.find(kind);
    if (k == lookup_->byName.end()) return DefId{};
    const auto n = k->second.find(defName);
    if (n == k->second.end()) return DefId{};
    DefId id;
    id.value = n->second;
    return id;
}

std::vector<DefId> DefDatabase::idsOfKind(const std::string& kind) const {
    const auto k = lookup_->byKind.find(kind);
    return k == lookup_->byKind.end() ? std::vector<DefId>() : k->second;
}

std::vector<std::string> DefDatabase::kinds() const {
    std::vector<std::string> out;
    for (const auto& [kind, ids] : lookup_->byKind) out.push_back(kind);
    return out;
}

double DefDatabase::tuning(const std::string& name) const {
    const auto it = lookup_->values.find(name);
    return it == lookup_->values.end() ? 1.0 : it->second;
}

}  // namespace colony
