//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#ifndef MATERIALX_ESSLHOSTSHADERGENERATOR_H
#define MATERIALX_ESSLHOSTSHADERGENERATOR_H

/// @file
/// ESSL "host" shader generator for the WebGL2 path tracer raster host.
///
/// This is the C++ counterpart of the previous TypeScript post-processing
/// (adaptEsslForHost / buildEsslMaterialParamBlock / bakeEsslMaterialParams).
/// Instead of splicing the stock EsslShaderGenerator output with regex, this
/// generator emits GLSL ES 3.00 that is directly compatible with the app's
/// fullscreen raster host (shaders/skeleton-essl.glsl):
///
///   - material inputs are folded as correctly-typed GLSL literals (no unbound
///     uniforms, no int/float mismatches, no graph-internal inputs mistaken for
///     material parameters);
///   - node functions of the whole graph are emitted by the generator itself
///     (nothing dropped by text extraction);
///   - geometric streams and the environment contract are wired to the host
///     symbols (added incrementally, see the .cpp TODO markers).

#include <MaterialXGenGlsl/EsslShaderGenerator.h>

MATERIALX_NAMESPACE_BEGIN

using EsslHostShaderGeneratorPtr = shared_ptr<class EsslHostShaderGenerator>;

/// @class EsslHostShaderGenerator
/// A stock-ESSL forward-shading generator specialized for the path tracer raster
/// host: it reuses EsslShaderGenerator's pixel stage (standard_surface light loop
/// + environment) but folds constant material inputs as GLSL literals so the
/// output needs no material-parameter uniform binding.
class MX_GENGLSL_API EsslHostShaderGenerator : public EsslShaderGenerator
{
  public:
    /// Constructor.
    EsslHostShaderGenerator(TypeSystemPtr typeSystem);

    /// Creator function.
    /// If a TypeSystem is not provided it will be created internally.
    static ShaderGeneratorPtr create(TypeSystemPtr typeSystem = nullptr)
    {
        return std::make_shared<EsslHostShaderGenerator>(typeSystem ? typeSystem : TypeSystem::create());
    }

    /// Generate the ESSL host shader. Forces a reduced shader interface so that
    /// constant MaterialX inputs (base_color, roughness, uv0_index, ...) are
    /// folded as correctly-typed GLSL literals instead of being published as
    /// unbound uniforms.
    ShaderPtr generate(const string& name, ElementPtr element, GenContext& context) const override;

  public:
    /// Unique identifier for this generator (ESSL raster host GLSL).
    static const string TARGET;

  protected:
    /// Emit the shader inputs. For the pixel stage, geometric streams
    /// (normalWorld, tangentWorld, positionWorld, texcoord_0, ...) are emitted as
    /// mutable globals fed by the host through pt_MtlxBindGeom() instead of vertex
    /// `in` varyings, because the path tracer raster host has no vertex stage
    /// (fixes "texcoord_0 undeclared" and removes the host-side stream guesswork).
    void emitInputs(GenContext& context, ShaderStage& stage) const override;

    /// Emit the shader uniforms. Only the public material parameters are emitted,
    /// as globals initialized to their authored .mtlx values (wrapped in the
    /// `__MTLX_PARAMS_BEGIN__`/`__MTLX_PARAMS_END__` markers). The private uniforms
    /// (u_env* / u_refractionTwoSided / u_viewPosition / u_numActiveLightSources)
    /// and the light-data uniforms are provided by the raster host, so they are not
    /// emitted here (which would otherwise clash with the host's own declarations).
    void emitUniforms(GenContext& context, ShaderStage& stage) const override;
};

MATERIALX_NAMESPACE_END

#endif
