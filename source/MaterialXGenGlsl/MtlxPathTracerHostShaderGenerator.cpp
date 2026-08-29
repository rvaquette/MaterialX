//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXGenGlsl/MtlxPathTracerHostShaderGenerator.h>

#include <MaterialXGenGlsl/GlslShaderGenerator.h>
#include <MaterialXGenGlsl/Nodes/MtlxPathTracerHostSurfaceNode.h>

#include <MaterialXGenHw/HwConstants.h>
#include <MaterialXGenShader/Exception.h>
#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/GenOptions.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/ShaderGraph.h>
#include <MaterialXGenShader/ShaderNode.h>
#include <MaterialXGenShader/ShaderNodeImpl.h>
#include <MaterialXGenShader/ShaderStage.h>
#include <MaterialXGenShader/Syntax.h>

#include <MaterialXCore/Document.h>
#include <MaterialXCore/Node.h>

#include <functional>
#include <initializer_list>
#include <set>

// Non-dependency contract (feature 003): this translation unit MUST NOT include
// or reference PathTracerGlslShaderGenerator. The design is inspired by
// EsslHostShaderGenerator only. Keep this guard to fail the build if that
// dependency is ever introduced by mistake.
#if defined(MATERIALX_PATHTRACERGLSLSHADERGENERATOR_H)
#error "MtlxPathTracerHostShaderGenerator must not depend on PathTracerGlslShaderGenerator"
#endif

MATERIALX_NAMESPACE_BEGIN

namespace
{

// Map an upstream world-space vertex-data variable (normalWorld, tangentWorld,
// ...) to the host global that carries the equivalent value. "bitangent" must be
// tested before "tangent" (it contains that substring).
string hostGlobalForVertexVar(const string& var, const string& typeName)
{
    if (var.find("normal") != string::npos)    return "g_ptN";
    if (var.find("bitangent") != string::npos) return "g_ptBitangent";
    if (var.find("tangent") != string::npos)   return "g_ptTangent";
    if (var.find("position") != string::npos)  return "g_ptP";
    if (var.find("texcoord") != string::npos)  return "g_ptTexcoord";
    return typeName + "(0.0)";
}

bool modelIsSupported(const StringVec& models, const string& model)
{
    for (const string& m : models)
    {
        if (m == model) return true;
    }
    return false;
}

} // anonymous namespace

// Identifier for this generator itself. Node implementations are still resolved
// through the inherited ESSL/GLSL target; this string identifies the pathtracer
// host dispatch generator (e.g. for the JS bindings).
const string MtlxPathTracerHostShaderGenerator::TARGET = "genglsl_mtlx_pathtracer_host";

const StringVec& MtlxPathTracerHostShaderGenerator::supportedMaterialModels()
{
    // Authoritative MaterialX node categories (as read from the .mtlx surface
    // node). Note the USD model's category is CamelCase "UsdPreviewSurface".
    static const StringVec models = {
        "open_pbr_surface",
        "standard_surface",
        "disney_principled",
        "gltf_pbr",
        "UsdPreviewSurface"
    };
    return models;
}

MtlxPathTracerHostShaderGenerator::MtlxPathTracerHostShaderGenerator(TypeSystemPtr typeSystem) :
    EsslHostShaderGenerator(typeSystem)
{
    // Reuse the ESSL host base (literal-folded material params, direct node
    // function emission, GLSL ES 3.00 syntax). Override the surface node so the
    // material assembly emits a single (V, N, L) closure sample (no forward light
    // loop); the dispatch entry points drive it via g_pt* globals.
    registerImplementation("IM_surface_" + GlslShaderGenerator::TARGET, MtlxPathTracerHostSurfaceNode::create);
}

ShaderPtr MtlxPathTracerHostShaderGenerator::generate(const string& name, ElementPtr element, GenContext& context) const
{
    // Resolve the material model authoritatively from the renderable element's
    // node category before the graph is flattened. A surfacematerial node exposes
    // the surface shader via its "surfaceshader" input; if the element already is
    // the surface shader node, its own category is the model.
    _resolvedMaterialModel.clear();
    if (NodePtr node = element ? element->asA<Node>() : nullptr)
    {
        if (NodePtr surf = node->getConnectedNode("surfaceshader"))
        {
            _resolvedMaterialModel = surf->getCategory();
        }
        if (_resolvedMaterialModel.empty())
        {
            _resolvedMaterialModel = node->getCategory();
        }
    }

    // Fold constant material inputs as GLSL literals so the generated dispatch
    // needs no material-parameter uniform binding.
    context.getOptions().shaderInterfaceType = SHADER_INTERFACE_REDUCED;
    return EsslHostShaderGenerator::generate(name, element, context);
}

