//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXGenGlsl/MtlxPathTracerHostShaderGenerator.h>

#include <emscripten/bind.h>

namespace ems = emscripten;
namespace mx = MaterialX;

namespace
{
    // Creator wrapper to avoid having to expose the TypeSystem class in JavaScript.
    mx::ShaderGeneratorPtr MtlxPathTracerHostShaderGenerator_create()
    {
        return mx::MtlxPathTracerHostShaderGenerator::create();
    }
}

EMSCRIPTEN_BINDINGS(MtlxPathTracerHostShaderGenerator)
{
    // generate(...) -> Shader and Shader.getSourceCode(stage) are inherited from
    // the already-bound ShaderGenerator / Shader classes; only the creation entry
    // point is added here (mirrors JsEsslHostShaderGenerator.cpp).
    ems::class_<mx::MtlxPathTracerHostShaderGenerator, ems::base<mx::HwShaderGenerator>>("MtlxPathTracerHostShaderGenerator")
        .class_function("create", &MtlxPathTracerHostShaderGenerator_create);
}
