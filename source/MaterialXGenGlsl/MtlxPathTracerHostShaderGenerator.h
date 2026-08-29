//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#ifndef MATERIALX_MTLXPATHTRACERHOSTSHADERGENERATOR_H
#define MATERIALX_MTLXPATHTRACERHOSTSHADERGENERATOR_H

/// @file
/// MaterialX path tracer host dispatch generator.
///
/// Emits per-material pathtracer host dispatch GLSL (evaluateBsdf / sampleBsdf)
/// for a selected .mtlx material, wired to MaterialX-generated closure code
/// (EDF/BSDF/BRDF/BTDF). Design is inspired by EsslHostShaderGenerator's
/// host-shader responsibilities (fold material params as literals, emit graph
/// node functions directly, wire geometric/host contract), NOT by
/// PathTracerGlslShaderGenerator.
///
/// Scope (feature 003, first delivery): open_pbr_surface, standard_surface,
/// disney_principled, gltf_pbr, usd_preview_surface.
///
/// Non-dependency contract: this generator MUST NOT use
/// PathTracerGlslShaderGenerator as an implementation class, base class, or
/// required pipeline dependency. Any adapted helper concept must be copied into
/// this generator and documented here.
///
/// Failure contract: unsupported material models, missing generated closures,
/// signature mismatches, or incomplete evaluate/sample/pdf strategies MUST fail
/// explicitly. No generic approximation and no legacy BXDF fallback are allowed.
///
/// Usage:
///   C++/WASM:  auto gen = MtlxPathTracerHostShaderGenerator::create();
///              GenContext ctx(gen); loadStandardLibraries(ctx);
///              auto elem = findRenderableElement(doc);
///              string glsl = gen->generate(elem->getNamePath(), elem, ctx)
///                                ->getSourceCode("pixel");
///   The emitted `pixel` source is self-contained (folded params, closure
///   library, evaluateBsdf/sampleBsdf) and depends only on host helpers provided
///   by the pathtracer route (Basis, Volume, PI, ggx_*, localToWorld, rand,
///   FresnelDielectricReflectance, sampleHemisphereCosineWeighted,
///   pdfHemisphereCosineWeighted, PDF_EPSILON).
///   Node.js wrapper: tools/generate-mtlx-pathtracer-dispatch.mjs (OpenPBR-viewer)
///   writes glsl/pathtracing/mtlx/generated/<material-id>/generated_bsdf_dispatch.glsl.

#include <MaterialXGenGlsl/EsslHostShaderGenerator.h>

MATERIALX_NAMESPACE_BEGIN

using MtlxPathTracerHostShaderGeneratorPtr = shared_ptr<class MtlxPathTracerHostShaderGenerator>;

/// @class MtlxPathTracerHostShaderGenerator
/// Host generator that emits pathtracer BSDF dispatch (evaluateBsdf/sampleBsdf)
/// from a MaterialX material closure graph. Derives from EsslHostShaderGenerator
/// to reuse GLSL ES 3.00 host emission (literal-folded material params, direct
/// node-function emission), while replacing the forward-shading pixel stage with
/// pathtracer dispatch emission.
class MX_GENGLSL_API MtlxPathTracerHostShaderGenerator : public EsslHostShaderGenerator
{
  public:
    /// Constructor.
    MtlxPathTracerHostShaderGenerator(TypeSystemPtr typeSystem);

    /// Creator function.
    /// If a TypeSystem is not provided it will be created internally.
    static ShaderGeneratorPtr create(TypeSystemPtr typeSystem = nullptr)
    {
        return std::make_shared<MtlxPathTracerHostShaderGenerator>(typeSystem ? typeSystem : TypeSystem::create());
    }

    /// Generate the per-material pathtracer dispatch shader. Forces a reduced
    /// shader interface so constant MaterialX inputs are folded as GLSL literals.
    ShaderPtr generate(const string& name, ElementPtr element, GenContext& context) const override;

  public:
    /// Unique identifier for this generator (pathtracer host dispatch GLSL).
    static const string TARGET;

    /// Supported MaterialX material models for the first delivery.
    static const StringVec& supportedMaterialModels();

    /// Result of closure/model discovery for a material graph (feature 003, T022).
    /// `materialModel` is one of supportedMaterialModels(); the closure flags
    /// summarize which closure categories the graph actually contains.
    struct DispatchDiscovery
    {
        string materialModel;
        bool hasEDF = false;      ///< graph contains an emission closure
        bool hasReflect = false;  ///< graph contains a reflection BSDF/BRDF closure
        bool hasTransmit = false; ///< graph contains a transmission BSDF/BTDF closure
        bool hasVDF = false;      ///< graph contains a volume closure (unsupported in v1)
    };

  protected:
    /// Emit the pathtracer dispatch stage (evaluateBsdf / sampleBsdf) in place of
    /// the forward-shading pixel stage, consuming MaterialX-generated closures.
    void emitPixelStage(const ShaderGraph& graph, GenContext& context, ShaderStage& stage) const override;

    /// Discover the material model and closure categories present in `graph`.
    /// Throws (via throwUnsupportedDispatch) when the surface node is missing or
    /// the material model is not in supportedMaterialModels().
    DispatchDiscovery discoverDispatch(const ShaderGraph& graph) const;

    /// Raise a named, explicit error for an unsupported material model or an
    /// incomplete evaluate/sample/pdf strategy. Guarantees no silent fallback.
    [[noreturn]] void throwUnsupportedDispatch(const string& detail) const;

  private:
    /// Material model category resolved authoritatively from the renderable
    /// element in generate() (e.g. "open_pbr_surface"), consumed by
    /// discoverDispatch(). Single-threaded per-generate use (JS/WASM); reset at
    /// the start of each generate() call.
    mutable string _resolvedMaterialModel;
};

MATERIALX_NAMESPACE_END

#endif
