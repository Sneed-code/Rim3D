#pragma once

// The game's real data with the Human body's zones taken off (HealthRules `zones` `bodies` emptied), for tests of
// RimWorld's health rules (pain shock, capacities, bleed rates) that animals and unzoned bodies still use. People have
// body zones in the game (docs/design/body-health.md).

#include "ColonySim/Defs.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace colony {

inline std::shared_ptr<const DefDatabase> zonelessTestDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        namespace fs = std::filesystem;
        const fs::path root = fs::path(COLONY_DATA_DIR);
        std::vector<fs::path> files;
        for (const auto& entry : fs::recursive_directory_iterator(root / "defs")) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        std::vector<DefSource> sources;
        for (const fs::path& file : files) {
            std::ifstream in(file, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            std::string body = text.str();
            const std::string zoned = "\"bodies\": [\"Human\"]";
            const size_t at = body.find(zoned);
            if (file.filename() == "Rules.json" && at != std::string::npos) {
                body.replace(at, zoned.size(), "\"bodies\": []");
            }
            sources.push_back(DefSource{file.parent_path().filename().string(), file.filename().string(), body});
        }
        std::ifstream tuningFile(root / "tuning.json", std::ios::binary);
        std::stringstream tuning;
        tuning << tuningFile.rdbuf();
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::build(sources, tuning.str(), *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

}  // namespace colony
