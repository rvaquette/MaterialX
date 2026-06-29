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
    if (name == "subsurface_scale") return "state.mat.subsurfaceScale";
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
    // T014: emission is owned by the path tracer integrator, which adds
    // `state.mat.emission` (folded color * weight) as radiance once per hit
    // (pathtrace.glsl: `radiance += state.mat.emission * throughput`). The
    // closure must therefore NOT fold the emission EDF into its BSDF response,
    // or emission would be double-counted and wrongly mixed into throughput /
    // direct-lighting MIS. Binding the emission weight to 0 zeroes the generated
    // EDF (emission_weight_out = emission_color * 0) while leaving the scattering
    // closures untouched.
    if (name == "emission") return "0.0";
    if (name == "emission_color") return "state.mat.emissionColor";
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

        // T016/FR-012/FR-014: textured inputs sample the path tracer texture
        // array (textureMapsArrayTex) at the per-material layer instead of being
        // reduced to a constant. Color inputs are linearized from sRGB (pow 2.2);
        // data inputs (roughness/metalness) stay linear. Guarded by the texID so
        // untextured materials keep their authored State.mat values (no change).
        // In pure mode GetMaterial defers these maps to the closure (see
        // pathtrace.glsl), so there is no double texturing.
        emitPathTracerTextureOverride(v->getName(), v->getVariable(), stage);
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
    emitLine("float pt_RefractAlpha(State state)", stage, false);
    emitScopeBegin(stage);
    emitLine("float r = clamp(state.mat.roughness + state.mat.transmissionExtraRoughness, 0.001, 1.0)", stage);
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
    emitLine("float etaEff = (state.mat.thinWalled > 0.5) ? 1.0 : state.eta", stage);
    emitLine("float aT = pt_RefractAlpha(state)", stage);
    emitLine("vec3 pt_Hraw = Vl + Ll * etaEff", stage);
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
    emitLine("vec3 tint = max(state.mat.transmissionColor, vec3(0.0))", stage);
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
    emitLine("float pt_metal = state.mat.metallic", stage);
    emitLine("float pt_wTrans = state.mat.specTrans * (1.0 - pt_metal)", stage);
    emitLine("vec3 pt_F0 = mix(vec3(0.04) * max(state.mat.specularColor, vec3(0.0)) * state.mat.specularWeight, state.mat.baseColor, pt_metal)", stage);
    emitLine("float pt_F0lum = max(pt_F0.x, max(pt_F0.y, pt_F0.z))", stage);
    emitLine("float pt_Fv = pt_F0lum + (1.0 - pt_F0lum) * pow(1.0 - pt_NDotV, 5.0)", stage);
    emitLine("float pt_diffLum = (1.0 - pt_metal) * (1.0 - state.mat.specTrans) * dot(state.mat.baseColor, vec3(0.2126, 0.7152, 0.0722))", stage);
    emitLine("float pt_pTrans = clamp(pt_wTrans * (1.0 - pt_Fv), 0.0, 0.9)", stage);
    emitLine("float pt_pSpec = clamp(pt_Fv / (pt_Fv + (1.0 - pt_Fv) * pt_diffLum + 1e-3), 0.1, 0.9)", stage);
    emitLine("if (pt_Ll.z < 0.0)", stage, false);
    emitScopeBegin(stage);
    emitLine("float pdfT", stage);
    emitLine("pt_RefractBtdf(state, pt_Vl, pt_Ll, pdfT)", stage);
    emitLine("return max(pt_pTrans * pdfT, 1e-6)", stage);
    emitScopeEnd(stage);
    emitLine("float pt_rough = clamp(state.mat.roughness, 0.001, 1.0)", stage);
    emitLine("float pt_a = max(pt_rough * pt_rough, 1e-4)", stage);
    emitLine("vec3 pt_H = normalize(pt_Vl + pt_Ll)", stage);
    emitLine("float pt_NDotH = clamp(pt_H.z, 0.0, 1.0)", stage);
    emitLine("float pt_specPdf = SmithG(pt_NDotV, pt_a) * GTR2(pt_NDotH, pt_a) / (4.0 * pt_NDotV)", stage);
    emitLine("float pt_diffPdf = max(pt_Ll.z, 1e-4) * INV_PI", stage);
    emitLine("return max((1.0 - pt_pTrans) * (pt_pSpec * pt_specPdf + (1.0 - pt_pSpec) * pt_diffPdf), 1e-6)", stage);
    emitScopeEnd(stage);
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
    emitLine("float pt_wTransL = state.mat.specTrans * (1.0 - state.mat.metallic)", stage);
    emitLine("return btdf * abs(pt_Ll.z) * pt_wTransL", stage);
    emitScopeEnd(stage);
    emitLine("g_ptV = V", stage);
    emitLine("g_ptN = N", stage);
    emitLine("g_ptL = L", stage);
    emitLine("g_ptP = state.fhp", stage);
    emitLine("g_ptTangent = state.tangent", stage);
    emitLine("g_ptBitangent = state.bitangent", stage);
    emitLine("g_ptTexcoord = state.texCoord", stage);
    emitLine("g_ptClosureType = CLOSURE_TYPE_REFLECTION", stage);
    emitLine("surfaceshader pt_surf = mtlxEvalSurface(state)", stage);
    emitLine("return pt_surf.color", stage);
    emitScopeEnd(stage);
    emitLineBreak(stage);

    emitLine("vec3 SampleMtlxPureClosure(int matID, State state, vec3 V, vec3 N, out vec3 L, out float pdf, out int flags)", stage, false);
    emitScopeBegin(stage);
    emitComment("T012/T013/T015: one-sample mixture (GGX specular reflection + cosine diffuse + rough dielectric transmission).", stage);
    emitLine("vec3 pt_T", stage);
    emitLine("vec3 pt_B", stage);
    emitLine("Onb(N, pt_T, pt_B)", stage);
    emitLine("vec3 pt_Vl = vec3(dot(V, pt_T), dot(V, pt_B), dot(V, N))", stage);
    emitLine("if (pt_Vl.z < 0.0) pt_Vl = -pt_Vl", stage);
    emitLine("float pt_NDotV = max(pt_Vl.z, 1e-4)", stage);
    emitLine("float pt_metal = state.mat.metallic", stage);
    emitLine("float pt_wTrans = state.mat.specTrans * (1.0 - pt_metal)", stage);
    emitLine("vec3 pt_F0 = mix(vec3(0.04) * max(state.mat.specularColor, vec3(0.0)) * state.mat.specularWeight, state.mat.baseColor, pt_metal)", stage);
    emitLine("float pt_F0lum = max(pt_F0.x, max(pt_F0.y, pt_F0.z))", stage);
    emitLine("float pt_Fv = pt_F0lum + (1.0 - pt_F0lum) * pow(1.0 - pt_NDotV, 5.0)", stage);
    emitLine("float pt_diffLum = (1.0 - pt_metal) * (1.0 - state.mat.specTrans) * dot(state.mat.baseColor, vec3(0.2126, 0.7152, 0.0722))", stage);
    emitLine("float pt_pTrans = clamp(pt_wTrans * (1.0 - pt_Fv), 0.0, 0.9)", stage);
    emitLine("float pt_pSpec = clamp(pt_Fv / (pt_Fv + (1.0 - pt_Fv) * pt_diffLum + 1e-3), 0.1, 0.9)", stage);
    emitLine("float pt_rough = clamp(state.mat.roughness, 0.001, 1.0)", stage);
    emitLine("float pt_a = max(pt_rough * pt_rough, 1e-4)", stage);
    emitLine("float pt_r1 = rand()", stage);
    emitLine("float pt_r2 = rand()", stage);
    emitLine("float pt_sel = rand()", stage);
    emitLine("vec3 pt_Ll", stage);
    emitLine("if (pt_sel < pt_pTrans)", stage, false);
    emitScopeBegin(stage);
    emitLine("float aT = pt_RefractAlpha(state)", stage);
    emitLine("vec3 pt_Hl = SampleGGXVNDF(pt_Vl, aT, aT, pt_r1, pt_r2)", stage);
    emitLine("if (pt_Hl.z < 0.0) pt_Hl = -pt_Hl", stage);
    emitLine("float etaEff = (state.mat.thinWalled > 0.5) ? 1.0 : state.eta", stage);
    emitLine("pt_Ll = refract(-pt_Vl, pt_Hl, etaEff)", stage);
    emitLine("if (dot(pt_Ll, pt_Ll) < 1e-8) pt_Ll = reflect(-pt_Vl, pt_Hl)", stage);
    emitLine("pt_Ll = normalize(pt_Ll)", stage);
    emitScopeEnd(stage);
    emitLine("else if (pt_sel < pt_pTrans + (1.0 - pt_pTrans) * pt_pSpec)", stage, false);
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
    emitLine("float pt_wTransL = state.mat.specTrans * (1.0 - state.mat.metallic)", stage);
    emitLine("return btdf * abs(pt_Ll2.z) * pt_wTransL", stage);
    emitScopeEnd(stage);
    emitLine("g_ptV = V", stage);
    emitLine("g_ptN = N", stage);
    emitLine("g_ptL = L", stage);
    emitLine("g_ptP = state.fhp", stage);
    emitLine("g_ptTangent = state.tangent", stage);
    emitLine("g_ptBitangent = state.bitangent", stage);
    emitLine("g_ptTexcoord = state.texCoord", stage);
    emitLine("g_ptClosureType = CLOSURE_TYPE_REFLECTION", stage);
    emitLine("surfaceshader pt_surf = mtlxEvalSurface(state)", stage);
    emitLine("return pt_surf.color", stage);
    emitScopeEnd(stage);
    emitLineBreak(stage);
}

