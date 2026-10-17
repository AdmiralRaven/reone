/*
 * Copyright (c) 2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "reone/game/savedtemplates.h"

#include "reone/game/saveprovenance.h"
#include "reone/resource/director.h"
#include "reone/resource/provider/gffs.h"
#include "reone/system/logutil.h"

using namespace reone::resource;

namespace reone {

namespace game {

namespace {

// Doors and placeables stay where the module puts them, so a saved record is
// the authored object at its place, give or take float noise.
constexpr float kSamePlace = 0.1f;

struct TemplatedList {
    const char *list;
    ResType type;
    /** A field KOTOR's saves always hold for this kind of record. */
    const char *alwaysSaved;
};

const TemplatedList kDoors {"Door List", ResType::Utd, "OnOpen"};
const TemplatedList kPlaceables {"Placeable List", ResType::Utp, "Appearance"};

bool lacksTemplateFields(const Gff &record, const TemplatedList &kind) {
    return !record.has("TemplateResRef") && !record.has(kind.alwaysSaved);
}

glm::vec3 placeOf(const Gff &record) {
    return glm::vec3(record.getFloat("X"), record.getFloat("Y"), record.getFloat("Z"));
}

std::string lowerString(const Gff &record, const std::string &label) {
    return boost::to_lower_copy(record.getString(label));
}

/** An authored object of one list, with its template. */
struct AuthoredObject {
    std::string resRef;
    std::shared_ptr<Gff> blueprint;
    std::string tag;
    glm::vec3 place {0.0f};
};

std::vector<AuthoredObject> authoredObjects(
    const Gff &authoredGit,
    const TemplatedList &kind,
    const TemplateLookup &templates) {
    std::vector<AuthoredObject> objects;
    std::map<std::string, std::shared_ptr<Gff>> blueprints;
    for (const auto &record : authoredGit.getList(kind.list)) {
        AuthoredObject object;
        object.resRef = lowerString(*record, "TemplateResRef");
        if (object.resRef.empty()) {
            continue;
        }
        auto cached = blueprints.find(object.resRef);
        if (cached == blueprints.end()) {
            cached = blueprints.emplace(object.resRef, templates(object.resRef, kind.type)).first;
        }
        object.blueprint = cached->second;
        if (!object.blueprint) {
            continue;
        }
        // An authored door may set its own tag; a placeable takes its template's.
        object.tag = record->has("Tag") ? lowerString(*record, "Tag") : lowerString(*object.blueprint, "Tag");
        // Authored GITs place doors and placeables by X, Y and Z, as saves do.
        object.place = placeOf(*record);
        objects.push_back(std::move(object));
    }
    return objects;
}

std::shared_ptr<Gff> repairedRecord(const Gff &saved, const AuthoredObject &authored) {
    auto record = authored.blueprint->deepCopy();
    record->setType(saved.type());
    replaceSaveField(*record, Gff::Field::newResRef("TemplateResRef", authored.resRef));
    for (const auto &field : saved.fields()) {
        replaceSaveField(*record, field.deepCopy());
    }
    return record;
}

} // namespace

bool hasRecordsWithoutTemplateFields(const Gff &savedGit) {
    for (const auto *kind : {&kDoors, &kPlaceables}) {
        for (const auto &record : savedGit.getList(kind->list)) {
            if (lacksTemplateFields(*record, *kind)) {
                return true;
            }
        }
    }
    return false;
}

SavedTemplateRepair repairSavedTemplateFields(
    const Gff &savedGit,
    const Gff &authoredGit,
    const TemplateLookup &templates) {
    SavedTemplateRepair result;
    for (const auto *kind : {&kDoors, &kPlaceables}) {
        auto saved = savedGit.getList(kind->list);
        if (std::none_of(saved.begin(), saved.end(), [&](const auto &record) { return lacksTemplateFields(*record, *kind); })) {
            continue;
        }
        auto authored = authoredObjects(authoredGit, *kind, templates);
        std::vector<std::shared_ptr<Gff>> records;
        bool changed = false;
        for (const auto &record : saved) {
            if (!lacksTemplateFields(*record, *kind)) {
                records.push_back(record->deepCopy());
                continue;
            }
            std::string tag(lowerString(*record, "Tag"));
            glm::vec3 place(placeOf(*record));
            const AuthoredObject *match = nullptr;
            float nearest = kSamePlace;
            for (const auto &object : authored) {
                float distance = glm::distance(object.place, place);
                if (object.tag == tag && distance <= nearest) {
                    nearest = distance;
                    match = &object;
                }
            }
            if (!match) {
                result.unmatched.push_back(std::string(kind->list) + "/" + tag);
                records.push_back(record->deepCopy());
                continue;
            }
            records.push_back(repairedRecord(*record, *match));
            ++result.repaired;
            changed = true;
        }
        if (changed) {
            if (!result.git) {
                result.git = savedGit.deepCopy();
            }
            replaceSaveField(*result.git, Gff::Field::newList(kind->list, std::move(records)));
        }
    }
    return result;
}

std::shared_ptr<Gff> withSavedTemplateFields(
    IResourceDirector &director,
    IGffs &gffs,
    const std::string &moduleName,
    const Gff &ifo,
    std::shared_ptr<Gff> git) {
    if (!git || !hasRecordsWithoutTemplateFields(*git)) {
        return git;
    }
    // The module as installed, without the save, names each object's template.
    std::shared_ptr<Gff> authored;
    try {
        auto installed = director.prepareModuleLoad(moduleName, nullptr);
        if (installed && installed->moduleIfo() &&
            boost::iequals(installed->moduleIfo()->getString("Mod_Entry_Area"), ifo.getString("Mod_Entry_Area"))) {
            authored = installed->areaGit();
        }
    } catch (const std::exception &e) {
        warn("Saved doors and placeables of '" + moduleName + "' lack their templates' fields, and the installed module could not be read: " + e.what());
        return git;
    }
    if (!authored) {
        warn("Saved doors and placeables of '" + moduleName + "' lack their templates' fields, and the installed module has no GIT for their area");
        return git;
    }
    auto repair = repairSavedTemplateFields(*git, *authored, [&gffs](const std::string &resRef, ResType type) {
        return gffs.get(resRef, type);
    });
    info(str(boost::format("Restored the template fields of %d saved doors and placeables in '%s'") % repair.repaired % moduleName));
    for (const auto &record : repair.unmatched) {
        warn("A saved record without its template's fields matches no installed object: " + record);
    }
    return repair.git ? repair.git : git;
}

} // namespace game

} // namespace reone
