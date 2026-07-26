#include "ScriptFieldReconcile.h"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace rt2::core {

namespace {

ScriptFieldEntry DefaultEntry(const ScriptFieldDescriptor& descriptor)
{
    assert(descriptor.defaultValue.index() == ScriptFieldArmIndex(descriptor.type)
           && "Script field descriptor default does not match its type");
    return ScriptFieldEntry{ descriptor.type, descriptor.defaultValue };
}

std::string TypeChangeMessage(const UUID& entity,
                              const std::string& field,
                              const std::string& fromField,
                              ScriptFieldType oldType,
                              ScriptFieldType newType)
{
    std::ostringstream ss;
    ss << "entity " << entity.ToString() << " field '" << field << "'";
    if (!fromField.empty() && fromField != field)
        ss << " (aliased from '" << fromField << "')";
    ss << " changed type from " << ScriptFieldTypeName(oldType)
       << " to " << ScriptFieldTypeName(newType)
       << "; using the declared default";
    return ss.str();
}

} // namespace

ScriptFieldMap ReconcileScriptFields(
    const ScriptFieldMap& persisted,
    const std::vector<ScriptFieldDescriptor>& declared,
    const UUID& entity,
    std::vector<FieldDiagnostic>& outDiagnostics)
{
    std::vector<const ScriptFieldDescriptor*> ordered;
    ordered.reserve(declared.size());
    for (const auto& descriptor : declared)
        ordered.push_back(&descriptor);
    std::sort(ordered.begin(), ordered.end(),
              [](const auto* a, const auto* b) { return a->name < b->name; });

    std::unordered_map<std::string, std::vector<std::string>> aliasClaimants;
    std::unordered_set<std::string> declaredNames;
    for (const auto* descriptor : ordered)
    {
        declaredNames.insert(descriptor->name);
        if (descriptor->alias && !descriptor->alias->empty())
            aliasClaimants[*descriptor->alias].push_back(descriptor->name);
    }
    for (auto& [alias, claimants] : aliasClaimants)
        std::sort(claimants.begin(), claimants.end());

    ScriptFieldMap result;
    result.reserve(declared.size());
    std::unordered_set<std::string> handledPersisted;
    std::unordered_set<std::string> emittedAmbiguousAliases;

    auto preserveOrReset = [&](const ScriptFieldDescriptor& descriptor,
                               const std::string& sourceName,
                               const ScriptFieldEntry& stored,
                               bool renamed) {
        handledPersisted.insert(sourceName);

        if (!ScriptFieldEntryHasValidPayload(stored))
        {
            result[descriptor.name] = DefaultEntry(descriptor);
            FieldDiagnostic diagnostic;
            diagnostic.kind = FieldDiagnostic::Kind::InvalidStoredValue;
            diagnostic.entity = entity;
            diagnostic.field = descriptor.name;
            diagnostic.fromField = sourceName;
            diagnostic.message = "entity " + entity.ToString() + " field '" +
                sourceName + "' has a payload inconsistent with its stored " +
                ScriptFieldTypeName(stored.type) + " tag; using the declared default";
            outDiagnostics.push_back(std::move(diagnostic));
            return;
        }

        if (!ScriptFieldTypesCompatible(descriptor.type, stored.type))
        {
            result[descriptor.name] = DefaultEntry(descriptor);
            FieldDiagnostic diagnostic;
            diagnostic.kind = FieldDiagnostic::Kind::TypeChanged;
            diagnostic.entity = entity;
            diagnostic.field = descriptor.name;
            diagnostic.fromField = sourceName;
            diagnostic.message = TypeChangeMessage(entity, descriptor.name,
                                                   sourceName, stored.type,
                                                   descriptor.type);
            outDiagnostics.push_back(std::move(diagnostic));
            return;
        }

        result[descriptor.name] = ScriptFieldEntry{
            descriptor.type,
            stored.value
        };
        if (renamed)
        {
            FieldDiagnostic diagnostic;
            diagnostic.kind = FieldDiagnostic::Kind::Renamed;
            diagnostic.entity = entity;
            diagnostic.field = descriptor.name;
            diagnostic.fromField = sourceName;
            diagnostic.message = "entity " + entity.ToString() + " migrated script field '" +
                sourceName + "' to '" + descriptor.name + "'";
            outDiagnostics.push_back(std::move(diagnostic));
        }
    };

    for (const auto* descriptor : ordered)
    {
        const auto direct = persisted.find(descriptor->name);
        if (direct != persisted.end())
        {
            preserveOrReset(*descriptor, descriptor->name, direct->second, false);
            continue;
        }

        if (descriptor->alias && !descriptor->alias->empty())
        {
            const std::string& oldName = *descriptor->alias;
            const auto claims = aliasClaimants.find(oldName);
            // Ambiguity matters only when there is an authored source value
            // to claim. With no persisted oldName, each declaration is simply
            // a newly-added field and should receive the ordinary Added
            // diagnostic rather than a misleading migration conflict.
            const bool hasAuthoredSource = persisted.find(oldName) != persisted.end();
            const bool ambiguous = hasAuthoredSource
                && claims != aliasClaimants.end()
                && (claims->second.size() > 1 || declaredNames.count(oldName) != 0);
            if (ambiguous)
            {
                result[descriptor->name] = DefaultEntry(*descriptor);
                handledPersisted.insert(oldName);
                if (emittedAmbiguousAliases.insert(oldName).second)
                {
                    FieldDiagnostic diagnostic;
                    diagnostic.kind = FieldDiagnostic::Kind::AmbiguousAlias;
                    diagnostic.entity = entity;
                    diagnostic.field = descriptor->name;
                    diagnostic.fromField = oldName;
                    std::ostringstream ss;
                    ss << "entity " << entity.ToString() << " script field alias '"
                       << oldName << "' is claimed by ";
                    bool needsComma = false;
                    if (declaredNames.count(oldName) != 0)
                    {
                        ss << "'" << oldName << "'";
                        needsComma = true;
                    }
                    for (const auto& claimant : claims->second)
                    {
                        if (needsComma) ss << ", ";
                        ss << "'" << claimant << "'";
                        needsComma = true;
                    }
                    ss << "; using declared defaults";
                    diagnostic.message = ss.str();
                    outDiagnostics.push_back(std::move(diagnostic));
                }
                continue;
            }

            const auto aliased = persisted.find(oldName);
            if (aliased != persisted.end())
            {
                preserveOrReset(*descriptor, oldName, aliased->second, true);
                continue;
            }
        }

        result[descriptor->name] = DefaultEntry(*descriptor);
        FieldDiagnostic diagnostic;
        diagnostic.kind = FieldDiagnostic::Kind::Added;
        diagnostic.entity = entity;
        diagnostic.field = descriptor->name;
        diagnostic.message = "entity " + entity.ToString() + " added script field '" +
            descriptor->name + "' with its declared default";
        outDiagnostics.push_back(std::move(diagnostic));
    }

    std::vector<std::string> removed;
    for (const auto& [name, entry] : persisted)
    {
        (void)entry;
        if (handledPersisted.count(name) == 0)
            removed.push_back(name);
    }
    std::sort(removed.begin(), removed.end());
    for (const auto& name : removed)
    {
        FieldDiagnostic diagnostic;
        diagnostic.kind = FieldDiagnostic::Kind::Removed;
        diagnostic.entity = entity;
        diagnostic.field = name;
        diagnostic.fromField = name;
        diagnostic.message = "entity " + entity.ToString() + " removed script field '" +
            name + "'";
        outDiagnostics.push_back(std::move(diagnostic));
    }

    return result;
}

} // namespace rt2::core
