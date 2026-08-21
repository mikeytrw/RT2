#pragma once

#ifndef RT2_PREFAB_PROPAGATION_COMMAND_H
#define RT2_PREFAB_PROPAGATION_COMMAND_H

#include "EditorCommand.h"
#include "PrefabPropagationContracts.h"

#include <functional>
#include <memory>

// S4 live command boundary.  The command owns the immutable plan and never
// re-plans from the live scene on Execute/Undo/Redo.  All live reads are
// precondition checks against the captured before/after values.
class PrefabPropagationCommand final : public IEditorCommand
{
public:
    using SourceFingerprintReader =
        std::function<rt2::core::PrefabSourceFingerprint()>;

    explicit PrefabPropagationCommand(
        rt2::core::PrefabPropagationPlan plan,
        SourceFingerprintReader sourceReader = {});

    EditorMutationResult Execute(SceneManager& scene) override;
    EditorMutationResult Undo(SceneManager& scene) override;
    std::string Description() const override { return "Propagate Prefab"; }

    const rt2::core::PrefabPropagationPlan& Plan() const noexcept
    { return m_Plan; }

private:
    rt2::core::PrefabPropagationPlan m_Plan;
    SourceFingerprintReader m_SourceReader;
    std::uint64_t m_ExpectedRevision = 0;
    std::uint64_t m_ExpectedResourceGeneration = 0;
    bool m_HasExecuted = false;
    bool m_IsApplied = false;
};

// Local primitive editing deliberately shares the same append-only mesh
// ownership rules as propagation.  It is kept separate from the propagation
// plan because local edits also change the member marker/schema automatically.
class PrefabPrimitiveRecipeCommand final : public IEditorCommand
{
public:
    static rt2::core::Result<std::unique_ptr<PrefabPrimitiveRecipeCommand>>
    Prepare(SceneManager& scene, const rt2::core::UUID& entity,
            const PrimitiveComponent& after);

    EditorMutationResult Execute(SceneManager& scene) override;
    EditorMutationResult Undo(SceneManager& scene) override;
    std::string Description() const override { return "Edit Primitive Recipe"; }

private:
    PrefabPrimitiveRecipeCommand() = default;

    rt2::core::UUID m_Entity;
    PrimitiveComponent m_Before;
    PrimitiveComponent m_After;
    MeshRef m_BeforeRef;
    MeshRef m_AfterRef;
    MeshData m_OwnedMesh;
    bool m_BeforeMarker = false;
    bool m_AfterMarker = true;
    std::uint32_t m_BeforeSchema = 0;
    std::uint32_t m_AfterSchema = 0;
    std::uint64_t m_DocumentGeneration = 0;
    std::uint64_t m_ResourceGeneration = 0;
    std::uint64_t m_AuthoringRevision = 0;
    std::uint64_t m_ExpectedRevision = 0;
    std::uint64_t m_ExpectedResourceGeneration = 0;
    std::uint32_t m_OwnedMeshSlot = 0;
    bool m_HasExecuted = false;
    bool m_IsApplied = false;
};

#endif // RT2_PREFAB_PROPAGATION_COMMAND_H
