//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXGenGlsl/PathTracerGlslShaderGenerator.h>

#include <MaterialXGenGlsl/Nodes/PathTracerSurfaceNode.h>

#include <MaterialXGenHw/HwConstants.h>
#include <MaterialXGenShader/Exception.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/ShaderGraph.h>
#include <MaterialXGenShader/ShaderNode.h>
#include <MaterialXGenShader/ShaderStage.h>

MATERIALX_NAMESPACE_BEGIN

namespace
{
// Map an upstream world-space vertex-data variable (normalWorld, tangentWorld,
// ...) to the path tracer global that carries the equivalent value from State.
// Order matters: "bitangentWorld" contains "tangentWorld" as a substring.
string pathTracerStateSourceForVertexVar(const string& var, const string& typeName)
{
    if (var.find("normalWorld") != string::npos)
    {
        return "g_ptN";
    }
    if (var.find("bitangentWorld") != string::npos)
    {
        return "g_ptBitangent";
    }
    if (var.find("tangentWorld") != string::npos)
    {
        return "g_ptTangent";
    }
    if (var.find("positionWorld") != string::npos)
    {
        return "g_ptP";
    }
    if (var.find("texcoord") != string::npos)
    {
        return "g_ptTexcoord";
    }
    return typeName + "(0.0)";
}
} // anonymous namespace

// Identifier for this generator. Node implementations are still resolved via
// getTarget() == GlslShaderGenerator::TARGET ("genglsl"); this string only
// identifies the path tracer generator itself (e.g. for the JS bindings).
const string PathTracerGlslShaderGenerator::TARGET = "genglsl_pathtracer";

PathTracerGlslShaderGenerator::PathTracerGlslShaderGenerator(TypeSystemPtr typeSystem) :
    EsslShaderGenerator(typeSystem)
{
    // The ESSL base already installs the GLSL ES 3.00 syntax and reserved words.
    // The genglsl node implementations are registered by the GlslShaderGenerator
    // base constructor and reused as-is (see getTarget()).
    //
    // Override the surface node so the standard_surface assembly emits a single
    // (V, N, L) closure evaluation (no forward light loop) instead of forward
    // shading; the closure entry points in emitPixelStage drive it via globals.
    registerImplementation("IM_surface_" + GlslShaderGenerator::TARGET, PathTracerSurfaceNode::create);
}

