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

#include <initializer_list>

MATERIALX_NAMESPACE_BEGIN

namespace
{
// Map an upstream world-space vertex-data variable (normalWorld, tangentWorld,
// ...) to the path tracer global that carries the equivalent value from State.
// Order matters: "bitangentWorld" contains "tangentWorld" as a substring.
string pathTracerStateSourceForVertexVar(const string& var, const string& typeName)
{
    // Match every space (world/object/model) of each geometric stream: the path
    // tracer only tracks world-space frames (g_ptN/g_ptTangent/g_ptBitangent), so
    // map object/model variants to them too rather than letting them fall through
    // to vec3(0.0) (which silently breaks normal/tangent-driven graph nodes). NB:
    // "bitangent" must be tested before "tangent" (it contains that substring).
    if (var.find("normal") != string::npos)
    {
        return "g_ptN";
    }
    if (var.find("bitangent") != string::npos)
    {
        return "g_ptBitangent";
    }
    if (var.find("tangent") != string::npos)
    {
        return "g_ptTangent";
    }
    if (var.find("position") != string::npos)
    {
        // The path tracer only tracks the WORLD hit position (g_ptP = state.fhp).
        // Map EVERY position space (positionWorld / positionObject / positionModel)
        // to it: without the object's inverse transform we cannot recover true
        // object/model space, but using the world position keeps position-driven
        // nodes (e.g. procedural opacity masks) spatially varying instead of
        // collapsing to vec3(0.0) -> an object-space mask that reads 0 everywhere
        // makes the whole surface transparent (the object vanishes).
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

ShaderPtr PathTracerGlslShaderGenerator::generate(const string& name, ElementPtr element, GenContext& context) const
{
    // Fold constant standard_surface inputs (base_color, roughness, ...) as GLSL
    // literals rather than publishing them as unbound uniforms, so the path tracer
    // receives the authored material values without binding any uniforms.
    context.getOptions().shaderInterfaceType = SHADER_INTERFACE_REDUCED;
    // Our meshes are glTF: the loader stores V already flipped to GL convention
    // (state.texCoord.y = 1 - authoredV) for direct GL texture sampling. MaterialX
    // procedural coordinate nodes (texcoord/geomprop UV0, ramp*, etc.) expect the
    // authored V (origin top-left), so the closure entry points hand MaterialX
    // (x, 1 - state.texCoord.y) = authoredV. To keep IMAGE sampling identical we
    // enable fileTextureVerticalFlip so image nodes re-flip to GL convention
    // (mx_transform_uv_vflip), exactly matching the MaterialXView reference.
    context.getOptions().fileTextureVerticalFlip = true;
    return EsslShaderGenerator::generate(name, element, context);
}

void PathTracerGlslShaderGenerator::emitPixelStage(const ShaderGraph& graph, GenContext& context, ShaderStage& stage) const
{
    // ABI-sensitive contract for viewer substitution pipeline:
    // emitPixelStage must keep closure entrypoints compatible with
    // pt_InitMaterialSummary, EvalMtlxClosure, and SampleMtlxClosure.

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
    // with their authored .mtlx values instead of unbound uniforms.
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
    emitLine("#define DIRECTIONAL_ALBEDO_METHOD " + std::to_string(int(context.getOptions().hwDirectionalAlbedoMethod)), stage, false);
    emitLineBreak(stage);
    emitLine("#define AIRY_FRESNEL_ITERATIONS " + std::to_string(context.getOptions().hwAiryFresnelIterations), stage, false);
    emitLineBreak(stage);
    // Import the specular environment library selected by the configured method
    // (hwSpecularEnvironmentMethod: FIS / prefilter / none) instead of hardcoding
    // a single file, so the generated closure uses the correct mx_environment_*.glsl.
    // emitSpecularEnvironment also defines mx_environment_radiance, required by the
    // transmission render include below.
    emitSpecularEnvironment(context, stage);
    // Honor the configured transmission render method (default: refraction) by
    // delegating to the base generator's emitTransmissionRender instead of
    // hardcoding the opacity stub, so mx_surface_transmission matches the
    // MaterialX reference. u_refractionTwoSided is already declared from the
    // private uniform blocks emitted above.
    emitTransmissionRender(context, stage);
    emitLineBreak(stage);

    // --- Path tracer geometry / sampling globals ------------------------------
    // The upstream geometric nodes expect vertex-data variables (normalWorld,
    // tangentWorld, ...). Instead of declaring them as 'in' (no vertex stage in a
    // path tracer), emit them as mutable globals bound from State inside
    // mtlxEvalSurface, alongside the closure-direction globals.
    emitComment("Path tracer closure globals (set by the closure entry points).", stage);
    emitLine("vec3 g_ptV", stage);
    emitLine("vec3 g_ptN", stage);
    emitLine("vec3 g_ptL", stage);
    emitLine("vec3 g_ptP", stage);
    emitLine("vec3 g_ptTangent", stage);
    emitLine("vec3 g_ptBitangent", stage);
    emitLine("vec2 g_ptTexcoord", stage);
    emitLine("int g_ptClosureType", stage);
    // Occlusion applied inside the surface closure (per-light visibility in the
    // gather preview; 1.0 otherwise) and emission gate (1 = add EDF once, 0 = skip
    // so the gather's per-lobe passes don't double-count emission).
    emitLine("float g_ptOcclusion = 1.0", stage);
    emitLine("int g_ptEmitEmission = 1", stage);
    // Surface opacity evaluated by mtlxEvalSurface (procedural/nodegraph-driven
    // opacity masks): the host reads this after evaluating the surface to apply
    // stochastic coverage/cutout. 1.0 = fully opaque.
    emitLine("float g_ptOpacity = 1.0", stage);
    // Surface emission evaluated by mtlxEvalSurface (procedural/nodegraph-driven
    // emission_color, e.g. a UV/position debug that EMITS a coordinate). The host
    // reads this instead of the constant pt_mEmission summary when procedural.
    emitLine("vec3 g_ptEmission = vec3(0.0)", stage);
    const VariableBlock& vertexData = stage.getInputBlock(HW::VERTEX_DATA);
    for (size_t i = 0; i < vertexData.size(); ++i)
    {
        const ShaderPort* v = vertexData[i];
        emitLine(_syntax->getTypeName(v->getType()) + " " + v->getVariable(), stage);
    }
    emitLineBreak(stage);

    // standard_surface parameter globals with their authored/default .mtlx values.
    // Wrapped in markers so the multi-material assembler (sceneLoader) can turn the
    // per-material initializers into a single pt_LoadParams(matID) switch; the GLSL
    // structure is otherwise identical across standard_surface materials.
    const VariableBlock& publicUniforms = stage.getUniformBlock(HW::PUBLIC_UNIFORMS);
    emitComment("__MTLX_PARAMS_BEGIN__", stage);
    if (!publicUniforms.empty())
    {
        emitVariableDeclarations(publicUniforms, EMPTY_STRING, Syntax::SEMICOLON, context, stage, true);
    }
    emitComment("__MTLX_PARAMS_END__", stage);
    emitLineBreak(stage);

    // Lookup: standard_surface input name -> emitted global variable name, used by
    // pt_InitMaterialSummary() to derive the lobe-selection summary from the
    // authored parameter globals.
    StringMap paramVar;
    StringMap paramType;
    for (size_t i = 0; i < publicUniforms.size(); ++i)
    {
        const ShaderPort* v = publicUniforms[i];
        paramVar[v->getName()] = v->getVariable();
        paramType[v->getName()] = _syntax->getTypeName(v->getType());
    }
    auto pv = [&](std::initializer_list<const char*> names, const string& fallback) -> string
    {
        for (const char* n : names)
        {
            auto it = paramVar.find(n);
            if (it != paramVar.end())
                return it->second;
        }
        return fallback;
    };

    // Detect a procedural (nodegraph-driven) opacity/alpha input on the surface
    // shader node. With SHADER_INTERFACE_REDUCED a connected opacity is folded into
    // the graph body (NOT a published uniform), so pt_InitMaterialSummary cannot
    // read it and the host would treat the surface as fully opaque (e.g. a cutout
    // mask driven by position). When procedural we (a) flag it via pt_mProcOpacity
    // so the host evaluates the graph per hit, and (b) capture the computed value
    // into g_ptOpacity inside mtlxEvalSurface.
    const ShaderGraphOutputSocket* opOutSocket = graph.getOutputSocket();
    const ShaderNode* opSurfaceNode =
        (opOutSocket && opOutSocket->getConnection()) ? opOutSocket->getConnection()->getNode() : nullptr;
    const ShaderInput* opacityInput = nullptr;
    if (opSurfaceNode)
    {
        for (const char* n : {"opacity", "geometry_opacity", "alpha"})
        {
            const ShaderInput* in = opSurfaceNode->getInput(n);
            if (in) { opacityInput = in; break; }
        }
    }
    // A published-uniform opacity (constant, folded as a literal global) is handled
    // by pt_InitMaterialSummary; those inputs still report a graph connection, so
    // "procedural" must EXCLUDE them. Procedural == not a published param AND
    // connected (i.e. exactly the case the constant summary lookup misses).
    bool opacityIsPublishedParam = false;
    for (const char* n : {"opacity", "geometry_opacity", "alpha"})
        if (paramVar.find(n) != paramVar.end()) { opacityIsPublishedParam = true; break; }
    const bool opacityProcedural =
        !opacityIsPublishedParam && opacityInput && opacityInput->getConnection() != nullptr;

    // Same treatment for a nodegraph-driven emission_color (e.g. a UV/position
    // debug material that EMITS a coordinate). The constant summary pt_mEmission
    // only sees a published emission_color, so a graph-driven one reads vec3(0)
    // -> black. Detect it, flag pt_mProcEmission, and capture the evaluated
    // emission into g_ptEmission inside mtlxEvalSurface.
    const ShaderInput* emissionColorInput = nullptr;
    if (opSurfaceNode)
    {
        for (const char* n : {"emission_color", "emissive", "emissiveColor"})
        {
            const ShaderInput* in = opSurfaceNode->getInput(n);
            if (in) { emissionColorInput = in; break; }
        }
    }
    bool emissionColorIsPublishedParam = false;
    for (const char* n : {"emission_color", "emissive", "emissiveColor"})
        if (paramVar.find(n) != paramVar.end()) { emissionColorIsPublishedParam = true; break; }
    const bool emissionProcedural =
        !emissionColorIsPublishedParam && emissionColorInput && emissionColorInput->getConnection() != nullptr;

    // Lobe-selection material summary globals (constant initializers as required by
    // GLSL ES; the real values are assigned by pt_InitMaterialSummary() below,
    // after the parameter globals are loaded for the current matID).
    emitComment("Lobe-selection material summary (assigned by pt_InitMaterialSummary).", stage);
    emitLine("float pt_mMetal = 0.0", stage);
    emitLine("float pt_mSpecTrans = 0.0", stage);
    emitLine("vec3 pt_mBaseColor = vec3(0.0)", stage);
    emitLine("vec3 pt_mEmission = vec3(0.0)", stage);
    emitLine("vec3 pt_mSpecColor = vec3(0.0)", stage);
    emitLine("float pt_mSpecWeight = 0.0", stage);
    emitLine("float pt_mRough = 0.0", stage);
    emitLine("float pt_mTransExtraRough = 0.0", stage);
    emitLine("vec3 pt_mTransColor = vec3(0.0)", stage);
    emitLine("bool pt_mThinWalled = false", stage);
    emitLine("float pt_mIor = 1.5", stage);
    emitLine("float pt_mAnisotropy = 0.0", stage);
    emitLine("float pt_mAnisoRotDeg = 0.0", stage);
    emitLine("float pt_mOpacity = 1.0", stage);
    // True when opacity is nodegraph-driven (procedural mask); the host then
    // evaluates mtlxEvalSurface per hit to read the real coverage via g_ptOpacity.
    emitLine(string("bool pt_mProcOpacity = ") + (opacityProcedural ? "true" : "false"), stage);
    // True when emission_color is nodegraph-driven; the host then reads g_ptEmission
    // (evaluated per hit) instead of the constant pt_mEmission summary.
    emitLine(string("bool pt_mProcEmission = ") + (emissionProcedural ? "true" : "false"), stage);
    emitLine("float pt_mCoatWeight = 0.0", stage);
    emitLine("float pt_mCoatRough = 0.0", stage);
    emitLine("float pt_mCoatF0 = 0.0", stage);
    emitLineBreak(stage);

    // Set the include file used for UV transformations. Image/tiledimage nodes
    // emit `#include "lib/$fileTransformUv"`; the base GlslShaderGenerator sets
    // this token in its emitPixelStage, but we override that method so we must
    // replicate it here (otherwise the include stays literal and fails to load).
    _tokenSubstitutions[ShaderGenerator::T_FILE_TRANSFORM_UV] =
        context.getOptions().fileTextureVerticalFlip ? "mx_transform_uv_vflip.glsl" : "mx_transform_uv.glsl";

    // MaterialX node function definitions (mx_* bricks + the standard_surface
    // surface assembly, which references the g_pt* globals declared above).
    emitFunctionDefinitions(graph, context, stage);
    emitLineBreak(stage);

    // Assign the lobe-selection summary from the authored parameter globals. Called
    // by the closure entry points after pt_LoadParams(matID) has set the params.
    emitLine("void pt_InitMaterialSummary()", stage, false);
    emitScopeBegin(stage);
    emitLine("pt_mMetal = " + pv({"metalness", "metallic", "base_metalness"}, "0.0"), stage);
    emitLine("pt_mSpecTrans = " + pv({"transmission", "specTrans", "transmission_weight"}, "0.0"), stage);
    emitLine("pt_mBaseColor = (" + pv({"base_color", "baseColor", "diffuseColor"}, "vec3(0.8)") + ") * (" + pv({"base", "base_weight"}, "1.0") + ")", stage);
    // Emission color * strength. Strength fallback is 1.0 so models WITHOUT a
    // separate strength input (UsdPreviewSurface emissiveColor) still emit; models
    // WITH a weight (standard_surface emission=0 default, gltf emissive_strength)
    // use their real global.
    emitLine("pt_mEmission = (" + pv({"emission_color", "emissive", "emissiveColor"}, "vec3(0.0)") + ") * (" + pv({"emission", "emissive_strength", "emission_luminance"}, "1.0") + ")", stage);
    emitLine("pt_mSpecColor = " + pv({"specular_color"}, "vec3(1.0)"), stage);
    emitLine("pt_mSpecWeight = " + pv({"specular", "specular_weight"}, "1.0"), stage);
    emitLine("pt_mRough = " + pv({"specular_roughness", "roughness"}, "0.2"), stage);
    emitLine("pt_mTransExtraRough = " + pv({"transmission_extra_roughness"}, "0.0"), stage);
    // Transmission tint: use transmission_color when the model has one
    // (standard_surface / open_pbr), else fall back to the base color (gltf_pbr /
    // disney_principled use base color as the glass tint).
    emitLine("pt_mTransColor = " + pv({"transmission_color", "base_color", "baseColor"}, "vec3(1.0)"), stage);
    emitLine("pt_mThinWalled = " + pv({"thin_walled", "geometry_thin_walled"}, "false"), stage);
    emitLine("pt_mIor = " + pv({"specular_IOR", "ior", "specular_ior"}, "1.5"), stage);
    emitLine("pt_mAnisotropy = " + pv({"specular_anisotropy", "anisotropy_strength", "anisotropic", "specular_roughness_anisotropy"}, "0.0"), stage);
    {
        const string srot = pv({"specular_rotation"}, "");
        const string arot = pv({"anisotropy_rotation"}, "");
        if (!srot.empty())
            emitLine("pt_mAnisoRotDeg = (" + srot + ") * 360.0", stage);
        else if (!arot.empty())
            emitLine("pt_mAnisoRotDeg = (" + arot + ") * -57.2957795", stage);
        else
            emitLine("pt_mAnisoRotDeg = 0.0", stage);
    }
    {
        // Opacity: standard_surface authors a color3 `opacity` (use luminance);
        // UsdPreviewSurface / open_pbr use a float `opacity` / `geometry_opacity`;
        // gltf_pbr uses a float `alpha`. Pick the first authored one and emit the
        // type-correct assignment (dot() only valid on the color3 form).
        string opVar, opTy;
        for (const char* n : {"opacity", "geometry_opacity", "alpha"})
        {
            auto it = paramVar.find(n);
            if (it != paramVar.end()) { opVar = it->second; opTy = paramType[n]; break; }
        }
        if (opVar.empty())
            emitLine("pt_mOpacity = 1.0", stage);
        else if (opTy == "vec3")
            emitLine("pt_mOpacity = dot(" + opVar + ", vec3(0.2126, 0.7152, 0.0722))", stage);
        else
            emitLine("pt_mOpacity = " + opVar, stage);
    }
    // Coat lobe parameters: weight, roughness, and F0 derived from IOR.
    emitLine("pt_mCoatWeight = " + pv({"coat_weight"}, "0.0"), stage);
    emitLine("pt_mCoatRough  = " + pv({"coat_roughness"}, "0.0"), stage);
    {
        const string coatIor = pv({"coat_IOR", "coat_ior"}, "1.5");
        emitLine("{ float _ce = (" + coatIor + " - 1.0) / (" + coatIor + " + 1.0); pt_mCoatF0 = _ce * _ce; }", stage);
    }
    emitScopeEnd(stage);
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
    // The standard_surface parameters keep their authored/default values from the
    // .mtlx document (emitted as globals above); they are not read from the
    // material texture (State.mat) at all.
    emitFunctionCalls(graph, context, stage, ShaderNode::Classification::TEXTURE);
    // Capture a procedural opacity mask (computed by the texture/procedural node
    // stack above) so the host can apply stochastic coverage/cutout. Constant
    // opacity is handled by pt_InitMaterialSummary instead.
    if (opacityProcedural)
    {
        const string opVar = opacityInput->getConnection()->getVariable();
        if (_syntax->getTypeName(opacityInput->getType()) == "vec3")
            emitLine("g_ptOpacity = dot(" + opVar + ", vec3(0.2126, 0.7152, 0.0722))", stage);
        else
            emitLine("g_ptOpacity = " + opVar, stage);
    }
    // Capture a procedural emission_color (evaluated above) * its weight so the
    // host emits the graph-driven value instead of the constant pt_mEmission.
    if (emissionProcedural)
    {
        const string ecVar = emissionColorInput->getConnection()->getVariable();
        const string ewExpr = pv({"emission", "emissive_strength", "emission_luminance"}, "1.0");
        emitLine("g_ptEmission = (" + ecVar + ") * (" + ewExpr + ")", stage);
    }
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

    // --- Injectable BSDF stack (host-facing entry point) ----------------------
    // Thin wrapper the HOST (skeleton) calls per closure context: it sets the
    // g_pt* globals then delegates to mtlxEvalSurface (reusing its already-emitted
    // header + closure stack, so nodes are not emitted twice). Returns the BSDF
    // response ONLY (emission gated off via g_ptEmitEmission; the host adds
    // emission + opacity). Everything AFTER the __MTLX_STACK_END__ marker (gather /
    // Eval / Sample) is the stand-alone injection pipeline, stripped by
    // materialxMultiClosure.ts when the host provides its own integrator.
    emitLine("vec3 pt_MtlxLayerStackResponse(int closureType, vec3 L, vec3 V, vec3 N, vec3 P, vec3 T, float occlusion)", stage, false);
    emitScopeBegin(stage);
    emitLine("g_ptL = L", stage);
    emitLine("g_ptV = V", stage);
    emitLine("g_ptN = N", stage);
    emitLine("g_ptP = P", stage);
    emitLine("g_ptTangent = T", stage);
    emitLine("g_ptBitangent = cross(N, T)", stage);
    emitLine("g_ptClosureType = closureType", stage);
    emitLine("g_ptOcclusion = occlusion", stage);
    emitLine("g_ptEmitEmission = 0", stage);
    emitLine("State pt_s", stage);
    emitLine("return mtlxEvalSurface(pt_s).color", stage);
    emitScopeEnd(stage);
    emitLineBreak(stage);

    // --- Gather preview shading (OPT_MTLX_GATHER) -----------------------------
    // Single-pass shading that mirrors the MaterialX viewer (used by the low-res
    // preview): per-light REFLECTION + INDIRECT (environment) + TRANSMISSION and a
    // single EMISSION. It is host-coupled (scene lights + AnyHit shadow rays), so
    // it is wrapped in #ifdef OPT_MTLX_GATHER: it is only compiled when the host
    // shader provides that context, keeping the minimal-stub compile checks and the
    // recursive Eval/Sample path unaffected.
    emitLine("#ifdef OPT_MTLX_GATHER", stage, false);
    emitLineBreak(stage);

    // Convert a scene light to an equivalent directional sample (dir + irradiance).
    emitLine("vec3 pt_LightToDirectional(Light l, vec3 P, out vec3 dir, out float dist)", stage, false);
    emitScopeBegin(stage);
    emitLine("if (int(l.type) == DISTANT_LIGHT)", stage, false);
    emitScopeBegin(stage);
    emitLine("dir = normalize(l.position)", stage);
    emitLine("dist = INF", stage);
    emitLine("return l.emission", stage);
    emitScopeEnd(stage);
    emitLine("vec3 sp", stage);
    emitLine("vec3 ln", stage);
    emitLine("if (int(l.type) == QUAD_LIGHT)", stage, false);
    emitScopeBegin(stage);
    emitLine("sp = l.position + l.u * rand() + l.v * rand()", stage);
    emitLine("ln = normalize(cross(l.u, l.v))", stage);
    emitScopeEnd(stage);
    emitLine("else", stage, false);
    emitScopeBegin(stage);
    emitLine("sp = l.position + UniformSampleSphere(rand(), rand()) * l.radius", stage);
    emitLine("ln = normalize(sp - l.position)", stage);
    emitScopeEnd(stage);
    emitLine("vec3 dl = sp - P", stage);
    emitLine("dist = length(dl)", stage);
    emitLine("dir = dl / dist", stage);
    emitLine("float cosL = max(dot(-dir, ln), 0.0)", stage);
    emitLine("return l.emission * (l.area * cosL / max(dist * dist, EPS))", stage);
    emitScopeEnd(stage);
    emitLineBreak(stage);

    // Single-pass gather shading for the current hit (no recursion).
    emitLine("vec3 mtlxShadeGather(State state, Ray r)", stage, false);
    emitScopeBegin(stage);
    emitLine("int matID = state.matID", stage);
    emitLine("pt_InitMaterialSummary()", stage);
    emitLine("vec3 N = normalize(state.ffnormal)", stage);
    emitLine("vec3 V = -r.direction", stage);
    emitLine("vec3 P = state.fhp", stage);
    emitLine("g_ptV = V", stage);
    emitLine("g_ptN = N", stage);
    emitLine("g_ptP = P", stage);
    emitLine("g_ptTangent = state.tangent", stage);
    emitLine("g_ptBitangent = state.bitangent", stage);
    emitLine("g_ptTexcoord = vec2(state.texCoord.x, 1.0 - state.texCoord.y)", stage);
    emitLine("vec3 col = vec3(0.0)", stage);
    emitComment("(1) Direct lighting: REFLECTION closure per light (emission gated off).", stage);
    emitLine("g_ptEmitEmission = 0", stage);
    emitLine("for (int i = 0; i < numOfLights; i++)", stage, false);
    emitScopeBegin(stage);
    emitLine("int idx = i * 5", stage);
    emitLine("vec3 lp = texelFetch(lightsTex, ivec2(idx + 0, 0), 0).xyz", stage);
    emitLine("vec3 le = texelFetch(lightsTex, ivec2(idx + 1, 0), 0).xyz", stage);
    emitLine("vec3 lu = texelFetch(lightsTex, ivec2(idx + 2, 0), 0).xyz", stage);
    emitLine("vec3 lv = texelFetch(lightsTex, ivec2(idx + 3, 0), 0).xyz", stage);
    emitLine("vec3 lpar = texelFetch(lightsTex, ivec2(idx + 4, 0), 0).xyz", stage);
    emitLine("Light l = Light(lp, le, lu, lv, lpar.x, lpar.y, lpar.z)", stage);
    emitLine("vec3 Ldir", stage);
    emitLine("float dist", stage);
    emitLine("vec3 intensity = pt_LightToDirectional(l, P, Ldir, dist)", stage);
    emitLine("float occ = 1.0", stage);
    emitLine("Ray sray = Ray(P + N * EPS, Ldir)", stage);
    emitLine("if (AnyHit(sray, dist - 2.0 * EPS)) occ = 0.0", stage);
    emitLine("g_ptOcclusion = occ", stage);
    emitLine("g_ptL = Ldir", stage);
    emitLine("g_ptClosureType = CLOSURE_TYPE_REFLECTION", stage);
    emitLine("col += intensity * mtlxEvalSurface(state).color", stage);
    emitScopeEnd(stage);
    emitLine("g_ptOcclusion = 1.0", stage);
    emitComment("(2) Indirect environment radiance + (3) environment transmission.", stage);
    emitLine("g_ptL = vec3(0.0)", stage);
    emitLine("g_ptClosureType = CLOSURE_TYPE_INDIRECT", stage);
    emitLine("col += mtlxEvalSurface(state).color", stage);
    emitLine("g_ptClosureType = CLOSURE_TYPE_TRANSMISSION", stage);
    emitLine("col += mtlxEvalSurface(state).color", stage);
    emitComment("(4) Emission (added exactly once).", stage);
    emitLine("g_ptEmitEmission = 1", stage);
    emitLine("g_ptClosureType = CLOSURE_TYPE_EMISSION", stage);
    emitLine("col += mtlxEvalSurface(state).color", stage);
    emitLine("return col", stage);
    emitScopeEnd(stage);
    emitLineBreak(stage);
    emitLine("#endif", stage, false);
    emitLineBreak(stage);

    // --- Importance sampling (T012/T013/T015) ---------------------------------
    // One-sample mixture of three lobes shared by Eval/Sample:
    //   - GGX specular reflection (VNDF), response from the genglsl assembly;
    //   - cosine diffuse, response from the genglsl assembly;
    //   - rough dielectric transmission (T013), response synthesized below
    //     (the genglsl standard_surface transmission is an environment-map
    //     approximation, NOT a path-traceable BTDF, so we evaluate the
    //     microfacet refraction BTDF directly, matching the path tracer's
    //     EvalMicrofacetRefraction). Lobe probabilities derive from the view
    //     Fresnel (pt_Fv) and the transmission weight (specTrans * (1 - metal)).
    // Reuses path tracer helpers (SmithG/GTR2/Onb/SampleGGXVNDF/
    // CosineSampleHemisphere/DielectricFresnel) included before the injection.

    // Transmission roughness -> GGX alpha (adds transmission_extra_roughness).
    emitLine("float pt_RefractAlpha()", stage, false);
    emitScopeBegin(stage);
    emitLine("float r = clamp(pt_mRough + pt_mTransExtraRough, 0.001, 1.0)", stage);
    emitLine("return max(r * r, 1e-4)", stage);
    emitScopeEnd(stage);
    emitLineBreak(stage);

    // Rough dielectric refraction BTDF in the local frame (Ll.z < 0 = transmit).
    // Mirrors EvalMicrofacetRefraction (disney.glsl): returns the BTDF value
    // (without the cosine) and the transmission-lobe pdf in pdfT.
    emitLine("vec3 pt_RefractBtdf(State state, vec3 Vl, vec3 Ll, out float pdfT)", stage, false);
    emitScopeBegin(stage);
    emitLine("pdfT = 0.0", stage);
    emitLine("if (Ll.z >= 0.0) return vec3(0.0)", stage);
    emitLine("float etaEff = pt_mThinWalled ? 1.0 : state.eta", stage);
    emitLine("float aT = pt_RefractAlpha()", stage);
    emitComment("Walter/disney refraction half-vector: H = normalize(L + V*eta) (matches EvalMicrofacetRefraction). NOT V + L*eta.", stage);
    emitLine("vec3 pt_Hraw = Ll + Vl * etaEff", stage);
    emitComment("Degenerate half-vector (etaEff ~ 1, i.e. thin-walled straight transmission): no microfacet BTDF.", stage);
    emitLine("if (dot(pt_Hraw, pt_Hraw) < 1e-6) return vec3(0.0)", stage);
    emitLine("vec3 H = normalize(pt_Hraw)", stage);
    emitLine("if (H.z < 0.0) H = -H", stage);
    emitLine("float LDotH = dot(Ll, H)", stage);
    emitLine("float VDotH = dot(Vl, H)", stage);
    emitLine("float D = GTR2(H.z, aT)", stage);
    emitLine("float G1 = SmithG(abs(Vl.z), aT)", stage);
    emitLine("float G2 = G1 * SmithG(abs(Ll.z), aT)", stage);
    emitLine("float denom = LDotH + VDotH * etaEff", stage);
    emitLine("denom *= denom", stage);
    emitLine("float jacobian = abs(LDotH) / max(denom, 1e-7)", stage);
    emitLine("float F = DielectricFresnel(abs(VDotH), etaEff)", stage);
    emitLine("pdfT = G1 * max(0.0, VDotH) * D * jacobian / max(abs(Vl.z), 1e-4)", stage);
    emitLine("vec3 tint = max(pt_mTransColor, vec3(0.0))", stage);
    emitLine("return tint * (1.0 - F) * D * G2 * abs(VDotH) * jacobian * (etaEff * etaEff) / max(abs(Ll.z * Vl.z), 1e-5)", stage);
    emitScopeEnd(stage);
    emitLineBreak(stage);

    // Mixture pdf shared by Eval/Sample (reflection lobes + transmission lobe).
    emitLine("float pt_ClosurePdf(State state, vec3 V, vec3 N, vec3 L)", stage, false);
    emitScopeBegin(stage);
    emitLine("vec3 pt_T", stage);
    emitLine("vec3 pt_B", stage);
    emitLine("Onb(N, pt_T, pt_B)", stage);
    emitLine("vec3 pt_Vl = vec3(dot(V, pt_T), dot(V, pt_B), dot(V, N))", stage);
    emitLine("vec3 pt_Ll = vec3(dot(L, pt_T), dot(L, pt_B), dot(L, N))", stage);
    emitLine("float pt_NDotV = max(pt_Vl.z, 1e-4)", stage);
    emitLine("float pt_metal = pt_mMetal", stage);
    emitLine("float pt_wTrans = pt_mSpecTrans * (1.0 - pt_metal)", stage);
    emitLine("vec3 pt_F0 = mix(vec3(0.04) * max(pt_mSpecColor, vec3(0.0)) * pt_mSpecWeight, pt_mBaseColor, pt_metal)", stage);
    emitLine("float pt_F0lum = max(pt_F0.x, max(pt_F0.y, pt_F0.z))", stage);
    emitLine("float pt_Fv = pt_F0lum + (1.0 - pt_F0lum) * pow(1.0 - pt_NDotV, 5.0)", stage);
    emitLine("float pt_diffLum = (1.0 - pt_metal) * (1.0 - pt_mSpecTrans) * dot(pt_mBaseColor, vec3(0.2126, 0.7152, 0.0722))", stage);
    emitLine("float pt_pTrans = clamp(pt_wTrans * (1.0 - pt_Fv), 0.0, 0.9)", stage);
    emitLine("float pt_pSpec = clamp(pt_Fv / (pt_Fv + (1.0 - pt_Fv) * pt_diffLum + 1e-3), 0.01, 0.9)", stage);
    emitLine("if (pt_Ll.z < 0.0)", stage, false);
    emitScopeBegin(stage);
    emitLine("float pdfT", stage);
    emitLine("pt_RefractBtdf(state, pt_Vl, pt_Ll, pdfT)", stage);
    emitLine("return max(pt_pTrans * pdfT, 1e-6)", stage);
    emitScopeEnd(stage);
    emitLine("float pt_rough = clamp(pt_mRough, 0.001, 1.0)", stage);
    emitLine("float pt_a = max(pt_rough * pt_rough, 1e-4)", stage);
    emitLine("vec3 pt_H = normalize(pt_Vl + pt_Ll)", stage);
    emitLine("float pt_NDotH = clamp(pt_H.z, 0.0, 1.0)", stage);
    emitLine("float pt_specPdf = SmithG(pt_NDotV, pt_a) * GTR2(pt_NDotH, pt_a) / (4.0 * pt_NDotV)", stage);
    emitLine("float pt_diffPdf = max(pt_Ll.z, 1e-4) * INV_PI", stage);
    emitLine("float pt_coat_Fv = pt_mCoatF0 + (1.0 - pt_mCoatF0) * pow(1.0 - pt_NDotV, 5.0)", stage);
    emitLine("float pt_pCoat = clamp(pt_mCoatWeight * pt_coat_Fv, 0.0, 0.9)", stage);
    emitLine("float pt_coat_a = max(pt_mCoatRough * pt_mCoatRough, 1e-4)", stage);
    emitLine("float pt_coatPdf = SmithG(pt_NDotV, pt_coat_a) * GTR2(pt_NDotH, pt_coat_a) / (4.0 * pt_NDotV)", stage);
    emitLine("float pt_basePdf = pt_pSpec * pt_specPdf + (1.0 - pt_pSpec) * pt_diffPdf", stage);
    emitLine("return max((1.0 - pt_pTrans) * (pt_pCoat * pt_coatPdf + (1.0 - pt_pCoat) * pt_basePdf), 1e-6)", stage);
    emitScopeEnd(stage);
    emitLineBreak(stage);

    // --- Path tracer closure entry points -------------------------------------
    // Contract (shaders/common/mtlx_closure.glsl):
    //   vec3 EvalMtlxClosure(int matID, State state, vec3 V, vec3 N, vec3 L, out float pdf, out int flags);
    //   vec3 SampleMtlxClosure(int matID, State state, vec3 V, vec3 N, out vec3 L, out float pdf, out int flags);
    emitComment("Path tracer closure entry points (generated by PathTracerGlslShaderGenerator).", stage);
    emitLineBreak(stage);

    emitLine("vec3 EvalMtlxClosure(int matID, State state, vec3 V, vec3 N, vec3 L, out float pdf, out int flags)", stage, false);
    emitScopeBegin(stage);
    emitLine("pt_InitMaterialSummary()", stage);
    emitLine("bool isReflect = dot(N, L) >= 0.0", stage);
    emitLine("flags = isReflect ? CLOSURE_FLAG_REFLECT : CLOSURE_FLAG_TRANSMIT", stage);
    emitLine("pdf = pt_ClosurePdf(state, V, N, L)", stage);
    emitComment("Transmission (T013): synthesized microfacet refraction BTDF (genglsl transmission is env-based, not path-traceable).", stage);
    emitLine("if (!isReflect)", stage, false);
    emitScopeBegin(stage);
    emitLine("vec3 pt_T", stage);
    emitLine("vec3 pt_B", stage);
    emitLine("Onb(N, pt_T, pt_B)", stage);
    emitLine("vec3 pt_Vl = vec3(dot(V, pt_T), dot(V, pt_B), dot(V, N))", stage);
    emitLine("vec3 pt_Ll = vec3(dot(L, pt_T), dot(L, pt_B), dot(L, N))", stage);
    emitLine("float pdfT", stage);
    emitLine("vec3 btdf = pt_RefractBtdf(state, pt_Vl, pt_Ll, pdfT)", stage);
    emitComment("Weight by the standard_surface transmission layer (specTrans * (1 - metal)) so opaque materials do not transmit.", stage);
    emitLine("float pt_wTransL = pt_mSpecTrans * (1.0 - pt_mMetal)", stage);
    emitLine("return btdf * abs(pt_Ll.z) * pt_wTransL", stage);
    emitScopeEnd(stage);
    emitLine("g_ptV = V", stage);
    emitLine("g_ptN = N", stage);
    emitLine("g_ptL = L", stage);
    emitLine("g_ptP = state.fhp", stage);
    emitLine("g_ptTangent = state.tangent", stage);
    emitLine("g_ptBitangent = state.bitangent", stage);
    emitLine("g_ptTexcoord = vec2(state.texCoord.x, 1.0 - state.texCoord.y)", stage);
    emitLine("g_ptClosureType = CLOSURE_TYPE_REFLECTION", stage);
    emitLine("surfaceshader pt_surf = mtlxEvalSurface(state)", stage);
    emitLine("return pt_surf.color", stage);
    emitScopeEnd(stage);
    emitLineBreak(stage);

    emitLine("vec3 SampleMtlxClosure(int matID, State state, vec3 V, vec3 N, out vec3 L, out float pdf, out int flags)", stage, false);
    emitScopeBegin(stage);
    emitLine("pt_InitMaterialSummary()", stage);
    emitComment("T012/T013/T015: one-sample mixture (GGX specular reflection + cosine diffuse + rough dielectric transmission).", stage);
    emitLine("vec3 pt_T", stage);
    emitLine("vec3 pt_B", stage);
    emitLine("Onb(N, pt_T, pt_B)", stage);
    emitLine("vec3 pt_Vl = vec3(dot(V, pt_T), dot(V, pt_B), dot(V, N))", stage);
    emitLine("if (pt_Vl.z < 0.0) pt_Vl = -pt_Vl", stage);
    emitLine("float pt_NDotV = max(pt_Vl.z, 1e-4)", stage);
    emitLine("float pt_metal = pt_mMetal", stage);
    emitLine("float pt_wTrans = pt_mSpecTrans * (1.0 - pt_metal)", stage);
    emitLine("vec3 pt_F0 = mix(vec3(0.04) * max(pt_mSpecColor, vec3(0.0)) * pt_mSpecWeight, pt_mBaseColor, pt_metal)", stage);
    emitLine("float pt_F0lum = max(pt_F0.x, max(pt_F0.y, pt_F0.z))", stage);
    emitLine("float pt_Fv = pt_F0lum + (1.0 - pt_F0lum) * pow(1.0 - pt_NDotV, 5.0)", stage);
    emitLine("float pt_diffLum = (1.0 - pt_metal) * (1.0 - pt_mSpecTrans) * dot(pt_mBaseColor, vec3(0.2126, 0.7152, 0.0722))", stage);
    emitLine("float pt_pTrans = clamp(pt_wTrans * (1.0 - pt_Fv), 0.0, 0.9)", stage);
    emitLine("float pt_pSpec = clamp(pt_Fv / (pt_Fv + (1.0 - pt_Fv) * pt_diffLum + 1e-3), 0.01, 0.9)", stage);
    emitLine("float pt_coat_Fv_s = pt_mCoatF0 + (1.0 - pt_mCoatF0) * pow(1.0 - pt_NDotV, 5.0)", stage);
    emitLine("float pt_pCoat_s = clamp(pt_mCoatWeight * pt_coat_Fv_s, 0.0, 0.9)", stage);
    emitLine("float pt_rough = clamp(pt_mRough, 0.001, 1.0)", stage);
    emitLine("float pt_a = max(pt_rough * pt_rough, 1e-4)", stage);
    emitLine("float pt_coat_a_s = max(pt_mCoatRough * pt_mCoatRough, 1e-4)", stage);
    emitLine("float pt_r1 = rand()", stage);
    emitLine("float pt_r2 = rand()", stage);
    emitLine("float pt_sel = rand()", stage);
    emitLine("vec3 pt_Ll", stage);
    emitLine("if (pt_sel < pt_pTrans)", stage, false);
    emitScopeBegin(stage);
    emitLine("float aT = pt_RefractAlpha()", stage);
    emitLine("vec3 pt_Hl = SampleGGXVNDF(pt_Vl, aT, aT, pt_r1, pt_r2)", stage);
    emitLine("if (pt_Hl.z < 0.0) pt_Hl = -pt_Hl", stage);
    emitLine("float etaEff = pt_mThinWalled ? 1.0 : state.eta", stage);
    emitLine("pt_Ll = refract(-pt_Vl, pt_Hl, etaEff)", stage);
    emitLine("if (dot(pt_Ll, pt_Ll) < 1e-8) pt_Ll = reflect(-pt_Vl, pt_Hl)", stage);
    emitLine("pt_Ll = normalize(pt_Ll)", stage);
    emitScopeEnd(stage);
    emitLine("else if (pt_sel < pt_pTrans + (1.0 - pt_pTrans) * pt_pCoat_s)", stage, false);
    emitScopeBegin(stage);
    emitLine("vec3 pt_Hl_coat = SampleGGXVNDF(pt_Vl, pt_coat_a_s, pt_coat_a_s, pt_r1, pt_r2)", stage);
    emitLine("if (pt_Hl_coat.z < 0.0) pt_Hl_coat = -pt_Hl_coat", stage);
    emitLine("pt_Ll = reflect(-pt_Vl, pt_Hl_coat)", stage);
    emitComment("Coat only reflects; clamp below-horizon directions to avoid false transmission detection.", stage);
    emitLine("if (pt_Ll.z < 1e-4) pt_Ll = vec3(-pt_Ll.x, -pt_Ll.y, 1e-4)", stage);
    emitLine("pt_Ll = normalize(pt_Ll)", stage);
    emitScopeEnd(stage);
    emitLine("else if (pt_sel < pt_pTrans + (1.0 - pt_pTrans) * (pt_pCoat_s + (1.0 - pt_pCoat_s) * pt_pSpec))", stage, false);
    emitScopeBegin(stage);
    emitLine("vec3 pt_Hl = SampleGGXVNDF(pt_Vl, pt_a, pt_a, pt_r1, pt_r2)", stage);
    emitLine("if (pt_Hl.z < 0.0) pt_Hl = -pt_Hl", stage);
    emitLine("pt_Ll = reflect(-pt_Vl, pt_Hl)", stage);
    emitScopeEnd(stage);
    emitLine("else", stage, false);
    emitScopeBegin(stage);
    emitLine("pt_Ll = CosineSampleHemisphere(pt_r1, pt_r2)", stage);
    emitScopeEnd(stage);
    emitLine("L = normalize(pt_T * pt_Ll.x + pt_B * pt_Ll.y + N * pt_Ll.z)", stage);
    emitLine("bool isReflect = dot(N, L) >= 0.0", stage);
    emitLine("flags = isReflect ? CLOSURE_FLAG_REFLECT : CLOSURE_FLAG_TRANSMIT", stage);
    emitLine("pdf = pt_ClosurePdf(state, V, N, L)", stage);
    emitLine("if (pdf <= 0.0)", stage, false);
    emitScopeBegin(stage);
    emitLine("pdf = 0.0", stage);
    emitLine("return vec3(0.0)", stage);
    emitScopeEnd(stage);
    emitLine("if (!isReflect)", stage, false);
    emitScopeBegin(stage);
    emitLine("vec3 pt_Vl2 = vec3(dot(V, pt_T), dot(V, pt_B), dot(V, N))", stage);
    emitLine("vec3 pt_Ll2 = vec3(dot(L, pt_T), dot(L, pt_B), dot(L, N))", stage);
    emitLine("float pdfT", stage);
    emitLine("vec3 btdf = pt_RefractBtdf(state, pt_Vl2, pt_Ll2, pdfT)", stage);
    emitLine("float pt_wTransL = pt_mSpecTrans * (1.0 - pt_mMetal)", stage);
    emitLine("return btdf * abs(pt_Ll2.z) * pt_wTransL", stage);
    emitScopeEnd(stage);
    emitLine("g_ptV = V", stage);
    emitLine("g_ptN = N", stage);
    emitLine("g_ptL = L", stage);
    emitLine("g_ptP = state.fhp", stage);
    emitLine("g_ptTangent = state.tangent", stage);
    emitLine("g_ptBitangent = state.bitangent", stage);
    emitLine("g_ptTexcoord = vec2(state.texCoord.x, 1.0 - state.texCoord.y)", stage);
    emitLine("g_ptClosureType = CLOSURE_TYPE_REFLECTION", stage);
    emitLine("surfaceshader pt_surf = mtlxEvalSurface(state)", stage);
    emitLine("return pt_surf.color", stage);
    emitScopeEnd(stage);
    emitLineBreak(stage);

    // Mark end of injectable closure code. Everything above this line is kept by
    // the multi-material assembler (materialxMultiClosure.ts); nothing follows.
    emitLine("// __MTLX_STACK_END__", stage, false);
    emitLineBreak(stage);
}

void PathTracerGlslShaderGenerator::throwUnsupportedClosure(const string& nodeName, const string& reason) const
{
    throw ExceptionShaderGenError(
        "PathTracerGlslShaderGenerator: unsupported node/closure '" + nodeName + "': " + reason);
}

MATERIALX_NAMESPACE_END
