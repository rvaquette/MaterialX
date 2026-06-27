//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXGenGlsl/Nodes/PathTracerSurfaceNode.h>

#include <MaterialXGenHw/HwShaderGenerator.h>
#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/ShaderNode.h>
#include <MaterialXGenShader/ShaderStage.h>

MATERIALX_NAMESPACE_BEGIN

namespace
{
const string INPUT_BSDF = "bsdf";
const string INPUT_EDF = "edf";
} // anonymous namespace

ShaderNodeImplPtr PathTracerSurfaceNode::create()
{
    return std::make_shared<PathTracerSurfaceNode>();
}

void PathTracerSurfaceNode::createVariables(const ShaderNode&, GenContext&, Shader&) const
{
    // Intentionally empty: the path tracer supplies position/normal/view/light
    // direction via globals (g_pt*) set by the closure entry points, so the
    // forward-shading vertex inputs and light-data uniforms are not declared.
}

void PathTracerSurfaceNode::emitFunctionCall(const ShaderNode& node, GenContext& context, ShaderStage& stage) const
{
    const ShaderGenerator& shadergen = context.getShaderGenerator();

    DEFINE_SHADER_STAGE(stage, Stage::PIXEL)
    {
        // Declare the surfaceshader output (initialized to zero).
        const ShaderOutput* output = node.getOutput();
        shadergen.emitLineBegin(stage);
        shadergen.emitOutput(output, true, true, context, stage);
        shadergen.emitLineEnd(stage);

        shadergen.emitScopeBegin(stage);

        // Geometry and sampling direction come from the path tracer globals.
        shadergen.emitLine("vec3 N = g_ptN", stage);
        shadergen.emitLine("vec3 V = g_ptV", stage);
        shadergen.emitLine("vec3 L = g_ptL", stage);
        shadergen.emitLine("vec3 P = g_ptP", stage);
        shadergen.emitLine("float occlusion = 1.0", stage);
        shadergen.emitLineBreak(stage);

        const string outColor = output->getVariable() + ".color";

        // BSDF response for the single (V, N, L) direction. The closure type
        // (reflection / transmission) is selected by the entry point.
        const ShaderInput* bsdfInput = node.getInput(INPUT_BSDF);
        if (bsdfInput)
        {
            if (const ShaderNode* bsdf = bsdfInput->getConnectedSibling())
            {
                shadergen.emitLine("ClosureData closureData = makeClosureData(g_ptClosureType, L, V, N, P, occlusion)", stage);
                shadergen.emitFunctionCall(*bsdf, context, stage);
                shadergen.emitLine(outColor + " += " + bsdf->getOutput()->getVariable() + ".response", stage);
                shadergen.emitLineBreak(stage);
            }
        }

        // Surface emission (EDF), independent of the sampling direction.
        const ShaderInput* edfInput = node.getInput(INPUT_EDF);
        if (edfInput)
        {
            if (const ShaderNode* edf = edfInput->getConnectedSibling())
            {
                shadergen.emitScopeBegin(stage);
                shadergen.emitLine("ClosureData closureData = makeClosureData(CLOSURE_TYPE_EMISSION, L, V, N, P, occlusion)", stage);
                shadergen.emitFunctionCall(*edf, context, stage);
                shadergen.emitLine(outColor + " += " + edf->getOutput()->getVariable(), stage);
                shadergen.emitScopeEnd(stage);
            }
        }

        shadergen.emitScopeEnd(stage);
        shadergen.emitLineBreak(stage);
    }
}

MATERIALX_NAMESPACE_END
