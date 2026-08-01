//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXGenGlsl/EsslHostShaderGenerator.h>

#include <emscripten/bind.h>

namespace ems = emscripten;
namespace mx = MaterialX;

namespace
{
    // Creator wrapper to avoid having to expose the TypeSystem class in JavaScript.
    mx::ShaderGeneratorPtr EsslHostShaderGenerator_create()
    {
        return mx::EsslHostShaderGenerator::create();
    }
}

EMSCRIPTEN_BINDINGS(EsslHostShaderGenerator)
{
    // generate(...) -> Shader and Shader.getSourceCode(stage) are inherited from
    // the already-bound ShaderGenerator / Shader classes; only the creation entry
    // point is added here (mirrors JsPathTracerGlslShaderGenerator.cpp).
    ems::class_<mx::EsslHostShaderGenerator, ems::base<mx::HwShaderGenerator>>("EsslHostShaderGenerator")
        .class_function("create", &EsslHostShaderGenerator_create);
}
