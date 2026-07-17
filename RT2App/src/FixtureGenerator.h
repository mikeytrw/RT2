#pragma once

#ifndef RT2_FIXTURE_GENERATOR_H
#define RT2_FIXTURE_GENERATOR_H

#include "SceneSerializer.h"
#include "SceneDocument.h"
#include "ECSComponents.h"
#include "PrimitiveGeometry.h"
#include "core/UUID.h"

#include <filesystem>

namespace rt2::core {

// Generate the vertical-slice fixture scene and save it to the given path.
// Uses a deterministic UUID provider so the output is byte-stable.
inline bool GenerateSliceFixture(const std::filesystem::path& path, Error& err)
{
    DeterministicUuidProvider provider;
    SceneDocument doc;
    doc.SetUuidProvider(&provider);

    // Material
    SceneMaterial mat;
    mat.baseColor = { 0.8f, 0.2f, 0.2f };
    mat.roughness = 0.5f;
    doc.ecs.materials.push_back(mat);

    // Cube with motion
    MeshData cubeMesh = PrimitiveGeometry::CreateCube(1.0f);
    cubeMesh.name = "cube";
    uint32_t meshIdx = doc.ecs.meshRegistry.AddMesh(std::move(cubeMesh));

    entt::entity cube = doc.ecs.registry.create();
    doc.ecs.registry.emplace<NameComponent>(cube, "Cube");
    Transform& tf = doc.ecs.registry.emplace<Transform>(cube);
    tf.translation = { 0.0f, 0.0f, 0.0f };
    tf.dirty = true;
    doc.ecs.registry.emplace<VisibleComponent>(cube);
    doc.ecs.registry.emplace<MeshRef>(cube, meshIdx, 0);
    doc.ecs.registry.emplace<PrimitiveComponent>(cube, PrimitiveComponent{ PrimitiveComponent::Cube, 1.0f, 24, 16 });
    doc.ecs.registry.emplace<MotionComponent>(cube, MotionComponent{ { 1.0f, 0.0f, 0.0f } });
    doc.AssignNewUuid(cube);

    // Light
    entt::entity light = doc.ecs.registry.create();
    doc.ecs.registry.emplace<NameComponent>(light, "Light");
    Transform& ltf = doc.ecs.registry.emplace<Transform>(light);
    ltf.translation = { 5.0f, 10.0f, 5.0f };
    ltf.dirty = true;
    doc.ecs.registry.emplace<VisibleComponent>(light);
    doc.ecs.registry.emplace<LightComponent>(light, LightComponent{ { 1, 1, 1 }, 50.0f, 50.0f, 30.0f, 45.0f, false });
    doc.AssignNewUuid(light);

    // Camera
    entt::entity cam = doc.ecs.registry.create();
    doc.ecs.registry.emplace<NameComponent>(cam, "Camera");
    Transform& ctf = doc.ecs.registry.emplace<Transform>(cam);
    ctf.translation = { 0.0f, 1.0f, 10.0f };
    ctf.dirty = true;
    doc.ecs.registry.emplace<VisibleComponent>(cam);
    doc.ecs.registry.emplace<CameraComponent>(cam, CameraComponent{ 45.0f, 0.0f, 1.0f, { 0, 0, -1 } });
    doc.AssignNewUuid(cam);

    doc.ecs.camera.position = { 0.0f, 1.0f, 10.0f };
    doc.ecs.camera.forwardDirection = { 0.0f, 0.0f, -1.0f };
    doc.ecs.camera.verticalFOV = 45.0f;

    doc.metadata.name = "vertical-slice";
    doc.metadata.sourcePath = path;

    return SceneSerializer::Save(doc, path, err);
}

} // namespace rt2::core

#endif // RT2_FIXTURE_GENERATOR_H