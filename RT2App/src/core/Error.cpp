#include "Error.h"

namespace rt2::core {

const char* Error::CodeName(Code c)
{
    switch (c)
    {
        case None:                 return "none";
        case Io:                   return "io";
        case Parse:                return "parse";
        case SchemaVersion:        return "schema_version";
        case DuplicateUuid:        return "duplicate_uuid";
        case MissingParent:        return "missing_parent";
        case UnknownPrimitive:     return "unknown_primitive";
        case MissingAsset:         return "missing_asset";
        case InvalidEntity:        return "invalid_entity";
        case InvalidHierarchy:     return "invalid_hierarchy";
        case HierarchyCycle:       return "hierarchy_cycle";
        case InvalidTransform:     return "invalid_transform";
        case InvalidArgument:      return "invalid_argument";
        case LockedEntity:         return "locked_entity";
        case ClipboardStale:       return "clipboard_stale";
        case NotPrefabMember:      return "not_prefab_member";
        case InvalidRuntimeState:  return "invalid_runtime_state";
    }
    return "unknown";
}

std::string Error::Format() const
{
    std::string s = "code=";
    s += CodeName(code);
    if (!path.empty())
    {
        s += " path=";
        s += path;
    }
    if (!detail.empty())
    {
        s += " detail=";
        s += detail;
    }
    return s;
}

} // namespace rt2::core
