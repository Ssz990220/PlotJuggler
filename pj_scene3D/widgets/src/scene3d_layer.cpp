// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/scene3d_layer.h"

namespace pj::scene3d {

// Out-of-line ctor/dtor anchor the vtable in a single translation unit
// (key-function rule). Pure-virtual interface beyond that — every
// behavior lives in concrete subclasses.
Scene3DLayer::Scene3DLayer(QObject* parent) : PJ::ISceneLayer(parent) {}

Scene3DLayer::~Scene3DLayer() = default;

}  // namespace pj::scene3d