MtlxPathTracerHostShaderGenerator::DispatchDiscovery
MtlxPathTracerHostShaderGenerator::discoverDispatch(const ShaderGraph& graph) const
{
    DispatchDiscovery discovery;

    // Discover shading closures (EDF/BSDF/BRDF/BTDF) recursively: they live inside
    // the surface node's implementation sub-graph (visited set guards cycles).
    std::set<const ShaderGraph*> visited;
    std::function<void(const ShaderGraph&)> scan = [&](const ShaderGraph& g)
    {
        if (!visited.insert(&g).second) return;
        for (const ShaderNode* node : g.getNodes())
        {
            if (!node) continue;
            const uint32_t c = node->getClassification();
            if (c & ShaderNode::Classification::EDF)    discovery.hasEDF = true;
            if (c & ShaderNode::Classification::BSDF_R) discovery.hasReflect = true;
            if (c & ShaderNode::Classification::BSDF_T) discovery.hasTransmit = true;
            if ((c & ShaderNode::Classification::BSDF) &&
                !(c & (ShaderNode::Classification::BSDF_R | ShaderNode::Classification::BSDF_T)))
            {
                discovery.hasReflect = true;
            }
            if (const ShaderGraph* sub = node->getImplementation().getGraph())
            {
                scan(*sub);
            }
        }
    };
    scan(graph);

    // Volume detection is top-level only: a nested VDF inside a surface's
    // transmission tree (e.g. open_pbr dielectric volume) is legitimate and must
    // not reject the surface. Only a top-level volume shader is unsupported in v1.
    for (const ShaderNode* node : graph.getNodes())
    {
        if (node && (node->getClassification() & ShaderNode::Classification::VDF))
        {
            discovery.hasVDF = true;
        }
    }

    // Locate the surface shader node connected to the graph output.
    const ShaderGraphOutputSocket* outSocket = graph.getOutputSocket();
    const ShaderOutput* connectedOut = outSocket ? outSocket->getConnection() : nullptr;
    const ShaderNode* surfaceNode = connectedOut ? connectedOut->getNode() : nullptr;
    if (!surfaceNode)
    {
        throwUnsupportedDispatch("no surface shader node connected to the graph output");
    }

    // The material model is the surface node's category, resolved authoritatively
    // from the renderable element in generate() (e.g. "open_pbr_surface").
    discovery.materialModel = _resolvedMaterialModel;
    if (discovery.materialModel.empty() ||
        !modelIsSupported(supportedMaterialModels(), discovery.materialModel))
    {
        throwUnsupportedDispatch(
            "unsupported material model '" + discovery.materialModel +
            "' for surface node '" + surfaceNode->getName() + "'");
    }

    return discovery;
}

