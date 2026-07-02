//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#ifndef MATERIALX_PATHTRACERSURFACENODE_H
#define MATERIALX_PATHTRACERSURFACENODE_H

#include <MaterialXGenGlsl/Export.h>

#include <MaterialXGenHw/Nodes/HwSurfaceNode.h>

MATERIALX_NAMESPACE_BEGIN

/// @class PathTracerSurfaceNode
/// Surface node implementation for the path tracer GLSL generator.
///
/// Replaces the forward-shading emission of HwSurfaceNode (vertex-data normals,
/// view position uniform and the active-light loop) with a single-direction
/// closure evaluation driven by path-tracer globals (g_ptV/g_ptN/g_ptL/g_ptP and
/// g_ptClosureType). The node assembles the upstream BSDF/EDF bricks once and
/// writes the resulting response into the surfaceshader output's color, so the
/// generator's EvalMtlxClosure / SampleMtlxClosure entry points can read
/// the BSDF value for one (V, N, L) direction.
class MX_GENGLSL_API PathTracerSurfaceNode : public HwSurfaceNode
{
  public:
    static ShaderNodeImplPtr create();

    /// The path tracer provides geometry through globals/State, so no vertex
    /// inputs, view-position uniform or light-data uniforms are needed.
    void createVariables(const ShaderNode& node, GenContext& context, Shader& shader) const override;

    void emitFunctionCall(const ShaderNode& node, GenContext& context, ShaderStage& stage) const override;
};

MATERIALX_NAMESPACE_END

#endif
