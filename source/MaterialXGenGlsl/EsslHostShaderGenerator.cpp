//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXGenGlsl/EsslHostShaderGenerator.h>

#include <MaterialXGenHw/HwConstants.h>
#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/GenOptions.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/ShaderStage.h>
#include <MaterialXGenShader/Syntax.h>

MATERIALX_NAMESPACE_BEGIN

// Identifier for this generator itself. Node implementations are still resolved
// through the inherited ESSL target, so the forward-shading standard_surface
// assembly (light loop + environment) is emitted exactly as by the stock ESSL
// generator; only the shader interface differs (see generate()).
const string EsslHostShaderGenerator::TARGET = "genglsl_essl_host";

EsslHostShaderGenerator::EsslHostShaderGenerator(TypeSystemPtr typeSystem) :
    EsslShaderGenerator(typeSystem)
{
    // The ESSL base installs the GLSL ES 3.00 syntax, reserved words and node
    // implementations. This generator reuses all of it; the only behavioural
    // change (for now) is the reduced shader interface applied in generate().
}

ShaderPtr EsslHostShaderGenerator::generate(const string& name, ElementPtr element, GenContext& context) const
{
    // Fold constant material inputs (base_color, roughness, uv0_index, ...) as
    // correctly-typed GLSL literals rather than publishing them as unbound
    // uniforms. This is what the TypeScript post-processing tried to reproduce by
    // parsing the "PublicUniforms" block and reading materialsTex, which mis-typed
    // integer stream indices (uv0_index) and mistook graph-internal inputs
    // (uv_color_cool_in1/in3) for material parameters. Folding removes that whole
    // class of failure at the source.
    context.getOptions().shaderInterfaceType = SHADER_INTERFACE_REDUCED;
    return EsslShaderGenerator::generate(name, element, context);

    // --- Design note: what stays host/TS-side ----------------------------------
    // The environment contract (u_envRadiance / u_envIrradiance /
    // u_envLightIntensity / u_envMatrix) and the main() -> pt_MtlxGeneratedMain
    // rename are performed by the TypeScript adapter, which rewrites only stable
    // symbols (u_env*, "void main()"); those regex rewrites are robust and do not
    // need to move into the generator. Geometric streams DO move here (emitInputs)
    // because they were the fragile part: their names vary per graph and could not
    // be reliably wired host-side.
}

void EsslHostShaderGenerator::emitInputs(GenContext& context, ShaderStage& stage) const
{
    DEFINE_SHADER_STAGE(stage, Stage::VERTEX)
    {
        // The fullscreen raster host has no vertex stage; keep the base emission
        // for API completeness (the vertex stage output is unused by the host).
        EsslShaderGenerator::emitInputs(context, stage);
    }

    DEFINE_SHADER_STAGE(stage, Stage::PIXEL)
    {
        const VariableBlock& vertexData = stage.getInputBlock(HW::VERTEX_DATA);
        if (vertexData.empty())
        {
            return;
        }

        // Emit the geometric streams (normalWorld, tangentWorld, positionWorld,
        // texcoord_0, ...) as mutable globals rather than 'in' varyings: the path
        // tracer raster host feeds them per hit. Reading them at shading time (via
        // the globals below, assigned in pt_MtlxBindGeom) gives the generated graph
        // the live per-hit values instead of undefined varyings.
        emitComment("ESSL host geometric streams (fed each hit by pt_MtlxBindGeom).", stage);
        emitVariableDeclarations(vertexData, EMPTY_STRING, Syntax::SEMICOLON, context, stage, false);
        emitLineBreak(stage);

        // Stable host entry point: the host template calls pt_MtlxBindGeom() with
        // the hit-state geometry before invoking the generated shading entry. Only
        // the streams actually declared by the graph are assigned (unreferenced
        // streams are not emitted, so they are simply skipped here).
        emitLine("void pt_MtlxBindGeom(vec3 ptN, vec3 ptT, vec3 ptB, vec3 ptP, vec2 ptUv)", stage, false);
        emitScopeBegin(stage);
        for (size_t i = 0; i < vertexData.size(); ++i)
        {
            const ShaderPort* v = vertexData[i];
            const string& var = v->getVariable();
            // Order matters: "bitangentWorld" contains "tangentWorld" as a substring.
            string src;
            if (var.find("normalWorld") != string::npos)
            {
                src = "ptN";
            }
            else if (var.find("bitangentWorld") != string::npos)
            {
                src = "ptB";
            }
            else if (var.find("tangentWorld") != string::npos)
            {
                src = "ptT";
            }
            else if (var.find("positionWorld") != string::npos)
            {
                src = "ptP";
            }
            else if (var.find("texcoord") != string::npos)
            {
                src = "ptUv";
            }
            else
            {
                // Unknown geometric stream keeps its default (zero) value.
                continue;
            }
            emitLine(var + " = " + src, stage);
        }
        emitScopeEnd(stage);
        emitLineBreak(stage);
    }
}

void EsslHostShaderGenerator::emitUniforms(GenContext& context, ShaderStage& stage) const
{
    // Emit ONLY the public material parameters, as globals initialized to their
    // authored .mtlx values (folded), wrapped in the __MTLX_PARAMS markers the
    // TypeScript adapter recognizes. Private uniforms (u_envMatrix / u_envRadiance
    // / u_envIrradiance / u_envLightIntensity / u_refractionTwoSided /
    // u_viewPosition / u_numActiveLightSources) and the light-data block are
    // provided by the raster host (shaders/skeleton-essl.glsl); re-emitting them
    // here would collide with the host's #defines and declarations.
    const VariableBlock& publicUniforms = stage.getUniformBlock(HW::PUBLIC_UNIFORMS);
    emitComment("__MTLX_PARAMS_BEGIN__", stage);
    if (!publicUniforms.empty())
    {
        // assignValue = true: emit `float base = 1.0;` etc. so the material renders
        // with its authored values without binding any uniform.
        emitVariableDeclarations(publicUniforms, EMPTY_STRING, Syntax::SEMICOLON, context, stage, true);
    }
    emitComment("__MTLX_PARAMS_END__", stage);
    emitLineBreak(stage);
}

MATERIALX_NAMESPACE_END
