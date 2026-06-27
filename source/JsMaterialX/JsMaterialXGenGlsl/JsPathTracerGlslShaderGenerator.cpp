//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXGenGlsl/PathTracerGlslShaderGenerator.h>

#include <emscripten/bind.h>

namespace ems = emscripten;
namespace mx = MaterialX;

namespace
{
    // Creator wrapper to avoid having to expose the TypeSystem class in JavaScript.
    mx::ShaderGeneratorPtr PathTracerGlslShaderGenerator_create()
    {
        return mx::PathTracerGlslShaderGenerator::create();
    }
}

EMSCRIPTEN_BINDINGS(PathTracerGlslShaderGenerator)
{
    // The generation entry point (generate(...) -> Shader, then Shader.getSourceCode(stage))
    // is inherited from the already-bound ShaderGenerator / Shader classes, so the
    // TypeScript pipeline orchestrates document parsing, library loading and source
    // extraction. Only the creation entry point is added here (contracts/js-binding-api.md).
    ems::class_<mx::PathTracerGlslShaderGenerator, ems::base<mx::HwShaderGenerator>>("PathTracerGlslShaderGenerator")
        .class_function("create", &PathTracerGlslShaderGenerator_create);
}