void PathTracerGlslShaderGenerator::emitPixelStage(const ShaderGraph& graph, GenContext& context, ShaderStage& stage) const
{
    // --- No-silent-failure guard (FR-007 / SC-004) ----------------------------
    // Reject closures that have no path-tracer analogue in v1 (volume/VDF) with a
    // named error rather than emitting incorrect GLSL.
    for (const ShaderNode* node : graph.getNodes())
    {
        if (node->hasClassification(ShaderNode::Classification::VDF))
        {
            throwUnsupportedClosure(node->getName(), "closure de volume (VDF) non supportee en v1");
        }
    }

    // --- Stage boilerplate ----------------------------------------------------
    // Reuse the base (ESSL) emission for directives (#version 300 es, precision),
    // type definitions (BSDF/EDF/surfaceshader structs), constants, uniforms and
    // vertex-data inputs so the generated closures share the upstream data model.
    emitDirectives(context, stage);
    emitLineBreak(stage);

    emitTypeDefinitions(context, stage);

    emitConstants(context, stage);
    emitUniforms(context, stage);

    // Common math shared by the genglsl bricks.
    emitLibraryInclude("stdlib/genglsl/lib/mx_math.glsl", context, stage);
    emitLineBreak(stage);

    // Defines and environment/transmission stubs required by the genglsl BSDF
    // bricks. The path tracer integrator owns lighting, so the indirect
    // (environment) closure branches are stubbed to zero rather than sampling an
    // IBL; only the direct reflection/transmission responses are used.
    emitLine("#define DIRECTIONAL_ALBEDO_METHOD 0", stage, false);
    emitLineBreak(stage);
    emitLine("#define AIRY_FRESNEL_ITERATIONS 0", stage, false);
    emitLineBreak(stage);
    emitLibraryInclude("pbrlib/genglsl/lib/mx_environment_none.glsl", context, stage);
    emitLibraryInclude("pbrlib/genglsl/lib/mx_transmission_opacity.glsl", context, stage);
    emitLineBreak(stage);

    // --- Path tracer geometry / sampling globals ------------------------------
    // The upstream geometric nodes expect vertex-data variables (normalWorld,
    // tangentWorld, ...). Instead of declaring them as 'in' (no vertex stage in a
    // path tracer), emit them as mutable globals bound from State inside
    // __mtlxEvalSurface, alongside the closure-direction globals.
    emitComment("Path tracer closure globals (set by the closure entry points).", stage);
    emitLine("vec3 g_ptV", stage);
    emitLine("vec3 g_ptN", stage);
    emitLine("vec3 g_ptL", stage);
    emitLine("vec3 g_ptP", stage);
    emitLine("vec3 g_ptTangent", stage);
    emitLine("vec3 g_ptBitangent", stage);
    emitLine("vec2 g_ptTexcoord", stage);
    emitLine("int g_ptClosureType", stage);
    const VariableBlock& vertexData = stage.getInputBlock(HW::VERTEX_DATA);
    for (size_t i = 0; i < vertexData.size(); ++i)
    {
        const ShaderPort* v = vertexData[i];
        emitLine(_syntax->getTypeName(v->getType()) + " " + v->getVariable(), stage);
    }
    emitLineBreak(stage);

    emitFunctionDefinitions(graph, context, stage);
    emitLineBreak(stage);

    // --- Surface evaluation helper --------------------------------------------
    // Assembles the closure once for the current globals and returns the
    // surfaceshader whose .color holds the BSDF response (+ emission) for the
    // single (V, N, L) direction. Eval/Sample call this after setting the globals.
    const ShaderGraphOutputSocket* outputSocket = graph.getOutputSocket();
    emitLine("surfaceshader mtlxEvalSurface()", stage, false);
    emitFunctionBodyBegin(graph, context, stage);
    // Bind the upstream geometric vertex-data variables from the path tracer State.
    for (size_t i = 0; i < vertexData.size(); ++i)
    {
        const ShaderPort* v = vertexData[i];
        emitLine(v->getVariable() + " = " + pathTracerStateSourceForVertexVar(v->getVariable(), _syntax->getTypeName(v->getType())), stage);
    }
    emitFunctionCalls(graph, context, stage, ShaderNode::Classification::TEXTURE);
    for (ShaderGraphOutputSocket* socket : graph.getOutputSockets())
    {
        if (socket->getConnection())
        {
            const ShaderNode* upstream = socket->getConnection()->getNode();
            if (upstream->getParent() == &graph &&
                (upstream->hasClassification(ShaderNode::Classification::CLOSURE) ||
                 upstream->hasClassification(ShaderNode::Classification::SHADER)))
            {
                emitFunctionCall(*upstream, context, stage);
            }
        }
    }
    if (const ShaderOutput* outputConnection = outputSocket->getConnection())
    {
        emitLine("return " + outputConnection->getVariable(), stage);
    }
    else
    {
        emitLine("return surfaceshader(vec3(0.0), vec3(0.0))", stage);
    }
    emitFunctionBodyEnd(graph, context, stage);
    emitLineBreak(stage);

    // --- Path tracer closure entry points -------------------------------------
    // Contract (shaders/common/mtlx_pure_closure.glsl):
    //   vec3 EvalMtlxPureClosure(int matID, State state, vec3 V, vec3 N, vec3 L, out float pdf, out int flags);
    //   vec3 SampleMtlxPureClosure(int matID, State state, vec3 V, vec3 N, out vec3 L, out float pdf, out int flags);
    emitComment("Path tracer closure entry points (generated by PathTracerGlslShaderGenerator).", stage);
    emitLineBreak(stage);

    emitLine("vec3 EvalMtlxPureClosure(int matID, State state, vec3 V, vec3 N, vec3 L, out float pdf, out int flags)", stage, false);
    emitScopeBegin(stage);
    emitLine("bool isReflect = dot(N, L) >= 0.0", stage);
    emitLine("flags = isReflect ? CLOSURE_FLAG_REFLECT : CLOSURE_FLAG_TRANSMIT", stage);
    emitLine("g_ptV = V", stage);
    emitLine("g_ptN = N", stage);
    emitLine("g_ptL = L", stage);
    emitLine("g_ptP = state.fhp", stage);
    emitLine("g_ptTangent = state.tangent", stage);
    emitLine("g_ptBitangent = state.bitangent", stage);
    emitLine("g_ptTexcoord = state.texCoord", stage);
    emitLine("g_ptClosureType = isReflect ? CLOSURE_TYPE_REFLECTION : CLOSURE_TYPE_TRANSMISSION", stage);
    emitLine("surfaceshader pt_surf = mtlxEvalSurface()", stage);
    emitComment("TODO(T012-T014): per-closure importance-sampling pdf; cosine pdf placeholder.", stage);
    emitLine("pdf = max(dot(N, L), 0.0) * M_PI_INV", stage);
    emitLine("return pt_surf.color", stage);
    emitScopeEnd(stage);
    emitLineBreak(stage);

    emitLine("vec3 SampleMtlxPureClosure(int matID, State state, vec3 V, vec3 N, out vec3 L, out float pdf, out int flags)", stage, false);
    emitScopeBegin(stage);
    emitComment("TODO(T012-T014): per-closure importance sampling; cosine-weighted hemisphere placeholder.", stage);
    emitLine("vec3 pt_t = abs(N.x) > 0.5 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)", stage);
    emitLine("vec3 pt_b = normalize(cross(N, pt_t))", stage);
    emitLine("pt_t = cross(pt_b, N)", stage);
    emitLine("float pt_r1 = rand()", stage);
    emitLine("float pt_r2 = rand()", stage);
    emitLine("float pt_phi = 2.0 * M_PI * pt_r1", stage);
    emitLine("float pt_st = sqrt(pt_r2)", stage);
    emitLine("float pt_ct = sqrt(max(0.0, 1.0 - pt_r2))", stage);
    emitLine("L = normalize(pt_t * (cos(pt_phi) * pt_st) + pt_b * (sin(pt_phi) * pt_st) + N * pt_ct)", stage);
    emitLine("flags = CLOSURE_FLAG_REFLECT", stage);
    emitLine("g_ptV = V", stage);
    emitLine("g_ptN = N", stage);
    emitLine("g_ptL = L", stage);
    emitLine("g_ptP = state.fhp", stage);
    emitLine("g_ptTangent = state.tangent", stage);
    emitLine("g_ptBitangent = state.bitangent", stage);
    emitLine("g_ptTexcoord = state.texCoord", stage);
    emitLine("g_ptClosureType = CLOSURE_TYPE_REFLECTION", stage);
    emitLine("surfaceshader pt_surf = mtlxEvalSurface()", stage);
    emitLine("pdf = max(dot(N, L), 0.0) * M_PI_INV", stage);
    emitLine("return pt_surf.color", stage);
    emitScopeEnd(stage);
    emitLineBreak(stage);
}

void PathTracerGlslShaderGenerator::throwUnsupportedClosure(const string& nodeName, const string& reason) const
{
    throw ExceptionShaderGenError(
        "PathTracerGlslShaderGenerator: unsupported node/closure '" + nodeName + "': " + reason);
}

MATERIALX_NAMESPACE_END
