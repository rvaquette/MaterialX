//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#ifndef MATERIALX_MTLXPATHTRACERHOSTSURFACENODE_H
#define MATERIALX_MTLXPATHTRACERHOSTSURFACENODE_H

#include <MaterialXGenGlsl/Export.h>

#include <MaterialXGenHw/Nodes/HwSurfaceNode.h>

MATERIALX_NAMESPACE_BEGIN

/// @class MtlxPathTracerHostSurfaceNode
/// Surface node implementation for the MaterialX pathtracer host generator
/// (feature 003). Adapted from the concept in PathTracerSurfaceNode: replaces
/// HwSurfaceNode's forward-shading light loop with a single-direction closure
/// evaluation driven by host globals (g_ptV/g_ptN/g_ptL/g_ptP and
/// g_ptClosureType), writing the resulting response into the surfaceshader
/// output color so evaluateBsdf/sampleBsdf can read one (V, N, L) sample.
class MX_GENGLSL_API MtlxPathTracerHostSurfaceNode : public HwSurfaceNode
{
  public:
    static ShaderNodeImplPtr create();

    /// Host globals supply geometry/direction, so no vertex inputs, view-position
    /// uniform or light-data uniforms are declared.
    void createVariables(const ShaderNode& node, GenContext& context, Shader& shader) const override;

    void emitFunctionCall(const ShaderNode& node, GenContext& context, ShaderStage& stage) const override;
};

MATERIALX_NAMESPACE_END

#endif
