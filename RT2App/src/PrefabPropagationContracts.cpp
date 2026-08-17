#include "PrefabPropagationContracts.h"

namespace
{
static_assert(sizeof(rt2::core::PrefabPropagationComponentValue) > 0,
              "propagation component payload must remain a concrete CPU type");
static_assert(static_cast<unsigned>(
                  rt2::core::PrefabPropagationInstanceDisposition::Quarantined) == 2,
              "instance disposition ordering is part of deterministic diagnostics");
}
