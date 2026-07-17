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