void PathTracerGlslShaderGenerator::throwUnsupportedClosure(const string& nodeName, const string& reason) const
{
    throw ExceptionShaderGenError(
        "PathTracerGlslShaderGenerator: unsupported node/closure '" + nodeName + "': " + reason);
}

void PathTracerGlslShaderGenerator::emitPathTracerTextureOverride(const string& inputName, const string& inputVar, ShaderStage& stage) const
{
    // Map the standard_surface input to the path tracer texture slot (layer in
    // textureMapsArrayTex) and the channel/colorspace convention, mirroring
    // GetMaterial() in shaders/common/pathtrace.glsl so the generated closure
    // matches the path tracer texturing of the classic Material path.
    //   - baseColorTexID        : base color (sRGB) + opacity in .a
    //   - metallicRoughnessTexID : metalness (.b) / roughness (.g), linear data
    //   - emissionmapTexID       : emission color (sRGB)
    // The normal map is applied to State in GetMaterial (FR-013); the closure
    // consumes the perturbed frame and does not resample it here.
    string layer;
    string assign;
    if (inputName == "base_color")
    {
        layer = "state.mat.baseColorTexID";
        assign = inputVar + " *= pow(texture(textureMapsArrayTex, vec3(g_ptTexcoord * state.mat.uvScale, float(" + layer + "))).rgb, vec3(2.2))";
    }
    else if (inputName == "opacity")
    {
        layer = "state.mat.baseColorTexID";
        assign = inputVar + " *= texture(textureMapsArrayTex, vec3(g_ptTexcoord * state.mat.uvScale, float(" + layer + "))).a";
    }
    else if (inputName == "metalness")
    {
        layer = "state.mat.metallicRoughnessTexID";
        assign = inputVar + " = clamp(texture(textureMapsArrayTex, vec3(g_ptTexcoord * state.mat.uvScale, float(" + layer + "))).b, 0.0, 1.0)";
    }
    else if (inputName == "specular_roughness")
    {
        layer = "state.mat.metallicRoughnessTexID";
        assign = inputVar + " = texture(textureMapsArrayTex, vec3(g_ptTexcoord * state.mat.uvScale, float(" + layer + "))).g";
    }
    else if (inputName == "emission_color")
    {
        layer = "state.mat.emissionmapTexID";
        assign = inputVar + " = pow(texture(textureMapsArrayTex, vec3(g_ptTexcoord * state.mat.uvScale, float(" + layer + "))).rgb, vec3(2.2))";
    }
    else
    {
        return;
    }

    emitLine("if (" + layer + " >= 0)", stage, false);
    emitScopeBegin(stage);
    emitLine(assign, stage);
    emitScopeEnd(stage);
}

MATERIALX_NAMESPACE_END
