//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXGenGlsl/PathTracerGlslShaderGenerator.h>

#include <MaterialXGenShader/Exception.h>

MATERIALX_NAMESPACE_BEGIN

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
    // Closure stage assembly (Eval/Sample emission) is added by the User Story 1
    // implementation tasks; this foundational skeleton intentionally inherits the
    // base generation flow.
}

void PathTracerGlslShaderGenerator::throwUnsupportedClosure(const string& nodeName, const string& reason) const
{
    throw ExceptionShaderGenError(
        "PathTracerGlslShaderGenerator: unsupported node/closure '" + nodeName + "': " + reason);
}

MATERIALX_NAMESPACE_END
