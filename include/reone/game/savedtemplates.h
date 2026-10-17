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

#pragma once

#include "reone/resource/gff.h"
#include "reone/resource/types.h"

namespace reone {

namespace resource {

class IGffs;
class IResourceDirector;

} // namespace resource

} // namespace reone

namespace reone {

namespace game {

/** The template a GIT record names, by ResRef and type: a door's UTD, a placeable's UTP. */
using TemplateLookup = std::function<std::shared_ptr<resource::Gff>(const std::string &resRef, resource::ResType type)>;

/**
 * Whether a saved GIT holds door or placeable records without the fields
 * their templates give them.
 *
 * Until doors and placeables kept their template's name, reone saved them
 * from scratch: a door without its scripts, conversation and name, a
 * placeable without those and its appearance. KOTOR's own saves always hold
 * those fields, even empty, and a record reone saves now names its template.
 * So a door record with neither OnOpen nor TemplateResRef, or a placeable
 * record with neither Appearance nor TemplateResRef, is one of those.
 */
bool hasRecordsWithoutTemplateFields(const resource::Gff &savedGit);

struct SavedTemplateRepair {
    /** A copy of the saved GIT with its records repaired; null when none needed it or none could be. */
    std::shared_ptr<resource::Gff> git;

    /** How many records were repaired. */
    int repaired {0};

    /** The lists and tags of records that needed repair but matched nothing. */
    std::vector<std::string> unmatched;
};

/**
 * Repairs such records from the module's own GIT, \p authoredGit, which
 * names each object's template.
 *
 * A saved door matches the authored door with the same tag at the same place,
 * and a saved placeable the authored placeable at the same place whose
 * template has its tag; neither moves in play. The repaired record is the
 * template with every saved field over it, so it keeps its saved state, and
 * it names the template, as records saved now do.
 */
SavedTemplateRepair repairSavedTemplateFields(
    const resource::Gff &savedGit,
    const resource::Gff &authoredGit,
    const TemplateLookup &templates);

/**
 * The saved GIT of module \p moduleName, with its doors' and placeables'
 * template fields restored where an earlier reone saved them without. When
 * none needs it, or the installed module can't say, it is \p git itself.
 *
 * \param director reads the module as installed, without the save
 * \param gffs reads the templates, which need the module's resources committed
 * \param ifo the saved module's IFO, whose entry area must be the installed one's
 */
std::shared_ptr<resource::Gff> withSavedTemplateFields(
    resource::IResourceDirector &director,
    resource::IGffs &gffs,
    const std::string &moduleName,
    const resource::Gff &ifo,
    std::shared_ptr<resource::Gff> git);

} // namespace game

} // namespace reone
