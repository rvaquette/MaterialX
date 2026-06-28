//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXGenGlsl/PathTracerGlslShaderGenerator.h>

#include <MaterialXGenGlsl/Nodes/PathTracerSurfaceNode.h>

#include <MaterialXGenHw/HwConstants.h>
#include <MaterialXGenShader/Exception.h>
#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/GenOptions.h>
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

// Map a published standard_surface interface input to the equivalent path tracer
// State.mat field (read from materialsTex), so the closure uses the per-material
// values from the material texture rather than unbound uniforms. Returns "" when
// there is no mapping (the input keeps its authored default value).
string pathTracerMatBindingForInput(const string& name)
{
    if (name == "base") return "state.mat.baseWeight";
    if (name == "base_color") return "(state.mat.baseColor / max(state.mat.baseWeight, 1e-4))";
    if (name == "diffuse_roughness") return "state.mat.baseDiffuseRoughness";
    if (name == "metalness") return "state.mat.metallic";
    if (name == "specular") return "state.mat.specularWeight";
    if (name == "specular_color") return "state.mat.specularColor";
    if (name == "specular_roughness") return "state.mat.roughness";
    if (name == "specular_IOR") return "state.mat.ior";
    if (name == "specular_anisotropy") return "state.mat.anisotropic";
    if (name == "specular_rotation") return "state.mat.anisotropyRotation";
    if (name == "transmission") return "state.mat.specTrans";
    if (name == "transmission_color") return "state.mat.transmissionColor";
    if (name == "transmission_extra_roughness") return "state.mat.transmissionExtraRoughness";
    if (name == "transmission_depth") return "state.mat.transmissionDepth";
    if (name == "transmission_scatter") return "state.mat.transmissionScatter";
    if (name == "transmission_scatter_anisotropy") return "state.mat.transmissionScatterAnisotropy";
    if (name == "transmission_dispersion") return "state.mat.transmissionDispersion";
    if (name == "subsurface") return "state.mat.subsurface";
    if (name == "subsurface_color") return "state.mat.subsurfaceColor";
    if (name == "subsurface_radius") return "state.mat.subsurfaceRadiusScale";
    if (name == "subsurface_scale") return "1.0";
    if (name == "subsurface_anisotropy") return "state.mat.subsurfaceAnisotropy";
    if (name == "sheen") return "state.mat.sheen";
    if (name == "sheen_color") return "state.mat.fuzzColor";
    if (name == "sheen_roughness") return "state.mat.fuzzRoughness";
    if (name == "coat") return "state.mat.clearcoat";
    if (name == "coat_color") return "state.mat.coatColor";
    if (name == "coat_roughness") return "(1.0 - state.mat.clearcoatGloss)";
    if (name == "coat_anisotropy") return "state.mat.coatRoughnessAnisotropy";
    if (name == "coat_rotation") return "state.mat.coatAnisotropyRotation";
    if (name == "coat_IOR") return "state.mat.coatIOR";
    if (name == "coat_affect_color") return "state.mat.coatDarkening";
    if (name == "coat_affect_roughness") return "state.mat.coatAffectRoughness";
    if (name == "thin_film_thickness") return "state.mat.thinFilmThickness";
    if (name == "thin_film_IOR") return "state.mat.thinFilmIor";
    if (name == "thin_walled") return "(state.mat.thinWalled > 0.5)";
    if (name == "emission") return "1.0";
    if (name == "emission_color") return "state.mat.emission";
    if (name == "opacity") return "vec3(state.mat.opacity)";
    return "";
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

ShaderPtr PathTracerGlslShaderGenerator::generate(const string& name, ElementPtr element, GenContext& context) const
{
    // Fold constant standard_surface inputs (base_color, roughness, ...) as GLSL
    // literals rather than publishing them as unbound uniforms, so the path tracer
    // receives the authored material values without binding any uniforms.
    context.getOptions().shaderInterfaceType = SHADER_INTERFACE_REDUCED;
    return EsslShaderGenerator::generate(name, element, context);
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
    // Emit non-interface uniform blocks (private/sampler uniforms) as real
    // uniforms; the public material-parameter block is emitted as globals below
    // and bound from State.mat (materialsTex) instead of unbound uniforms.
    for (const auto& it : stage.getUniformBlocks())
    {
        const VariableBlock& uniforms = *it.second;
        if (!uniforms.empty() && uniforms.getName() != HW::LIGHT_DATA && uniforms.getName() != HW::PUBLIC_UNIFORMS)
        {
            // assignValue = false: GLSL ES forbids initializers on uniforms.
            emitVariableDeclarations(uniforms, _syntax->getUniformQualifier(), Syntax::SEMICOLON, context, stage, false);
            emitLineBreak(stage);
        }
    }

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

    // standard_surface parameter interface, emitted as mutable globals with their
    // authored default values; bound from State.mat (materialsTex) in
    // mtlxEvalSurface so per-material values come from the material texture.
    const VariableBlock& publicUniforms = stage.getUniformBlock(HW::PUBLIC_UNIFORMS);
    if (!publicUniforms.empty())
    {
        emitComment("standard_surface parameters (bound from State.mat in mtlxEvalSurface).", stage);
        emitVariableDeclarations(publicUniforms, EMPTY_STRING, Syntax::SEMICOLON, context, stage, true);
        emitLineBreak(stage);
    }

    emitFunctionDefinitions(graph, context, stage);
    emitLineBreak(stage);

    // --- Surface evaluation helper --------------------------------------------
    // Assembles the closure once for the current globals and returns the
    // surfaceshader whose .color holds the BSDF response (+ emission) for the
    // single (V, N, L) direction. Eval/Sample call this after setting the globals.
    const ShaderGraphOutputSocket* outputSocket = graph.getOutputSocket();
    emitLine("surfaceshader mtlxEvalSurface(State state)", stage, false);
    emitFunctionBodyBegin(graph, context, stage);
    // Bind the upstream geometric vertex-data variables from the path tracer State.
    for (size_t i = 0; i < vertexData.size(); ++i)
    {
        const ShaderPort* v = vertexData[i];
        emitLine(v->getVariable() + " = " + pathTracerStateSourceForVertexVar(v->getVariable(), _syntax->getTypeName(v->getType())), stage);
    }
    // Bind the standard_surface parameters from State.mat (materialsTex).
    for (size_t i = 0; i < publicUniforms.size(); ++i)
    {
        const ShaderPort* v = publicUniforms[i];
        const string binding = pathTracerMatBindingForInput(v->getName());
        if (!binding.empty())
        {
            emitLine(v->getVariable() + " = " + binding, stage);
        }
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
    emitLine("surfaceshader pt_surf = mtlxEvalSurface(state)", stage);
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
    emitLine("surfaceshader pt_surf = mtlxEvalSurface(state)", stage);
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
