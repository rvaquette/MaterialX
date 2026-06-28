//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#ifndef MATERIALX_PATHTRACERGLSLSHADERGENERATOR_H
#define MATERIALX_PATHTRACERGLSLSHADERGENERATOR_H

/// @file
/// Path tracer GLSL shader generator.
///
/// Emits GLSL conforming to the Monte Carlo path tracer closure contract
/// (EvalMtlxPureClosure / SampleMtlxPureClosure) instead of the forward-shading
/// rasterization contract. See specs/001-mtlx-pathtracer-glslgen.

#include <MaterialXGenGlsl/EsslShaderGenerator.h>

MATERIALX_NAMESPACE_BEGIN

using PathTracerGlslShaderGeneratorPtr = shared_ptr<class PathTracerGlslShaderGenerator>;

/// @class PathTracerGlslShaderGenerator
/// A GLSL generator that produces Monte Carlo path tracer closure entry points
/// rather than forward-shading vertex/pixel stages with explicit light loops.
///
/// It derives from EsslShaderGenerator to reuse GLSL ES 3.00 (WebGL2) emission
/// (directives, precision, version "300 es") while reusing the upstream
/// "genglsl" node implementations (mx_*_bsdf.glsl, mx_*_edf.glsl, image, etc.).
///
/// This is the foundational skeleton: the closure stage assembly
/// (Eval/Sample emission, texture/sampler binding, normal-map/TBN handling) is
/// added by the User Story 1 implementation tasks. Unsupported nodes/closures
/// (e.g. volume, displacement in v1) must fail explicitly via
/// throwUnsupportedClosure().
class MX_GENGLSL_API PathTracerGlslShaderGenerator : public EsslShaderGenerator
{
  public:
    /// Constructor.
    PathTracerGlslShaderGenerator(TypeSystemPtr typeSystem);

    /// Creator function.
    /// If a TypeSystem is not provided it will be created internally.
    static ShaderGeneratorPtr create(TypeSystemPtr typeSystem = nullptr)
    {
        return std::make_shared<PathTracerGlslShaderGenerator>(typeSystem ? typeSystem : TypeSystem::create());
    }

    /// Reuse the upstream "genglsl" node implementations (BSDF/EDF/image/etc.)
    /// rather than introducing a new implementation target. The path tracer
    /// output dialect remains GLSL ES 3.00 via the inherited ESSL version.
    const string& getTarget() const override { return GlslShaderGenerator::TARGET; }

    /// Generate the closure shader. Forces a reduced shader interface so that
    /// constant MaterialX inputs (base_color, roughness, ...) are folded as GLSL
    /// literals instead of being published as unbound uniforms.
    ShaderPtr generate(const string& name, ElementPtr element, GenContext& context) const override;

  public:
    /// Unique identifier for this generator (path tracer closure GLSL).
    static const string TARGET;

  protected:
    /// Emit the path tracer closure stage in place of the forward-shading pixel
    /// stage. Instead of vertex/pixel rasterization with an explicit light loop,
    /// this emits the closure entry points (EvalMtlxPureClosure /
    /// SampleMtlxPureClosure) consumed by the Monte Carlo integrator, reusing the
    /// upstream "genglsl" node function definitions for the BSDF/EDF bricks.
    void emitPixelStage(const ShaderGraph& graph, GenContext& context, ShaderStage& stage) const override;

    /// Raise a named, explicit error for an unsupported node/closure.
    ///
    /// Used to guarantee "no silent failure": when a document references a
    /// node/closure without a path-tracer-compatible implementation (e.g.
    /// volume/displacement in v1), generation throws an ExceptionShaderGenError
    /// whose message names the offending node/closure and the reason. The
    /// JavaScript bindings surface this as a JS exception.
    [[noreturn]] void throwUnsupportedClosure(const string& nodeName, const string& reason) const;
};

MATERIALX_NAMESPACE_END

#endif