void MtlxPathTracerHostShaderGenerator::emitPixelStage(const ShaderGraph& graph, GenContext& context, ShaderStage& stage) const
{
    // Discover the model/closures; fail explicitly for unsupported models and
    // reject volume closures (no path-tracer analogue in v1).
    const DispatchDiscovery discovery = discoverDispatch(graph);
    if (discovery.hasVDF)
    {
        throwUnsupportedDispatch("volume closure (VDF) is not supported for model '" + discovery.materialModel + "'");
    }
    // Closure-dependency completeness (T025): a dispatch needs at least one shading
    // closure (EDF/BSDF/BRDF/BTDF). emitFunctionDefinitions below emits every
    // closure/helper brick present in the graph; guard against a degenerate graph
    // with no shading closure to emit.
    if (!discovery.hasEDF && !discovery.hasReflect && !discovery.hasTransmit)
    {
        throwUnsupportedDispatch("no shading closure (EDF/BSDF/BRDF/BTDF) found for model '" + discovery.materialModel + "'");
    }
    // Sampling-strategy completeness (T031): the importance sampler emitted below
    // (sampleBsdf) is reflection-based and requires a reflection closure. A
    // pure-emissive (EDF-only) or pure-transmission material would yield an
    // incomplete/degenerate sampler; fail explicitly rather than approximate it.
    // (Dedicated dielectric transmission sampling is added in T049.)
    if (!discovery.hasReflect)
    {
        throwUnsupportedDispatch(
            "no reflection closure (BSDF_R) for model '" + discovery.materialModel +
            "'; the reflection sampler cannot represent an emissive-only or transmission-only "
            "material without approximation");
    }

    // --- Stage boilerplate ----------------------------------------------------
    emitDirectives(context, stage);
    emitLineBreak(stage);
    emitTypeDefinitions(context, stage);
    emitConstants(context, stage);

    // Emit private/sampler uniform blocks as real uniforms; the public material
    // parameters are emitted below as literal-folded globals instead.
    for (const auto& it : stage.getUniformBlocks())
    {
        const VariableBlock& uniforms = *it.second;
        if (!uniforms.empty() && uniforms.getName() != HW::LIGHT_DATA && uniforms.getName() != HW::PUBLIC_UNIFORMS)
        {
            emitVariableDeclarations(uniforms, _syntax->getUniformQualifier(), Syntax::SEMICOLON, context, stage, false);
            emitLineBreak(stage);
        }
    }

    // Common math + closure library required by the genglsl BSDF/EDF bricks.
    emitLibraryInclude("stdlib/genglsl/lib/mx_math.glsl", context, stage);
    emitLineBreak(stage);
    emitLine("#define DIRECTIONAL_ALBEDO_METHOD " + std::to_string(int(context.getOptions().hwDirectionalAlbedoMethod)), stage, false);
    emitLine("#define AIRY_FRESNEL_ITERATIONS " + std::to_string(context.getOptions().hwAiryFresnelIterations), stage, false);
    emitLineBreak(stage);
    // Environment closures are host-integrated; import the configured env/transmission
    // libraries so the bricks link (indirect branches are unused by the sampler).
    emitSpecularEnvironment(context, stage);
    emitTransmissionRender(context, stage);
    emitLineBreak(stage);

    // --- Host closure/geometry globals ----------------------------------------
    // The dispatch entry points set these before evaluating the surface closure.
    emitComment("Host closure globals (set by evaluateBsdf / sampleBsdf).", stage);
    emitLine("vec3 g_ptV", stage);
    emitLine("vec3 g_ptN", stage);
    emitLine("vec3 g_ptL", stage);
    emitLine("vec3 g_ptP", stage);
    emitLine("vec3 g_ptTangent", stage);
    emitLine("vec3 g_ptBitangent", stage);
    emitLine("vec2 g_ptTexcoord", stage);
    emitLine("int g_ptClosureType", stage);
    emitLine("float g_ptOcclusion = 1.0", stage);
    emitLine("int g_ptEmitEmission = 1", stage);
    // Vertex-data streams referenced by the graph, emitted as mutable globals
    // bound from the host globals inside mtlxHostEvalSurface().
    const VariableBlock& vertexData = stage.getInputBlock(HW::VERTEX_DATA);
    for (size_t i = 0; i < vertexData.size(); ++i)
    {
        const ShaderPort* v = vertexData[i];
        emitLine(_syntax->getTypeName(v->getType()) + " " + v->getVariable(), stage);
    }
    emitLineBreak(stage);

    // --- Material parameter globals (authored .mtlx values as GLSL literals) ---
    const VariableBlock& publicUniforms = stage.getUniformBlock(HW::PUBLIC_UNIFORMS);
    emitComment("__MTLX_PARAMS_BEGIN__", stage);
    if (!publicUniforms.empty())
    {
        emitVariableDeclarations(publicUniforms, EMPTY_STRING, Syntax::SEMICOLON, context, stage, true);
    }
    emitComment("__MTLX_PARAMS_END__", stage);
    emitLineBreak(stage);

    // Image/tiledimage nodes emit `#include "lib/$fileTransformUv"`; set the token.
    _tokenSubstitutions[ShaderGenerator::T_FILE_TRANSFORM_UV] =
        context.getOptions().fileTextureVerticalFlip ? "mx_transform_uv_vflip.glsl" : "mx_transform_uv.glsl";

    // MaterialX node function definitions (mx_* bricks + the single-sample surface
    // assembly emitted by MtlxPathTracerHostSurfaceNode).
    emitFunctionDefinitions(graph, context, stage);
    emitLineBreak(stage);

    // --- Surface evaluation helper --------------------------------------------
    // Assembles the closure once for the current globals and returns the
    // surfaceshader whose .color holds the BSDF response for the single (V, N, L)
    // direction selected by g_ptClosureType.
    const ShaderGraphOutputSocket* outputSocket = graph.getOutputSocket();
    emitLine("surfaceshader mtlxHostEvalSurface()", stage, false);
    emitFunctionBodyBegin(graph, context, stage);
    for (size_t i = 0; i < vertexData.size(); ++i)
    {
        const ShaderPort* v = vertexData[i];
        emitLine(v->getVariable() + " = " + hostGlobalForVertexVar(v->getVariable(), _syntax->getTypeName(v->getType())), stage);
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

    // --- evaluateBsdf (T023) --------------------------------------------------
    // Reflection BSDF response for the outgoing direction woutputL, given the
    // incident direction winputL (both in the local shading frame `basis`). The
    // host `Basis`, localToWorld() and PI are provided by the pathtracer route.
    emitComment("Generated pathtracer dispatch (feature 003). Model: " + discovery.materialModel, stage);
    emitLine("vec3 evaluateBsdf(in vec3 pW, in Basis basis, in vec3 winputL, in vec3 woutputL, in int material, inout float pdf_woutputL)", stage, false);
    emitScopeBegin(stage);
    emitLine("g_ptP = pW", stage);
    emitLine("g_ptN = basis.nW", stage);
    emitLine("g_ptTangent = basis.tW", stage);
    emitLine("g_ptBitangent = basis.bW", stage);
    emitLine("g_ptV = localToWorld(winputL, basis)", stage);
    emitLine("g_ptL = localToWorld(woutputL, basis)", stage);
    emitLine("g_ptOcclusion = 1.0", stage);
    emitLine("g_ptEmitEmission = 0", stage);
    emitLine("g_ptClosureType = CLOSURE_TYPE_REFLECTION", stage);
    // Cosine-weighted pdf for the reflection sample (refined by the sampler, T024).
    emitLine("pdf_woutputL = max(woutputL.z, 0.0) / PI", stage);
    emitLine("return mtlxHostEvalSurface().color", stage);
    emitScopeEnd(stage);
    emitLineBreak(stage);

    // --- sampleBsdf (T024) ----------------------------------------------------
    // Closure-aware one-sample importance sampler: clearcoat GGX, optional
    // transmission GGX, base GGX reflection, and cosine diffuse with a mixture
    // pdf. The response comes from the real closures via evaluateBsdf(); only the
    // outgoing direction and its pdf are analytic. Reuses the pathtracer route's
    // local-frame helpers (ggx_ndf_sample / ggx_ndf_eval / ggx_G1 /
    // FresnelDielectricReflectance / sampleHemisphereCosineWeighted /
    // pdfHemisphereCosineWeighted / rand).
    //
    // Material summary is derived from the folded parameter globals via
    // multi-name lookups so it works across the supported models.
    StringMap paramVar;
    for (size_t i = 0; i < publicUniforms.size(); ++i)
    {
        const ShaderPort* v = publicUniforms[i];
        paramVar[v->getName()] = v->getVariable();
    }
    auto pv = [&](std::initializer_list<const char*> names, const string& fallback) -> string
    {
        for (const char* n : names)
        {
            auto it = paramVar.find(n);
            if (it != paramVar.end()) return it->second;
        }
        return fallback;
    };
    // Per-model parameter mapping for the sampler summary. Each supported model
    // uses its authored input names explicitly; unknown models fall back to
    // generic multi-name lookups.
    string exMetal, exRough, exAniso, exBaseCol, exBaseW, exSpecCol, exSpecW, exIor;
    string exCoatW, exCoatRough, exCoatAniso, exCoatIor;
    if (discovery.materialModel == "open_pbr_surface")
    {
        exMetal   = pv({"base_metalness"}, "0.0");
        exRough   = pv({"specular_roughness"}, "0.3");
        exAniso   = pv({"specular_roughness_anisotropy"}, "0.0");
        exBaseCol = pv({"base_color"}, "vec3(0.8)");
        exBaseW   = pv({"base_weight"}, "1.0");
        exSpecCol = pv({"specular_color"}, "vec3(1.0)");
        exSpecW   = pv({"specular_weight"}, "1.0");
        exIor     = pv({"specular_ior"}, "1.5");
        exCoatW     = pv({"coat_weight"}, "0.0");
        exCoatRough = pv({"coat_roughness"}, "0.0");
        exCoatAniso = pv({"coat_roughness_anisotropy"}, "0.0");
        exCoatIor   = pv({"coat_ior"}, "1.5");
    }
    else if (discovery.materialModel == "standard_surface")
    {
        exMetal   = pv({"metalness"}, "0.0");
        exRough   = pv({"specular_roughness"}, "0.2");
        exAniso   = pv({"specular_anisotropy"}, "0.0");
        exBaseCol = pv({"base_color"}, "vec3(0.8)");
        exBaseW   = pv({"base"}, "1.0");
        exSpecCol = pv({"specular_color"}, "vec3(1.0)");
        exSpecW   = pv({"specular"}, "1.0");
        exIor     = pv({"specular_IOR"}, "1.5");
        exCoatW     = pv({"coat"}, "0.0");
        exCoatRough = pv({"coat_roughness"}, "0.0");
        exCoatAniso = pv({"coat_anisotropy"}, "0.0");
        exCoatIor   = pv({"coat_IOR"}, "1.5");
    }
    else if (discovery.materialModel == "disney_principled")
    {
        exMetal   = pv({"metallic"}, "0.0");
        exRough   = pv({"roughness"}, "0.5");
        exAniso   = pv({"anisotropic"}, "0.0");
        exBaseCol = pv({"baseColor"}, "vec3(0.16)");
        exBaseW   = "1.0"; // no base-weight input in disney_principled
        exSpecCol = "vec3(1.0)"; // disney uses scalar specular + specularTint, no specular color
        exSpecW   = pv({"specular"}, "0.5");
        exIor     = pv({"ior"}, "1.5");
        exCoatW     = pv({"clearcoat"}, "0.0");
        exCoatRough = "1.0 - " + pv({"clearcoatGloss"}, "1.0");
        exCoatAniso = "0.0";
        exCoatIor   = "1.5";
    }
    else if (discovery.materialModel == "gltf_pbr")
    {
        exMetal   = pv({"metallic"}, "1.0");
        exRough   = pv({"roughness"}, "1.0");
        exAniso   = "0.0";
        exBaseCol = pv({"base_color"}, "vec3(1.0)");
        exBaseW   = "1.0"; // no base-weight input in gltf_pbr
        exSpecCol = pv({"specular_color"}, "vec3(1.0)");
        exSpecW   = pv({"specular"}, "1.0");
        exIor     = pv({"ior"}, "1.5");
        exCoatW     = "0.0";
        exCoatRough = "0.0";
        exCoatAniso = "0.0";
        exCoatIor   = "1.5";
    }
    else if (discovery.materialModel == "UsdPreviewSurface")
    {
        // Metallic workflow: dielectric F0 from ior, base color = diffuseColor.
        exMetal   = pv({"metallic"}, "0.0");
        exRough   = pv({"roughness"}, "0.5");
        exAniso   = "0.0";
        exBaseCol = pv({"diffuseColor"}, "vec3(0.18)");
        exBaseW   = "1.0"; // no base-weight input in UsdPreviewSurface
        exSpecCol = "vec3(1.0)"; // specularColor applies only to the specular workflow
        exSpecW   = "1.0";
        exIor     = pv({"ior"}, "1.5");
        exCoatW     = "0.0";
        exCoatRough = "0.0";
        exCoatAniso = "0.0";
        exCoatIor   = "1.5";
    }
    else
    {
        exMetal   = pv({"metalness", "metallic", "base_metalness"}, "0.0");
        exRough   = pv({"specular_roughness", "roughness"}, "0.2");
        exAniso   = pv({"specular_roughness_anisotropy", "specular_anisotropy", "anisotropic"}, "0.0");
        exBaseCol = pv({"base_color", "baseColor", "diffuseColor"}, "vec3(0.8)");
        exBaseW   = pv({"base", "base_weight"}, "1.0");
        exSpecCol = pv({"specular_color"}, "vec3(1.0)");
        exSpecW   = pv({"specular", "specular_weight"}, "1.0");
        exIor     = pv({"specular_IOR", "ior", "specular_ior"}, "1.5");
        exCoatW     = pv({"coat_weight", "coat", "clearcoat"}, "0.0");
        exCoatRough = pv({"coat_roughness"}, "0.0");
        exCoatAniso = pv({"coat_roughness_anisotropy", "coat_anisotropy"}, "0.0");
        exCoatIor   = pv({"coat_ior", "coat_IOR"}, "1.5");
    }
    // Dielectric transmission params for the transmission lobe (T049). Missing on
    // metallic/USD models -> weight fallback 0.0 disables the lobe at runtime.
    const string exTransW = pv({"transmission_weight", "transmission", "specTrans"}, "0.0");
    const string exTransC = pv({"transmission_color", "attenuation_color"}, "vec3(1.0)");
    const string exTransD = pv({"transmission_depth", "attenuation_distance"}, "0.0");

    emitLine("vec3 sampleBsdf(in vec3 pW, in Basis basis, in vec3 winputL, inout uint rndSeed, in int material, out vec3 woutputL, out float pdf_woutputL, out Volume internal_medium)", stage, false);
    emitScopeBegin(stage);
    emitLine("internal_medium.extinction = vec3(0.0)", stage);
    emitLine("internal_medium.albedo = vec3(0.0)", stage);
    emitLine("internal_medium.anisotropy = 0.0", stage);
    // Material summary (folded params).
    emitLine("float m_metal = clamp(" + exMetal + ", 0.0, 1.0)", stage);
    emitLine("float m_rough = clamp(" + exRough + ", 0.0, 1.0)", stage);
    emitLine("float m_aniso = clamp(" + exAniso + ", 0.0, 0.99)", stage);
    emitLine("vec3  m_base  = (" + exBaseCol + ") * (" + exBaseW + ")", stage);
    emitLine("vec3  m_specC = " + exSpecCol, stage);
    emitLine("float m_specW = " + exSpecW, stage);
    emitLine("float m_ior   = max(" + exIor + ", 1.0 + 1e-3)", stage);
    emitLine("float m_coatW = clamp(" + exCoatW + ", 0.0, 1.0)", stage);
    emitLine("float m_coatRough = clamp(" + exCoatRough + ", 0.0, 1.0)", stage);
    emitLine("float m_coatAniso = clamp(" + exCoatAniso + ", 0.0, 0.99)", stage);
    emitLine("float m_coatIor = max(" + exCoatIor + ", 1.0 + 1e-3)", stage);
    emitLineBreak(stage);
    // Local incident direction (+Z is the shading normal); keep it in the upper hemisphere.
    emitLine("vec3 V = winputL", stage);
    emitLine("if (V.z < 0.0) V = -V", stage);
    emitLine("float NdotV = max(V.z, 1e-4)", stage);
    emitLine("float alpha = clamp(m_rough * m_rough, 1e-4, 1.0)", stage);
    emitLine("float anisoAspect = max(1e-4, 1.0 - m_aniso)", stage);
    emitLine("vec2 sampleAlpha = clamp(vec2(alpha * sqrt(2.0 / (anisoAspect * anisoAspect + 1.0)), alpha * anisoAspect * sqrt(2.0 / (anisoAspect * anisoAspect + 1.0))), vec2(1e-4), vec2(1.0))", stage);
    emitLine("float coatAlpha = clamp(m_coatRough * m_coatRough, 1e-4, 1.0)", stage);
    emitLine("float coatAnisoAspect = max(1e-4, 1.0 - m_coatAniso)", stage);
    emitLine("vec2 coatSampleAlpha = clamp(vec2(coatAlpha * sqrt(2.0 / (coatAnisoAspect * coatAnisoAspect + 1.0)), coatAlpha * coatAnisoAspect * sqrt(2.0 / (coatAnisoAspect * coatAnisoAspect + 1.0))), vec2(1e-4), vec2(1.0))", stage);
    // Fresnel-weighted lobe selection. Dielectric F0 is derived from the IOR
    // (open_pbr specular_ior); metals use the base color as F0.
    emitLine("float F0d = pow((m_ior - 1.0) / (m_ior + 1.0), 2.0)", stage);
    emitLine("vec3 F0 = mix(vec3(F0d) * max(m_specC, vec3(0.0)) * m_specW, m_base, m_metal)", stage);
    emitLine("float F0lum = max(F0.x, max(F0.y, F0.z))", stage);
    emitLine("float Fv = F0lum + (1.0 - F0lum) * pow(1.0 - NdotV, 5.0)", stage);
    emitLine("float coatFv = FresnelDielectricReflectance(NdotV, m_coatIor)", stage);
    emitLine("float pCoat = clamp(m_coatW * coatFv, 0.0, 0.75)", stage);
    emitLine("float xiLobe = rand(rndSeed)", stage);
    // Dielectric transmission lobe (T049). Selected with probability
    // transmission_weight * (1 - Fresnel); disabled (pTrans=0) when the model has
    // no transmission closure or authors zero weight. Reflection and transmission
    // supports are disjoint hemispheres, so the reflection pdf is scaled by
    // (1 - pTrans) below and no cross-hemisphere MIS term is needed.
    emitLine("float pTrans = 0.0", stage);
    if (discovery.hasTransmit)
    {
        emitLine("float m_transW = clamp(" + exTransW + ", 0.0, 1.0)", stage);
        emitLine("vec3  m_transC = " + exTransC, stage);
        emitLine("float m_transD = " + exTransD, stage);
        emitLine("pTrans = clamp(m_transW * (1.0 - Fv), 0.0, 0.95)", stage);
        emitLine("if (xiLobe < pCoat)", stage, false);
        emitScopeBegin(stage);
        emitLine("vec3 Hc = ggx_ndf_sample(V, coatSampleAlpha.x, coatSampleAlpha.y, rndSeed)", stage);
        emitLine("woutputL = reflect(-V, Hc)", stage);
        emitLine("if (woutputL.z <= 1e-4)", stage, false);
        emitScopeBegin(stage);
        emitLine("pdf_woutputL = 0.0", stage);
        emitLine("return vec3(0.0)", stage);
        emitScopeEnd(stage);
        emitLine("float pdfCoat = ggx_G1(V, coatSampleAlpha.x, coatSampleAlpha.y) * ggx_ndf_eval(normalize(V + woutputL), coatSampleAlpha.x, coatSampleAlpha.y) / (4.0 * NdotV)", stage);
        emitLine("float pdfBaseSpec = ggx_G1(V, sampleAlpha.x, sampleAlpha.y) * ggx_ndf_eval(normalize(V + woutputL), sampleAlpha.x, sampleAlpha.y) / (4.0 * NdotV)", stage);
        emitLine("float pdfBaseDiff = pdfHemisphereCosineWeighted(woutputL)", stage);
        emitLine("float diffLumCoat = (1.0 - m_metal) * dot(m_base, vec3(0.2126, 0.7152, 0.0722))", stage);
        emitLine("float pSpecCoat = clamp(Fv / (Fv + (1.0 - Fv) * diffLumCoat + 1e-3), 0.05, 0.95)", stage);
        emitLine("pdf_woutputL = max(pCoat * pdfCoat + (1.0 - pCoat) * (1.0 - pTrans) * (pSpecCoat * pdfBaseSpec + (1.0 - pSpecCoat) * pdfBaseDiff), PDF_EPSILON)", stage);
        emitLine("float ignorePdfCoat", stage);
        emitLine("return evaluateBsdf(pW, basis, winputL, woutputL, material, ignorePdfCoat)", stage);
        emitScopeEnd(stage);
        emitLine("if (xiLobe < pCoat + (1.0 - pCoat) * pTrans)", stage, false);
        emitScopeBegin(stage);
        // GGX micronormal + refraction through the interface. Use the same
        // convention as the legacy BTDF: winputL may be either entering or
        // exiting, and the sampled output direction must be returned in that
        // original local frame.
        emitLine("bool externalTransmission = (winputL.z > 0.0)", stage);
        emitLine("float etaRatio = externalTransmission ? (1.0 / m_ior) : m_ior", stage);
        emitLine("if (alpha <= 1e-3)", stage, false);
        emitScopeBegin(stage);
        emitLine("vec3 Hdelta = vec3(0.0, 0.0, externalTransmission ? 1.0 : -1.0)", stage);
        emitLine("float HdotWiDelta = dot(Hdelta, winputL)", stage);
        emitLine("float discrDelta = 1.0 - etaRatio * etaRatio * (1.0 - HdotWiDelta * HdotWiDelta)", stage);
        emitLine("if (discrDelta < 0.0)", stage, false);
        emitScopeBegin(stage);
        emitLine("woutputL = -winputL + 2.0 * dot(winputL, Hdelta) * Hdelta", stage);
        emitLine("pdf_woutputL = max((1.0 - pCoat) * pTrans, PDF_EPSILON)", stage);
        emitLine("return vec3(m_transW * pdf_woutputL / max(abs(woutputL.z), DENOM_TOLERANCE))", stage);
        emitScopeEnd(stage);
        emitLine("vec3 beamIncidentDelta = etaRatio * winputL - Hdelta * sign(HdotWiDelta) * (etaRatio * abs(HdotWiDelta) - sqrt(discrDelta))", stage);
        emitLine("woutputL = -normalize(beamIncidentDelta)", stage);
        emitLine("if (winputL.z * woutputL.z >= -1e-4)", stage, false);
        emitScopeBegin(stage);
        emitLine("pdf_woutputL = 0.0", stage);
        emitLine("return vec3(0.0)", stage);
        emitScopeEnd(stage);
        emitLine("if (m_transD > 0.0)", stage, false);
        emitScopeBegin(stage);
        emitLine("internal_medium.extinction = -log(max(vec3(1e-6), m_transC)) / m_transD", stage);
        emitScopeEnd(stage);
        emitLine("float Tdelta = clamp(1.0 - FresnelDielectricReflectance(abs(HdotWiDelta), 1.0 / etaRatio), 0.0, 1.0)", stage);
        emitLine("vec3 tintDelta = (m_transD == 0.0) ? m_transC : vec3(1.0)", stage);
        emitLine("pdf_woutputL = max((1.0 - pCoat) * pTrans, PDF_EPSILON)", stage);
        emitLine("return m_transW * tintDelta * Tdelta * pdf_woutputL / max(abs(woutputL.z), DENOM_TOLERANCE)", stage);
        emitScopeEnd(stage);
        emitLine("vec3 Vsample = winputL", stage);
        emitLine("if (Vsample.z < 0.0) Vsample.z *= -1.0", stage);
        emitLine("vec3 Ht = ggx_ndf_sample(Vsample, sampleAlpha.x, sampleAlpha.y, rndSeed)", stage);
        emitLine("if (winputL.z < 0.0) Ht.z *= -1.0", stage);
        emitLine("float HdotWi = dot(Ht, winputL)", stage);
        emitLine("float discr = 1.0 - etaRatio * etaRatio * (1.0 - HdotWi * HdotWi)", stage);
        // Reject total internal reflection and any non-transmitted direction.
        emitLine("if (discr < 0.0)", stage, false);
        emitScopeBegin(stage);
        emitLine("pdf_woutputL = 0.0", stage);
        emitLine("return vec3(0.0)", stage);
        emitScopeEnd(stage);
        emitLine("vec3 beamIncident = etaRatio * winputL - Ht * sign(HdotWi) * (etaRatio * abs(HdotWi) - sqrt(discr))", stage);
        emitLine("woutputL = -normalize(beamIncident)", stage);
        emitLine("if (winputL.z * woutputL.z >= -1e-4)", stage, false);
        emitScopeBegin(stage);
        emitLine("pdf_woutputL = 0.0", stage);
        emitLine("return vec3(0.0)", stage);
        emitScopeEnd(stage);
        // Beer-Lambert extinction of the internal medium (OpenPBR translucent base).
        emitLine("if (m_transD > 0.0)", stage, false);
        emitScopeBegin(stage);
        emitLine("internal_medium.extinction = -log(max(vec3(1e-6), m_transC)) / m_transD", stage);
        emitScopeEnd(stage);
        // Refraction pdf (Walter et al. 2007): VNDF density x half-vector Jacobian.
        emitLine("vec3 Hr = normalize(-(V + m_ior * woutputL))", stage);
        emitLine("if (Hr.z < 0.0) Hr = -Hr", stage);
        emitLine("float VoH = abs(dot(winputL, Ht))", stage);
        emitLine("float LoH = abs(dot(woutputL, Ht))", stage);
        emitLine("float denomT = LoH + etaRatio * VoH", stage);
        emitLine("float jacT = (etaRatio * etaRatio) * VoH / max(denomT * denomT, 1e-8)", stage);
        emitLine("float DvT = ggx_G1(Vsample, sampleAlpha.x, sampleAlpha.y) * VoH * ggx_ndf_eval(abs(Ht.z) > 0.0 ? vec3(Ht.x, Ht.y, abs(Ht.z)) : Ht, sampleAlpha.x, sampleAlpha.y) / max(abs(winputL.z), 1e-4)", stage);
        emitLine("pdf_woutputL = max((1.0 - pCoat) * pTrans * DvT * jacT, PDF_EPSILON)", stage);
        // Return a BTDF throughput for path continuation. MaterialX's surface
        // transmission closure is environment-integrated, so using it here would
        // double-count environment lighting and fail to transport the ray through
        // scene geometry.
        emitLine("float D = ggx_ndf_eval(abs(Ht.z) > 0.0 ? vec3(Ht.x, Ht.y, abs(Ht.z)) : Ht, sampleAlpha.x, sampleAlpha.y)", stage);
        emitLine("float G2 = ggx_G2(winputL, woutputL, sampleAlpha.x, sampleAlpha.y)", stage);
        emitLine("float etaRefl = 1.0 / etaRatio", stage);
        emitLine("float T = clamp(1.0 - FresnelDielectricReflectance(VoH, etaRefl), 0.0, 1.0)", stage);
        emitLine("vec3 tint = (m_transD == 0.0) ? m_transC : vec3(1.0)", stage);
        emitLine("return m_transW * tint * T * VoH * jacT * D * G2 / max(abs(woutputL.z) * abs(winputL.z), DENOM_TOLERANCE)", stage);
        emitScopeEnd(stage);
    }
    else
    {
        emitLine("if (xiLobe < pCoat)", stage, false);
        emitScopeBegin(stage);
        emitLine("vec3 Hc = ggx_ndf_sample(V, coatSampleAlpha.x, coatSampleAlpha.y, rndSeed)", stage);
        emitLine("woutputL = reflect(-V, Hc)", stage);
        emitLine("if (woutputL.z <= 1e-4)", stage, false);
        emitScopeBegin(stage);
        emitLine("pdf_woutputL = 0.0", stage);
        emitLine("return vec3(0.0)", stage);
        emitScopeEnd(stage);
        emitLine("float pdfCoat = ggx_G1(V, coatSampleAlpha.x, coatSampleAlpha.y) * ggx_ndf_eval(normalize(V + woutputL), coatSampleAlpha.x, coatSampleAlpha.y) / (4.0 * NdotV)", stage);
        emitLine("float pdfBaseSpec = ggx_G1(V, sampleAlpha.x, sampleAlpha.y) * ggx_ndf_eval(normalize(V + woutputL), sampleAlpha.x, sampleAlpha.y) / (4.0 * NdotV)", stage);
        emitLine("float pdfBaseDiff = pdfHemisphereCosineWeighted(woutputL)", stage);
        emitLine("float diffLumCoat = (1.0 - m_metal) * dot(m_base, vec3(0.2126, 0.7152, 0.0722))", stage);
        emitLine("float pSpecCoat = clamp(Fv / (Fv + (1.0 - Fv) * diffLumCoat + 1e-3), 0.05, 0.95)", stage);
        emitLine("pdf_woutputL = max(pCoat * pdfCoat + (1.0 - pCoat) * (pSpecCoat * pdfBaseSpec + (1.0 - pSpecCoat) * pdfBaseDiff), PDF_EPSILON)", stage);
        emitLine("float ignorePdfCoat", stage);
        emitLine("return evaluateBsdf(pW, basis, winputL, woutputL, material, ignorePdfCoat)", stage);
        emitScopeEnd(stage);
    }
    emitLine("float diffLum = (1.0 - m_metal) * dot(m_base, vec3(0.2126, 0.7152, 0.0722))", stage);
    emitLine("float pSpec = clamp(Fv / (Fv + (1.0 - Fv) * diffLum + 1e-3), 0.05, 0.95)", stage);
    emitLineBreak(stage);
    // Sample one lobe.
    emitLine("if (rand(rndSeed) < pSpec)", stage, false);
    emitScopeBegin(stage);
    emitLine("vec3 H = ggx_ndf_sample(V, sampleAlpha.x, sampleAlpha.y, rndSeed)", stage);
    emitLine("woutputL = reflect(-V, H)", stage);
    emitScopeEnd(stage);
    emitLine("else", stage, false);
    emitScopeBegin(stage);
    emitLine("float pdfTmp", stage);
    emitLine("woutputL = sampleHemisphereCosineWeighted(rndSeed, pdfTmp)", stage);
    emitScopeEnd(stage);
    emitLineBreak(stage);
    // Reject below-horizon samples (this sampler emits reflection only).
    emitLine("if (woutputL.z <= 1e-4)", stage, false);
    emitScopeBegin(stage);
    emitLine("pdf_woutputL = 0.0", stage);
    emitLine("return vec3(0.0)", stage);
    emitScopeEnd(stage);
    // MIS mixture pdf (specular VNDF + cosine diffuse).
    emitLine("vec3 Hh = normalize(V + woutputL)", stage);
    emitLine("float pdfSpec = ggx_G1(V, sampleAlpha.x, sampleAlpha.y) * ggx_ndf_eval(Hh, sampleAlpha.x, sampleAlpha.y) / (4.0 * NdotV)", stage);
    emitLine("float pdfDiff = pdfHemisphereCosineWeighted(woutputL)", stage);
    emitLine("float pdfCoat = ggx_G1(V, coatSampleAlpha.x, coatSampleAlpha.y) * ggx_ndf_eval(Hh, coatSampleAlpha.x, coatSampleAlpha.y) / (4.0 * NdotV)", stage);
    emitLine("pdf_woutputL = max(pCoat * pdfCoat + (1.0 - pCoat) * (1.0 - pTrans) * (pSpec * pdfSpec + (1.0 - pSpec) * pdfDiff), PDF_EPSILON)", stage);
    emitLineBreak(stage);
    // Response from the real closures for the sampled direction.
    emitLine("float ignorePdf", stage);
    emitLine("return evaluateBsdf(pW, basis, winputL, woutputL, material, ignorePdf)", stage);
    emitScopeEnd(stage);
}

void MtlxPathTracerHostShaderGenerator::throwUnsupportedDispatch(const string& detail) const
{
    throw ExceptionShaderGenError(
        "MtlxPathTracerHostShaderGenerator: unsupported or incomplete pathtracer dispatch: " + detail);
}

MATERIALX_NAMESPACE_END
