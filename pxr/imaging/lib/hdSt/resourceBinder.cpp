//
// Copyright 2016 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#include "pxr/imaging/glf/glew.h"
#include "pxr/imaging/glf/contextCaps.h"

#include "pxr/imaging/hdSt/resourceBinder.h"
#include "pxr/imaging/hdSt/glConversions.h"
#include "pxr/imaging/hdSt/bufferArrayRangeGL.h"
#include "pxr/imaging/hdSt/bufferResourceGL.h"
#include "pxr/imaging/hdSt/resourceGL.h"
#include "pxr/imaging/hdSt/shaderCode.h"
#include "pxr/imaging/hdSt/drawItem.h"
#include "pxr/imaging/hd/bufferSpec.h"
#include "pxr/imaging/hd/enums.h"
#include "pxr/imaging/hd/tokens.h"

#include "pxr/base/tf/staticTokens.h"

#include <boost/functional/hash.hpp>

PXR_NAMESPACE_OPEN_SCOPE


TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    ((_double, "double"))
    ((_float, "float"))
    ((_int, "int"))
    (vec2)
    (vec3)
    (vec4)
    (dvec2)
    (dvec3)
    (dvec4)
    (ivec2)
    (ivec3)
    (ivec4)
    (constantPrimvars)
    (primitiveParam)
    (topologyVisibility)
);

namespace {
    struct BindingLocator {
        BindingLocator() :
            uniformLocation(0), uboLocation(0),
            ssboLocation(0), attribLocation(0),
            textureUnit(0) {}

        HdBinding GetBinding(HdBinding::Type type, TfToken const &debugName) {
            switch(type) {
            case HdBinding::UNIFORM:
                return HdBinding(HdBinding::UNIFORM, uniformLocation++);
                break;
            case HdBinding::UBO:
                return HdBinding(HdBinding::UBO, uboLocation++);
                break;
            case HdBinding::SSBO:
                return HdBinding(HdBinding::SSBO, ssboLocation++);
                break;
            case HdBinding::BINDLESS_SSBO_RANGE:
                return HdBinding(HdBinding::BINDLESS_SSBO_RANGE, uniformLocation++);
            case HdBinding::TBO:
                return HdBinding(HdBinding::TBO, uniformLocation++, textureUnit++);
                break;
            case HdBinding::BINDLESS_UNIFORM:
                return HdBinding(HdBinding::BINDLESS_UNIFORM, uniformLocation++);
                break;
            case HdBinding::VERTEX_ATTR:
                return HdBinding(HdBinding::VERTEX_ATTR, attribLocation++);
                break;
            case HdBinding::DRAW_INDEX:
                return HdBinding(HdBinding::DRAW_INDEX, attribLocation++);
                break;
            case HdBinding::DRAW_INDEX_INSTANCE:
                return HdBinding(HdBinding::DRAW_INDEX_INSTANCE, attribLocation++);
                break;
            default:
                TF_CODING_ERROR("Unknown binding type %d for %s",
                                type, debugName.GetText());
                return HdBinding();
                break;
            }
        }

        int uniformLocation;
        int uboLocation;
        int ssboLocation;
        int attribLocation;
        int textureUnit;
    };

    static inline GLboolean _ShouldBeNormalized(HdType type) {
        return type == HdTypeInt32_2_10_10_10_REV;
    }

    // GL has special handling for the "number of components" for
    // packed vectors.  Handle that here.
    static inline int _GetNumComponents(HdType type) {
        if (type == HdTypeInt32_2_10_10_10_REV) {
            return 4;
        } else {
            return HdGetComponentCount(type);
        }
    }
}

HdSt_ResourceBinder::HdSt_ResourceBinder()
    : _numReservedTextureUnits(0)
{
}

void
HdSt_ResourceBinder::ResolveBindings(HdStDrawItem const *drawItem,
                                   HdStShaderCodeSharedPtrVector const &shaders,
                                   HdSt_ResourceBinder::MetaData *metaDataOut,
                                   bool indirect,
                                   bool instanceDraw,
                                   HdBindingRequestVector const &customBindings)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    if (!TF_VERIFY(metaDataOut)) return;

    // GL context caps
    const bool ssboEnabled
        = GlfContextCaps::GetInstance().shaderStorageBufferEnabled;
    const bool bindlessUniformEnabled
        = GlfContextCaps::GetInstance().bindlessBufferEnabled;
    const bool bindlessTextureEnabled
        = GlfContextCaps::GetInstance().bindlessTextureEnabled;

    HdBinding::Type arrayBufferBindingType = HdBinding::TBO;  // 3.0
    if (bindlessUniformEnabled) {
        arrayBufferBindingType = HdBinding::BINDLESS_UNIFORM; // EXT
    } else if (ssboEnabled) {
        arrayBufferBindingType = HdBinding::SSBO;             // 4.3
    }

    HdBinding::Type structBufferBindingType = HdBinding::UBO;  // 3.1
    if (bindlessUniformEnabled) {
        structBufferBindingType = HdBinding::BINDLESS_UNIFORM; // EXT
    } else if (ssboEnabled) {
        structBufferBindingType = HdBinding::SSBO;             // 4.3
    }

    HdBinding::Type drawingCoordBindingType = HdBinding::UNIFORM;
    if (indirect) {
        if (instanceDraw) {
            drawingCoordBindingType = HdBinding::DRAW_INDEX_INSTANCE;
        } else {
            drawingCoordBindingType = HdBinding::DRAW_INDEX;
        }
    }

    bool useBindlessForTexture = bindlessTextureEnabled;

    // binding assignments
    BindingLocator locator;
    locator.textureUnit = 5; // XXX: skip glop's texture --- need fix.

    int bindlessTextureLocation = 0;
    // Note that these locations are used for hash keys only and
    // are never used for actual resource binding.
    int shaderFallbackLocation = 0;
    int shaderRedirectLocation = 0;

    // clear all
    _bindingMap.clear();

    // constant primvar (per-object)
    HdBinding constantPrimvarBinding =
                locator.GetBinding(structBufferBindingType,
                                   _tokens->constantPrimvars);

    if (HdBufferArrayRangeSharedPtr constantBar_ =
        drawItem->GetConstantPrimvarRange()) {

        HdStBufferArrayRangeGLSharedPtr constantBar =
            boost::static_pointer_cast<HdStBufferArrayRangeGL>(constantBar_);

        MetaData::StructBlock sblock(_tokens->constantPrimvars);
        TF_FOR_ALL (it, constantBar->GetResources()) {
            HdTupleType valueType = it->second->GetTupleType();
            TfToken glType = HdStGLConversions::GetGLSLTypename(valueType.type);
            TfToken glName =  HdStGLConversions::GetGLSLIdentifier(it->first);
            sblock.entries.emplace_back(
                /*name=*/glName,
                /*type=*/glType,
                /*offset=*/it->second->GetOffset(),
                /*arraySize=*/valueType.count);
        }
        // sort by offset
        // XXX: not robust enough, should consider padding and layouting rules
        // to match with the logic in HdInterleavedMemoryManager if we
        // want to use a layouting policy other than default padding.
        std::sort(sblock.entries.begin(), sblock.entries.end());

        metaDataOut->constantData.insert(
            std::make_pair(constantPrimvarBinding, sblock));
    }

     // constant primvars are interleaved into single struct.
    _bindingMap[_tokens->constantPrimvars] = constantPrimvarBinding;

    // instance primvar (per-instance)
    int instancerNumLevels = drawItem->GetInstancePrimvarNumLevels();
    metaDataOut->instancerNumLevels = instancerNumLevels;
    for (int i = 0; i < instancerNumLevels; ++i) {
        if (HdBufferArrayRangeSharedPtr instanceBar_ =
            drawItem->GetInstancePrimvarRange(i)) {

            HdStBufferArrayRangeGLSharedPtr instanceBar =
                boost::static_pointer_cast<HdStBufferArrayRangeGL>(instanceBar_);

            TF_FOR_ALL (it, instanceBar->GetResources()) {
                TfToken const& name = it->first;
                TfToken glName =  HdStGLConversions::GetGLSLIdentifier(name);
                // non-interleaved, always create new binding.
                HdBinding instancePrimvarBinding =
                    locator.GetBinding(arrayBufferBindingType, name);
                _bindingMap[NameAndLevel(name, i)] = instancePrimvarBinding;

                HdTupleType valueType = it->second->GetTupleType();
                TfToken glType =
                    HdStGLConversions::GetGLSLTypename(valueType.type);
                metaDataOut->instanceData[instancePrimvarBinding] =
                    MetaData::NestedPrimvar(
                        /*name=*/glName,
                        /*type=*/glType,
                        /*level=*/i);
            }
        }
    }

    // vertex primvar (per-vertex)
    // always assigned to VertexAttribute.
    if (HdBufferArrayRangeSharedPtr vertexBar_ =
        drawItem->GetVertexPrimvarRange()) {

        HdStBufferArrayRangeGLSharedPtr vertexBar =
            boost::static_pointer_cast<HdStBufferArrayRangeGL>(vertexBar_);

        TF_FOR_ALL (it, vertexBar->GetResources()) {
            TfToken const& name = it->first;
            TfToken glName =  HdStGLConversions::GetGLSLIdentifier(name);
            HdBinding vertexPrimvarBinding =
                locator.GetBinding(HdBinding::VERTEX_ATTR, name);
            _bindingMap[name] = vertexPrimvarBinding;

            HdTupleType valueType = it->second->GetTupleType();
            // Special case: VBOs have intrinsic support for packed types,
            // so expand them out to their target type for the shader binding.
            if (valueType.type == HdTypeInt32_2_10_10_10_REV) {
                valueType.type = HdTypeFloatVec4;
            }
            TfToken glType = HdStGLConversions::GetGLSLTypename(valueType.type);
            metaDataOut->vertexData[vertexPrimvarBinding] =
                MetaData::Primvar(/*name=*/glName,
                                  /*type=*/glType);
        }
    }

    // index buffer
    if (HdBufferArrayRangeSharedPtr topologyBar_ =
        drawItem->GetTopologyRange()) {

        HdStBufferArrayRangeGLSharedPtr topologyBar =
            boost::static_pointer_cast<HdStBufferArrayRangeGL>(topologyBar_);

        TF_FOR_ALL (it, topologyBar->GetResources()) {
            // Don't need to sanitize the name, since topology resources are
            // created internally.
            TfToken const& name = it->first;
            HdStBufferResourceGLSharedPtr const& resource = it->second;

            if (name == HdTokens->indices) {
                // IBO. no need for codegen
                _bindingMap[name] = HdBinding(HdBinding::INDEX_ATTR, 0);
            } else {
                // We expect the following additional topology based info:
                // - primitive parameter (for all tris, quads and patches) OR
                // - edge indices (for all tris, quads and patches)
                HdBinding binding =
                    locator.GetBinding(arrayBufferBindingType, name);
                _bindingMap[name] = binding;

                HdTupleType valueType = resource->GetTupleType();
                TfToken glType =
                    HdStGLConversions::GetGLSLTypename(valueType.type);

                auto bindingDecl = MetaData::BindingDeclaration(
                                     /*name=*/name,
                                     /*type=*/glType,
                                     /*binding=*/binding);

                if (name == HdTokens->primitiveParam) {
                    metaDataOut->primitiveParamBinding = bindingDecl;
                } else if (name == HdTokens->edgeIndices) {
                    metaDataOut->edgeIndexBinding = bindingDecl;
                } else {
                    TF_WARN("Unexpected topological resource '%s'\n",
                    name.GetText());
                }
            }
        }
    }

     // topology visibility
    HdBinding topologyVisibilityBinding =
                locator.GetBinding(structBufferBindingType,
                                   /*debugName*/_tokens->topologyVisibility);

    if (HdBufferArrayRangeSharedPtr topVisBar_ =
        drawItem->GetTopologyVisibilityRange()) {

        HdStBufferArrayRangeGLSharedPtr topVisBar =
            boost::static_pointer_cast<HdStBufferArrayRangeGL>(topVisBar_);

        MetaData::StructBlock sblock(_tokens->topologyVisibility);
        TF_FOR_ALL (it, topVisBar->GetResources()) {
            HdTupleType valueType = it->second->GetTupleType();
            TfToken glType = HdStGLConversions::GetGLSLTypename(valueType.type);
            sblock.entries.emplace_back(
                /*name=*/it->first,
                /*type=*/glType,
                /*offset=*/it->second->GetOffset(),
                /*arraySize=*/valueType.count);
        }
        
        std::sort(sblock.entries.begin(), sblock.entries.end());

        metaDataOut->topologyVisibilityData.insert(
            std::make_pair(topologyVisibilityBinding, sblock));
    }

     // topology visibility is interleaved into single struct.
    _bindingMap[_tokens->topologyVisibility] = topologyVisibilityBinding;

    // element primvar (per-face, per-line)
    if (HdBufferArrayRangeSharedPtr elementBar_ =
        drawItem->GetElementPrimvarRange()) {

        HdStBufferArrayRangeGLSharedPtr elementBar =
            boost::static_pointer_cast<HdStBufferArrayRangeGL>(elementBar_);

        TF_FOR_ALL (it, elementBar->GetResources()) {
            TfToken const& name = it->first;
            TfToken glName =  HdStGLConversions::GetGLSLIdentifier(name);
            HdBinding elementPrimvarBinding =
                locator.GetBinding(arrayBufferBindingType, name);
            _bindingMap[name] = elementPrimvarBinding;
            HdTupleType valueType = it->second->GetTupleType();
                TfToken glType =
                    HdStGLConversions::GetGLSLTypename(valueType.type);
            metaDataOut->elementData[elementPrimvarBinding] =
                MetaData::Primvar(/*name=*/glName,
                                  /*type=*/glType);
        }
    }

    // facevarying primvar (per-face-vertex)
    if (HdBufferArrayRangeSharedPtr fvarBar_ =
        drawItem->GetFaceVaryingPrimvarRange()) {

        HdStBufferArrayRangeGLSharedPtr fvarBar =
            boost::static_pointer_cast<HdStBufferArrayRangeGL>(fvarBar_);

        TF_FOR_ALL (it, fvarBar->GetResources()) {
            TfToken const& name = it->first;
            TfToken glName =  HdStGLConversions::GetGLSLIdentifier(name);
            HdBinding fvarPrimvarBinding =
                locator.GetBinding(arrayBufferBindingType, name);
            _bindingMap[name] = fvarPrimvarBinding;
            HdTupleType valueType = it->second->GetTupleType();
                TfToken glType =
                    HdStGLConversions::GetGLSLTypename(valueType.type);
            metaDataOut->fvarData[fvarPrimvarBinding] =
                MetaData::Primvar(/*name=*/glName,
                                  /*type=*/glType);
        }
    }

    // draw parameter
    // assigned to draw index (vertex attributeI w/divisor) (indiect)
    // assigned to uniform          (immediate)
    //
    // note that instanceDraw may be true even for non-instance drawing,
    // because there's only instanced version of glMultiDrawElementsIndirect.
    HdBinding drawingCoord0Binding = locator.GetBinding(
        drawingCoordBindingType, HdTokens->drawingCoord0);
    _bindingMap[HdTokens->drawingCoord0] = drawingCoord0Binding;
    metaDataOut->drawingCoord0Binding =
        MetaData::BindingDeclaration(/*name=*/HdTokens->drawingCoord0,
                                     /*type=*/_tokens->ivec4,
                                     /*binding=*/drawingCoord0Binding);

    HdBinding drawingCoord1Binding = locator.GetBinding(
        drawingCoordBindingType, HdTokens->drawingCoord1);
    _bindingMap[HdTokens->drawingCoord1] = drawingCoord1Binding;
    metaDataOut->drawingCoord1Binding =
        MetaData::BindingDeclaration(/*name=*/HdTokens->drawingCoord1,
                                     /*type=*/_tokens->ivec4,
                                     /*binding=*/drawingCoord1Binding);

    HdBinding drawingCoord2Binding = locator.GetBinding(
        drawingCoordBindingType, HdTokens->drawingCoord2);
    _bindingMap[HdTokens->drawingCoord2] = drawingCoord2Binding;
    metaDataOut->drawingCoord2Binding =
        MetaData::BindingDeclaration(/*name=*/HdTokens->drawingCoord2,
                                     /*type=*/_tokens->_int,
                                     /*binding=*/drawingCoord2Binding);

    if (instancerNumLevels > 0) {
        HdBinding drawingCoordIBinding = indirect
            ? HdBinding(HdBinding::DRAW_INDEX_INSTANCE_ARRAY, locator.attribLocation)
            : HdBinding(HdBinding::UNIFORM_ARRAY, locator.uniformLocation);
        if (indirect) {
            // each vertex attribute takes 1 location
            locator.attribLocation += instancerNumLevels;
        } else {
            // int[N] may consume more than 1 location
            locator.uniformLocation += instancerNumLevels;
        }
        _bindingMap[HdTokens->drawingCoordI] = drawingCoordIBinding;
        metaDataOut->drawingCoordIBinding =
            MetaData::BindingDeclaration(/*name=*/HdTokens->drawingCoordI,
                                         /*type=*/_tokens->_int,
                                         /*binding=*/drawingCoordIBinding);
    }

    // instance index indirection buffer
    if (HdBufferArrayRangeSharedPtr instanceIndexBar_ =
        drawItem->GetInstanceIndexRange()) {

        HdStBufferArrayRangeGLSharedPtr instanceIndexBar =
            boost::static_pointer_cast<HdStBufferArrayRangeGL>(instanceIndexBar_);

        HdStBufferResourceGLSharedPtr instanceIndices
            = instanceIndexBar->GetResource(HdTokens->instanceIndices);
        HdStBufferResourceGLSharedPtr culledInstanceIndices
            = instanceIndexBar->GetResource(HdTokens->culledInstanceIndices);

        if (instanceIndices) {
            HdBinding instanceIndexArrayBinding =
                locator.GetBinding(arrayBufferBindingType, HdTokens->instanceIndices);
            _bindingMap[HdTokens->instanceIndices] = instanceIndexArrayBinding;
            HdTupleType valueType = instanceIndices->GetTupleType();
                TfToken glType =
                    HdStGLConversions::GetGLSLTypename(valueType.type);
            metaDataOut->instanceIndexArrayBinding =
                MetaData::BindingDeclaration(
                    /*name=*/HdTokens->instanceIndices,
                    /*type=*/glType,
                    /*binding=*/instanceIndexArrayBinding);
        }
        if (culledInstanceIndices) {
            HdBinding culledInstanceIndexArrayBinding =
                locator.GetBinding(arrayBufferBindingType, HdTokens->culledInstanceIndices);
            _bindingMap[HdTokens->culledInstanceIndices] =
                culledInstanceIndexArrayBinding;
            HdTupleType valueType = instanceIndices->GetTupleType();
                TfToken glType =
                    HdStGLConversions::GetGLSLTypename(valueType.type);
            metaDataOut->culledInstanceIndexArrayBinding =
                MetaData::BindingDeclaration(
                    /*name=*/HdTokens->culledInstanceIndices,
                    /*type=*/glType,
                    /*binding=*/culledInstanceIndexArrayBinding);
        }
    }

    // indirect dispatch
    if (indirect) {
        HdBinding dispatchBinding(HdBinding::DISPATCH, /*location=(not used)*/0);
        _bindingMap[HdTokens->drawDispatch] = dispatchBinding;
    }

    // shader parameter bindings

    TF_FOR_ALL(shader, shaders) {

        // uniform block
        HdBufferArrayRangeSharedPtr const &shaderBar_ = (*shader)->GetShaderData();
        HdStBufferArrayRangeGLSharedPtr shaderBar =
            boost::static_pointer_cast<HdStBufferArrayRangeGL> (shaderBar_);
        if (shaderBar) {
            HdBinding shaderParamBinding =
                locator.GetBinding(structBufferBindingType, HdTokens->materialParams);

            // for fallback values and bindless textures
            // XXX: name of sblock must be unique for each shaders.
            MetaData::StructBlock sblock(HdTokens->materialParams);
            TF_FOR_ALL(it, shaderBar->GetResources()) {
                TfToken const& name = it->first;
                TfToken glName =  HdStGLConversions::GetGLSLIdentifier(name);
                HdTupleType valueType = it->second->GetTupleType();
                TfToken glType =
                    HdStGLConversions::GetGLSLTypename(valueType.type);
                sblock.entries.emplace_back(
                    /*name=*/glName,
                    /*type=*/glType,
                    /*offset=*/it->second->GetOffset(),
                    /*arraySize=*/valueType.count);
            }
            // sort by offset
            std::sort(sblock.entries.begin(), sblock.entries.end());
            metaDataOut->shaderData.insert(
                std::make_pair(shaderParamBinding, sblock));

            //XXX:hack  we want to generalize materialParams to other shaders.
            if ((*shader) == drawItem->GetMaterialShader()) {
                // shader parameters are interleaved into single struct.
                _bindingMap[HdTokens->materialParams] = shaderParamBinding;
            }
        }

        HdMaterialParamVector params = (*shader)->GetParams();
        // for primvar and texture accessors
        TF_FOR_ALL(it, params) {
            // renderpass texture should be bindfull (for now)
            bool bindless = useBindlessForTexture && ((*shader) == drawItem->GetMaterialShader());
            HdTupleType valueType = it->GetTupleType();
            TfToken glType =
                HdStGLConversions::GetGLSLTypename(valueType.type);
            TfToken const& name = it->GetName();
            TfToken glName =  HdStGLConversions::GetGLSLIdentifier(name);

            if (it->IsFallback()) {
                metaDataOut->shaderParameterBinding[HdBinding(HdBinding::FALLBACK, shaderFallbackLocation++)]
                    = MetaData::ShaderParameterAccessor(glName,
                                                        /*type=*/glType);
            } else if (it->IsTexture()) {
                if (it->GetTextureType() == HdTextureType::Ptex) {
                    // ptex texture
                    HdBinding texelBinding = bindless
                        ? HdBinding(HdBinding::BINDLESS_TEXTURE_PTEX_TEXEL, bindlessTextureLocation++)
                        : HdBinding(HdBinding::TEXTURE_PTEX_TEXEL, locator.uniformLocation++);

                    metaDataOut->shaderParameterBinding[texelBinding] =
                        MetaData::ShaderParameterAccessor(
                            /*name=*/glName,
                            /*type=*/glType);
                    _bindingMap[name] = texelBinding; // used for non-bindless

                    HdBinding layoutBinding = bindless
                        ? HdBinding(HdBinding::BINDLESS_TEXTURE_PTEX_LAYOUT, bindlessTextureLocation++)
                        : HdBinding(HdBinding::TEXTURE_PTEX_LAYOUT, locator.uniformLocation++);

                    TfToken glLayoutName = TfToken(std::string(glName.GetText()) + "_layout");
                    metaDataOut->shaderParameterBinding[layoutBinding] =
                        MetaData::ShaderParameterAccessor(
                            /*name=*/glLayoutName,
                            /*type=*/TfToken("isamplerBuffer"));

                    // XXX: same name ?
                    TfToken layoutName = TfToken(std::string(name.GetText()) + "_layout");
                    _bindingMap[layoutName] = layoutBinding; // used for non-bindless
                } else if (it->GetTextureType() == HdTextureType::Udim) {
                    // Texture Array for UDIM
                    HdBinding textureBinding =
                        bindless
                        ? HdBinding(HdBinding::BINDLESS_TEXTURE_UDIM_ARRAY,
                            bindlessTextureLocation++)
                        : HdBinding(HdBinding::TEXTURE_UDIM_ARRAY,
                            locator.uniformLocation++);
                    metaDataOut->shaderParameterBinding[textureBinding] =
                        MetaData::ShaderParameterAccessor(
                            /*name=*/it->GetName(),
                            /*type=*/glType,
                            /*inPrimvars=*/it->GetSamplerCoordinates());
                    // used for non-bindless
                    _bindingMap[it->GetName()] = textureBinding;

                    // Layout for UDIM
                    TfToken layoutName =
                        TfToken(std::string(it->GetName().GetText())
                        + "_layout");
                    HdBinding layoutBinding =
                        bindless
                        ? HdBinding(HdBinding::BINDLESS_TEXTURE_UDIM_LAYOUT,
                            bindlessTextureLocation++)
                        : HdBinding(HdBinding::TEXTURE_UDIM_LAYOUT,
                            locator.uniformLocation++);

                    metaDataOut->shaderParameterBinding[layoutBinding] =
                        MetaData::ShaderParameterAccessor(
                            /*name=*/layoutName,
                            /*type=*/HdStGLConversions::GetGLSLTypename(
                                HdType::HdTypeFloat));

                    // used for non-bindless
                    _bindingMap[layoutName] = layoutBinding;
                } else if (it->GetTextureType() == HdTextureType::Uv) {
                    // 2d texture
                    HdBinding textureBinding = bindless
                        ? HdBinding(HdBinding::BINDLESS_TEXTURE_2D, bindlessTextureLocation++)
                        : HdBinding(HdBinding::TEXTURE_2D, locator.uniformLocation++);

                    metaDataOut->shaderParameterBinding[textureBinding] =
                        MetaData::ShaderParameterAccessor(
                            /*name=*/glName,
                            /*type=*/glType,
                            /*inPrimvars=*/it->GetSamplerCoordinates());
                    _bindingMap[name] = textureBinding; // used for non-bindless
                }
            } else if (it->IsPrimvar()) {
                TfTokenVector const& samplePrimvars
                    = it->GetSamplerCoordinates();
                TfTokenVector glNames;
                glNames.reserve(samplePrimvars.size());
                for (auto const& pv : samplePrimvars) {
                    glNames.push_back(HdStGLConversions::GetGLSLIdentifier(pv));
                }
                
                metaDataOut->shaderParameterBinding[HdBinding(HdBinding::PRIMVAR_REDIRECT, shaderRedirectLocation++)]
                    = MetaData::ShaderParameterAccessor(
                    /*name=*/glName,
                    /*type=*/glType,
                    /*inPrimvars=*/glNames);
            } else {
                TF_CODING_ERROR("Can't resolve %s", it->GetName().GetText());
            }
        }
    }

    // Add custom bindings.
    // Don't need to sanitize the name used, since these are internally
    // generated.
    TF_FOR_ALL (it, customBindings) {
        if (it->IsInterleavedBufferArray()) {
            // Interleaved resource, only need a single binding point
            HdBinding binding = locator.GetBinding(it->GetBindingType(),
                                                   it->GetName());
            MetaData::StructBlock sblock(it->GetName());

            HdBufferArrayRangeSharedPtr bar_ = it->GetBar();
            HdStBufferArrayRangeGLSharedPtr bar =
                boost::static_pointer_cast<HdStBufferArrayRangeGL> (bar_);

            for (auto const& nameRes : bar->GetResources()) {
                HdTupleType valueType = nameRes.second->GetTupleType();
                TfToken glType =
                    HdStGLConversions::GetGLSLTypename(valueType.type);
                sblock.entries.emplace_back(nameRes.first,
                                            glType,
                                             nameRes.second->GetOffset(),
                                            valueType.count);
            }
            metaDataOut->customInterleavedBindings.insert(
                std::make_pair(binding, sblock));
            _bindingMap[it->GetName()] = binding;
        } else {
            // Non interleaved resource
            typedef MetaData::BindingDeclaration BindingDeclaration;
            if (it->IsBufferArray()) {
                // The BAR was provided, so we will record the name, dataType,
                // binding type and binding location.

                HdBufferArrayRangeSharedPtr bar_ = it->GetBar();
                HdStBufferArrayRangeGLSharedPtr bar =
                    boost::static_pointer_cast<HdStBufferArrayRangeGL> (bar_);

                for (auto const& nameRes : bar->GetResources()) {
                    HdBinding binding = locator.GetBinding(it->GetBindingType(), nameRes.first);
                    BindingDeclaration b(nameRes.first,
                        HdStGLConversions::GetGLSLTypename(
                            nameRes.second->GetTupleType().type),
                        binding);
                    metaDataOut->customBindings.push_back(b);
                    _bindingMap[nameRes.first] = binding;
                }
            } else {
                HdBinding binding = locator.GetBinding(it->GetBindingType(), it->GetName());
                BindingDeclaration b(it->GetName(),
                                     HdStGLConversions::GetGLSLTypename(
                                                    it->GetDataType()),
                                     binding);

                // note that GetDataType() may return HdTypeInvalid,
                // in case it's a typeless binding. CodeGen generates
                // declarations and accessors only for BindingDeclaration
                // with a valid type.
                metaDataOut->customBindings.push_back(b);
                _bindingMap[it->GetName()] = binding;
            }
        }
    }
    _numReservedTextureUnits = locator.textureUnit;
}

void
HdSt_ResourceBinder::ResolveComputeBindings(
                    HdBufferSpecVector const &readWriteBufferSpecs,
                    HdBufferSpecVector const &readOnlyBufferSpecs,
                    HdStShaderCodeSharedPtrVector const &shaders,
                    MetaData *metaDataOut)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    if (!TF_VERIFY(metaDataOut)) {
        return;
    }

    // GL context caps
    HdBinding::Type bindingType =
        (GlfContextCaps::GetInstance().bindlessBufferEnabled
         ? HdBinding::BINDLESS_SSBO_RANGE : HdBinding::SSBO);

    // binding assignments
    BindingLocator locator;

    // clear all
    _bindingMap.clear();
    
    // read-write per prim data
    for (HdBufferSpec const& spec: readWriteBufferSpecs) {
        HdBinding binding = locator.GetBinding(bindingType, spec.name);
        _bindingMap[spec.name] = binding;
        metaDataOut->computeReadWriteData[binding] =
            MetaData::Primvar(spec.name,
                              HdStGLConversions::GetGLSLTypename(
                                             spec.tupleType.type));
    }
    
    // read-only per prim data
    for (HdBufferSpec const& spec: readOnlyBufferSpecs) {
        HdBinding binding = locator.GetBinding(bindingType, spec.name);
        _bindingMap[spec.name] = binding;
        metaDataOut->computeReadOnlyData[binding] =
            MetaData::Primvar(spec.name,
                              HdStGLConversions::GetGLSLTypename(
                                             spec.tupleType.type));
    }
}

void
HdSt_ResourceBinder::BindBuffer(TfToken const &name,
                              HdStBufferResourceGLSharedPtr const &buffer) const
{
    BindBuffer(name, buffer, buffer->GetOffset(), /*level=*/-1);
}

void
HdSt_ResourceBinder::BindBuffer(TfToken const &name,
                              HdStBufferResourceGLSharedPtr const &buffer,
                              int offset,
                              int level) const
{
    HD_TRACE_FUNCTION();

    // it is possible that the buffer has not been initialized when
    // the instanceIndex is empty (e.g. FX points. see bug 120354)
    if (buffer->GetId() == 0) return;

    HdBinding binding = GetBinding(name, level);
    HdBinding::Type type = binding.GetType();
    int loc              = binding.GetLocation();
    int textureUnit      = binding.GetTextureUnit();

    HdTupleType tupleType = buffer->GetTupleType();

    void const* offsetPtr =
        reinterpret_cast<const void*>(
            static_cast<intptr_t>(offset));
    switch(type) {
    case HdBinding::VERTEX_ATTR:
        glBindBuffer(GL_ARRAY_BUFFER, buffer->GetId());
        glVertexAttribPointer(loc,
                  _GetNumComponents(tupleType.type),
                  HdStGLConversions::GetGLAttribType(tupleType.type),
                  _ShouldBeNormalized(tupleType.type),
                              buffer->GetStride(),
                              offsetPtr);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        glEnableVertexAttribArray(loc);
        break;
    case HdBinding::DRAW_INDEX:
        glBindBuffer(GL_ARRAY_BUFFER, buffer->GetId());
        glVertexAttribIPointer(loc,
                               HdGetComponentCount(tupleType.type),
                               GL_INT,
                               buffer->GetStride(),
                               offsetPtr);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glEnableVertexAttribArray(loc);
        break;
    case HdBinding::DRAW_INDEX_INSTANCE:
        glBindBuffer(GL_ARRAY_BUFFER, buffer->GetId());
        glVertexAttribIPointer(loc,
                               HdGetComponentCount(tupleType.type),
                               GL_INT,
                               buffer->GetStride(),
                               offsetPtr);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        // set the divisor to uint-max so that the same base value is used
        // for all instances.
        glVertexAttribDivisor(loc,
                              std::numeric_limits<GLint>::max());
        glEnableVertexAttribArray(loc);
        break;
    case HdBinding::DRAW_INDEX_INSTANCE_ARRAY:
        glBindBuffer(GL_ARRAY_BUFFER, buffer->GetId());
        // instancerNumLevels is represented by the tuple size.
        // We unroll this to an array of int[1] attributes.
        for (size_t i = 0; i < buffer->GetTupleType().count; ++i) {
            offsetPtr = reinterpret_cast<const void*>(offset + i*sizeof(int));
            glVertexAttribIPointer(loc, 1, GL_INT, buffer->GetStride(),
                                   offsetPtr);
            // set the divisor to uint-max so that the same base value is used
            // for all instances.
            glVertexAttribDivisor(loc, std::numeric_limits<GLint>::max());
            glEnableVertexAttribArray(loc);
            ++loc;
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        break;
    case HdBinding::INDEX_ATTR:
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer->GetId());
        break;
    case HdBinding::BINDLESS_UNIFORM:
        // at least in nvidia driver 346.59, this query call doesn't show
        // any pipeline stall.
        if (!glIsNamedBufferResidentNV(buffer->GetId())) {
            glMakeNamedBufferResidentNV(buffer->GetId(), GL_READ_WRITE);
        }
        glUniformui64NV(loc, buffer->GetGPUAddress());
        break;
    case HdBinding::SSBO:
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, loc,
                         buffer->GetId());
        break;
    case HdBinding::BINDLESS_SSBO_RANGE:
        // at least in nvidia driver 346.59, this query call doesn't show
        // any pipeline stall.
        if (!glIsNamedBufferResidentNV(buffer->GetId())) {
            glMakeNamedBufferResidentNV(buffer->GetId(), GL_READ_WRITE);
        }
        glUniformui64NV(loc, buffer->GetGPUAddress()+offset);
        break;
    case HdBinding::DISPATCH:
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, buffer->GetId());
        break;
    case HdBinding::UBO:
    case HdBinding::UNIFORM:
        glBindBufferRange(GL_UNIFORM_BUFFER, loc,
                          buffer->GetId(),
                          offset,
                          buffer->GetStride());
        break;
    case HdBinding::TBO:
        if (loc != HdBinding::NOT_EXIST) {
            glUniform1i(loc, textureUnit);
            glActiveTexture(GL_TEXTURE0 + textureUnit);
            glBindSampler(textureUnit, 0);
            glBindTexture(GL_TEXTURE_BUFFER, buffer->GetTextureBuffer());
        }
        break;
    case HdBinding::TEXTURE_2D:
        // nothing
        break;
    default:
        TF_CODING_ERROR("binding type %d not found for %s",
                        type, name.GetText());
        break;
    }
}

void
HdSt_ResourceBinder::UnbindBuffer(TfToken const &name,
                                HdStBufferResourceGLSharedPtr const &buffer,
                                int level) const
{
    HD_TRACE_FUNCTION();

    // it is possible that the buffer has not been initialized when
    // the instanceIndex is empty (e.g. FX points)
    if (buffer->GetId() == 0) return;

    HdBinding binding = GetBinding(name, level);
    HdBinding::Type type = binding.GetType();
    int loc = binding.GetLocation();

    switch(type) {
    case HdBinding::VERTEX_ATTR:
        glDisableVertexAttribArray(loc);
        break;
    case HdBinding::DRAW_INDEX:
        glDisableVertexAttribArray(loc);
        break;
    case HdBinding::DRAW_INDEX_INSTANCE:
        glDisableVertexAttribArray(loc);
        glVertexAttribDivisor(loc, 0);
        break;
    case HdBinding::DRAW_INDEX_INSTANCE_ARRAY:
        // instancerNumLevels is represented by the tuple size.
        for (size_t i = 0; i < buffer->GetTupleType().count; ++i) {
            glDisableVertexAttribArray(loc);
            glVertexAttribDivisor(loc, 0);
            ++loc;
        }
        break;
    case HdBinding::INDEX_ATTR:
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        break;
    case HdBinding::BINDLESS_UNIFORM:
        if (glIsNamedBufferResidentNV(buffer->GetId())) {
            glMakeNamedBufferNonResidentNV(buffer->GetId());
        }
        break;
    case HdBinding::SSBO:
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, loc, 0);
        break;
    case HdBinding::BINDLESS_SSBO_RANGE:
        if (glIsNamedBufferResidentNV(buffer->GetId())) {
            glMakeNamedBufferNonResidentNV(buffer->GetId());
        }
        break;
    case HdBinding::DISPATCH:
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        break;
    case HdBinding::UBO:
    case HdBinding::UNIFORM:
        glBindBufferBase(GL_UNIFORM_BUFFER, loc, 0);
        break;
    case HdBinding::TBO:
        if (loc != HdBinding::NOT_EXIST) {
            glActiveTexture(GL_TEXTURE0 + binding.GetTextureUnit());
            glBindTexture(GL_TEXTURE_BUFFER, 0);
        }
        break;
    case HdBinding::TEXTURE_2D:
        // nothing
        break;
    default:
        TF_CODING_ERROR("binding type %d not found for %s",
                        type, name.GetText());
        break;
    }
}

void
HdSt_ResourceBinder::BindConstantBuffer(
    HdStBufferArrayRangeGLSharedPtr const &constantBar) const
{
    if (!constantBar) return;

    // constant buffer is interleaved. we just need to bind a buffer.
    BindBuffer(_tokens->constantPrimvars, constantBar->GetResource());
}

void
HdSt_ResourceBinder::UnbindConstantBuffer(
    HdStBufferArrayRangeGLSharedPtr const &constantBar) const
{
    if (!constantBar) return;

    UnbindBuffer(_tokens->constantPrimvars, constantBar->GetResource());
}

void
HdSt_ResourceBinder::BindInterleavedBuffer(
    HdStBufferArrayRangeGLSharedPtr const &interleavedBar,
    TfToken const &name) const
{
    if (!interleavedBar) return;

    BindBuffer(name, interleavedBar->GetResource());
}

void
HdSt_ResourceBinder::UnbindInterleavedBuffer(
    HdStBufferArrayRangeGLSharedPtr const &interleavedBar,
    TfToken const &name) const
{
    if (!interleavedBar) return;

    UnbindBuffer(name, interleavedBar->GetResource());
}

void
HdSt_ResourceBinder::BindInstanceBufferArray(
    HdStBufferArrayRangeGLSharedPtr const &bar, int level) const
{
    if (!bar) return;

    TF_FOR_ALL(it, bar->GetResources()) {
        BindBuffer(it->first, it->second, it->second->GetOffset(), level);
    }
}

void
HdSt_ResourceBinder::UnbindInstanceBufferArray(
    HdStBufferArrayRangeGLSharedPtr const &bar, int level) const
{
    if (!bar) return;

    TF_FOR_ALL(it, bar->GetResources()) {
        UnbindBuffer(it->first, it->second, level);
    }
}

void
HdSt_ResourceBinder::BindShaderResources(HdStShaderCode const *shader) const
{
    // bind fallback values and sampler uniforms (unit#? or bindless address)

    // this is bound in batches.
    //BindBufferArray(shader->GetShaderData());

    // bind textures
    HdStShaderCode::TextureDescriptorVector textures = shader->GetTextures();
    TF_FOR_ALL(it, textures) {
        HdBinding binding = GetBinding(it->name);
        HdBinding::Type type = binding.GetType();

        if (type == HdBinding::TEXTURE_2D) {
        } else if (type == HdBinding::BINDLESS_TEXTURE_2D
                || type == HdBinding::BINDLESS_TEXTURE_PTEX_TEXEL
                || type == HdBinding::BINDLESS_TEXTURE_PTEX_LAYOUT) {
            // nothing? or make it resident?? but it only binds the first one.
            // XXX: it looks like this function should take all textures in the batch.

//            if (!glIsTextureHandleResidentARB(it->handle)) {
//                glMakeTextureHandleResidentARB(it->handle);
//            }
        }
    }
}

void
HdSt_ResourceBinder::UnbindShaderResources(HdStShaderCode const *shader) const
{
//    UnbindBufferArray(shader->GetShaderData());

    HdStShaderCode::TextureDescriptorVector textures = shader->GetTextures();
    TF_FOR_ALL(it, textures) {
        HdBinding binding = GetBinding(it->name);
        HdBinding::Type type = binding.GetType();

        if (type == HdBinding::TEXTURE_2D) {
        } else if (type == HdBinding::BINDLESS_TEXTURE_2D
                || type == HdBinding::BINDLESS_TEXTURE_PTEX_TEXEL
                   || type == HdBinding::BINDLESS_TEXTURE_PTEX_LAYOUT) {
//            if (glIsTextureHandleResidentARB(it->handle)) {
//                glMakeTextureHandleNonResidentARB(it->handle);
//            }
        }
        // XXX: unbind
    }
}

void
HdSt_ResourceBinder::BindBufferArray(HdStBufferArrayRangeGLSharedPtr const &bar) const
{
    if (!bar) return;

    TF_FOR_ALL(it, bar->GetResources()) {
        BindBuffer(it->first, it->second);
    }
}

void
HdSt_ResourceBinder::Bind(HdBindingRequest const& req) const
{
    if (req.IsTypeless()) {
        return;
    } else if (req.IsResource()) {
        HdBufferResourceSharedPtr res_ = req.GetResource();
        HdStBufferResourceGLSharedPtr res =
            boost::static_pointer_cast<HdStBufferResourceGL> (res_);

        BindBuffer(req.GetName(), res, req.GetOffset());
    } else if (req.IsInterleavedBufferArray()) {
        // note: interleaved buffer needs only 1 binding
        HdBufferArrayRangeSharedPtr bar_ = req.GetBar();
        HdStBufferArrayRangeGLSharedPtr bar =
            boost::static_pointer_cast<HdStBufferArrayRangeGL> (bar_);
        BindBuffer(req.GetName(), bar->GetResource(), req.GetOffset());
    } else if (req.IsBufferArray()) {
        HdBufferArrayRangeSharedPtr bar_ = req.GetBar();
        HdStBufferArrayRangeGLSharedPtr bar =
            boost::static_pointer_cast<HdStBufferArrayRangeGL> (bar_);
        BindBufferArray(bar);
    }
}

void
HdSt_ResourceBinder::Unbind(HdBindingRequest const& req) const
{
    if (req.IsTypeless()) {
        return;
    } else if (req.IsResource()) {
        HdBufferResourceSharedPtr res_ = req.GetResource();
        HdStBufferResourceGLSharedPtr res =
            boost::static_pointer_cast<HdStBufferResourceGL> (res_);

        UnbindBuffer(req.GetName(), res);
    } else if (req.IsInterleavedBufferArray()) {
        // note: interleaved buffer needs only 1 binding
        HdBufferArrayRangeSharedPtr bar_ = req.GetBar();
        HdStBufferArrayRangeGLSharedPtr bar =
            boost::static_pointer_cast<HdStBufferArrayRangeGL> (bar_);

        UnbindBuffer(req.GetName(), bar->GetResource());
    } else if (req.IsBufferArray()) {
        HdBufferArrayRangeSharedPtr bar_ = req.GetBar();
        HdStBufferArrayRangeGLSharedPtr bar =
            boost::static_pointer_cast<HdStBufferArrayRangeGL> (bar_);

        UnbindBufferArray(bar);
    }
}

void
HdSt_ResourceBinder::UnbindBufferArray(
    HdStBufferArrayRangeGLSharedPtr const &bar) const
{
    if (!bar) return;

    TF_FOR_ALL(it, bar->GetResources()) {
        UnbindBuffer(it->first, it->second);
    }
}

void
HdSt_ResourceBinder::BindUniformi(TfToken const &name,
                                int count, const int *value) const
{
    HdBinding uniformLocation = GetBinding(name);
    if (uniformLocation.GetLocation() == HdBinding::NOT_EXIST) return;

    TF_VERIFY(uniformLocation.IsValid());
    TF_VERIFY(uniformLocation.GetType() == HdBinding::UNIFORM);

    if (count == 1) {
        glUniform1iv(uniformLocation.GetLocation(), 1, value);
    } else if (count == 2) {
        glUniform2iv(uniformLocation.GetLocation(), 1, value);
    } else if (count == 3) {
        glUniform3iv(uniformLocation.GetLocation(), 1, value);
    } else if (count == 4) {
        glUniform4iv(uniformLocation.GetLocation(), 1, value);
    } else {
        TF_CODING_ERROR("Invalid count %d.\n", count);
    }
}

void
HdSt_ResourceBinder::BindUniformArrayi(TfToken const &name,
                                 int count, const int *value) const
{
    HdBinding uniformLocation = GetBinding(name);
    if (uniformLocation.GetLocation() == HdBinding::NOT_EXIST) return;

    TF_VERIFY(uniformLocation.IsValid());
    TF_VERIFY(uniformLocation.GetType() == HdBinding::UNIFORM_ARRAY);

    glUniform1iv(uniformLocation.GetLocation(), count, value);
}

void
HdSt_ResourceBinder::BindUniformui(TfToken const &name,
                                int count, const unsigned int *value) const
{
    HdBinding uniformLocation = GetBinding(name);
    if (uniformLocation.GetLocation() == HdBinding::NOT_EXIST) return;

    TF_VERIFY(uniformLocation.IsValid());
    TF_VERIFY(uniformLocation.GetType() == HdBinding::UNIFORM);

    if (count == 1) {
        glUniform1uiv(uniformLocation.GetLocation(), 1, value);
    } else if (count == 2) {
        glUniform2uiv(uniformLocation.GetLocation(), 1, value);
    } else if (count == 3) {
        glUniform3uiv(uniformLocation.GetLocation(), 1, value);
    } else if (count == 4) {
        glUniform4uiv(uniformLocation.GetLocation(), 1, value);
    } else {
        TF_CODING_ERROR("Invalid count %d.", count);
    }
}

void
HdSt_ResourceBinder::BindUniformf(TfToken const &name,
                                int count, const float *value) const
{
    HdBinding uniformLocation = GetBinding(name);
    if (uniformLocation.GetLocation() == HdBinding::NOT_EXIST) return;

    if (!TF_VERIFY(uniformLocation.IsValid())) return;
    if (!TF_VERIFY(uniformLocation.GetType() == HdBinding::UNIFORM)) return;
    GLint location = uniformLocation.GetLocation();

    if (count == 1) {
        glUniform1fv(location, 1, value);
    } else if (count == 2) {
        glUniform2fv(location, 1, value);
    } else if (count == 3) {
        glUniform3fv(location, 1, value);
    } else if (count == 4) {
        glUniform4fv(location, 1, value);
    } else if (count == 16) {
        glUniformMatrix4fv(location, 1, /*transpose=*/false, value);
    } else {
        TF_CODING_ERROR("Invalid count %d.", count);
    }
}

void
HdSt_ResourceBinder::IntrospectBindings(HdStResourceGL const & programResource)
{
    GlfContextCaps const &caps = GlfContextCaps::GetInstance();

    GLuint program = programResource.GetId();

    if (ARCH_UNLIKELY(!caps.shadingLanguage420pack)) {
        GLint numUBO = 0;
        glGetProgramiv(program, GL_ACTIVE_UNIFORM_BLOCKS, &numUBO);

        const int MAX_NAME = 256;
        int length = 0;
        char name[MAX_NAME+1];
        for (int i = 0; i < numUBO; ++i) {
            glGetActiveUniformBlockName(program, i, MAX_NAME, &length, name);
            // note: ubo_ prefix is added in HdCodeGen::_EmitDeclaration()
            if (strstr(name, "ubo_") == name) {
                HdBinding binding;
                if (TfMapLookup(_bindingMap, NameAndLevel(TfToken(name+4)), &binding)) {
                    // set uniform block binding.
                    glUniformBlockBinding(program, i, binding.GetLocation());
                }
            }
        }
    }

    if (ARCH_UNLIKELY(!caps.explicitUniformLocation)) {
        TF_FOR_ALL(it, _bindingMap) {
            HdBinding binding = it->second;
            HdBinding::Type type = binding.GetType();
            std::string name = it->first.name;
            int level = it->first.level;
            if (level >=0) {
                // follow nested instancing naming convention.
                std::stringstream n;
                n << name << "_" << level;
                name = n.str();
            }
            if (type == HdBinding::UNIFORM       ||
                type == HdBinding::UNIFORM_ARRAY ||
                type == HdBinding::TBO) {
                GLint loc = glGetUniformLocation(program, name.c_str());
                // update location in resource binder.
                // some uniforms may be optimized out.
                if (loc < 0) loc = HdBinding::NOT_EXIST;
                it->second.Set(type, loc, binding.GetTextureUnit());
            } else if (type == HdBinding::TEXTURE_2D) {
                // note: sampler2d_ prefix is added in
                // HdCodeGen::_GenerateShaderParameters()
                name = "sampler2d_" + name;
                GLint loc = glGetUniformLocation(program, name.c_str());
                if (loc < 0) loc = HdBinding::NOT_EXIST;
                it->second.Set(type, loc, binding.GetTextureUnit());
            }
        }
    }
}

HdSt_ResourceBinder::MetaData::ID
HdSt_ResourceBinder::MetaData::ComputeHash() const
{
    ID hash = 0;
    
    boost::hash_combine(hash, drawingCoord0Binding.binding.GetValue());
    boost::hash_combine(hash, drawingCoord0Binding.dataType);
    boost::hash_combine(hash, drawingCoord1Binding.binding.GetValue());
    boost::hash_combine(hash, drawingCoord1Binding.dataType);
    boost::hash_combine(hash, drawingCoord2Binding.binding.GetValue());
    boost::hash_combine(hash, drawingCoord2Binding.dataType);
    boost::hash_combine(hash, drawingCoordIBinding.binding.GetValue());
    boost::hash_combine(hash, drawingCoordIBinding.dataType);
    boost::hash_combine(hash, instanceIndexArrayBinding.binding.GetValue());
    boost::hash_combine(hash, instanceIndexArrayBinding.dataType);
    boost::hash_combine(hash, instanceIndexBaseBinding.binding.GetValue());
    boost::hash_combine(hash, instanceIndexBaseBinding.dataType);
    boost::hash_combine(hash, primitiveParamBinding.binding.GetValue());
    boost::hash_combine(hash, primitiveParamBinding.dataType);
    boost::hash_combine(hash, edgeIndexBinding.binding.GetValue());
    boost::hash_combine(hash, edgeIndexBinding.dataType);

    // separators are inserted to distinguish primvars have a same layout
    // but different interpolation.
    boost::hash_combine(hash, 0); // separator
    TF_FOR_ALL(binDecl, customBindings) {
        boost::hash_combine(hash, binDecl->name.Hash());
        boost::hash_combine(hash, binDecl->dataType);
        boost::hash_combine(hash, binDecl->binding.GetType());
        boost::hash_combine(hash, binDecl->binding.GetLocation());
    }

    boost::hash_combine(hash, 0); // separator
    TF_FOR_ALL(blockIt, customInterleavedBindings) {
        boost::hash_combine(hash, (int)blockIt->first.GetType()); // binding
        TF_FOR_ALL (it, blockIt->second.entries) {
            StructEntry const &entry = *it;
            boost::hash_combine(hash, entry.name.Hash());
            boost::hash_combine(hash, entry.dataType);
            boost::hash_combine(hash, entry.offset);
            boost::hash_combine(hash, entry.arraySize);
        }
    }

    boost::hash_combine(hash, 0); // separator
    TF_FOR_ALL (blockIt, constantData) {
        boost::hash_combine(hash, (int)blockIt->first.GetType()); // binding
        TF_FOR_ALL (it, blockIt->second.entries) {
            StructEntry const &entry = *it;
            boost::hash_combine(hash, entry.name.Hash());
            boost::hash_combine(hash, entry.dataType);
            boost::hash_combine(hash, entry.offset);
            boost::hash_combine(hash, entry.arraySize);
        }
    }

    boost::hash_combine(hash, 0); // separator
    TF_FOR_ALL (blockIt, topologyVisibilityData) {
        boost::hash_combine(hash, (int)blockIt->first.GetType()); // binding
        TF_FOR_ALL (it, blockIt->second.entries) {
            StructEntry const &entry = *it;
            boost::hash_combine(hash, entry.name.Hash());
            boost::hash_combine(hash, entry.dataType);
            boost::hash_combine(hash, entry.offset);
            boost::hash_combine(hash, entry.arraySize);
        }
    }

    boost::hash_combine(hash, 0); // separator
    TF_FOR_ALL (it, instanceData) {
        boost::hash_combine(hash, (int)it->first.GetType()); // binding
        NestedPrimvar const &primvar = it->second;
        boost::hash_combine(hash, primvar.name.Hash());
        boost::hash_combine(hash, primvar.dataType);
        boost::hash_combine(hash, primvar.level);
    }
    boost::hash_combine(hash, 0); // separator
    TF_FOR_ALL (it, vertexData) {
        boost::hash_combine(hash, (int)it->first.GetType()); // binding
        Primvar const &primvar = it->second;
        boost::hash_combine(hash, primvar.name.Hash());
        boost::hash_combine(hash, primvar.dataType);
    }
    boost::hash_combine(hash, 0); // separator
    TF_FOR_ALL (it, elementData) {
        boost::hash_combine(hash, (int)it->first.GetType()); // binding
        Primvar const &primvar = it->second;
        boost::hash_combine(hash, primvar.name.Hash());
        boost::hash_combine(hash, primvar.dataType);
    }
    boost::hash_combine(hash, 0); // separator
    TF_FOR_ALL (it, fvarData) {
        boost::hash_combine(hash, (int)it->first.GetType()); // binding
        Primvar const &primvar = it->second;
        boost::hash_combine(hash, primvar.name.Hash());
        boost::hash_combine(hash, primvar.dataType);
    }
    boost::hash_combine(hash, 0); // separator
    TF_FOR_ALL (blockIt, shaderData) {
        boost::hash_combine(hash, (int)blockIt->first.GetType()); // binding
        TF_FOR_ALL (it, blockIt->second.entries) {
            StructEntry const &entry = *it;
            boost::hash_combine(hash, entry.name.Hash());
            boost::hash_combine(hash, entry.dataType);
            boost::hash_combine(hash, entry.offset);
            boost::hash_combine(hash, entry.arraySize);
        }
    }
    boost::hash_combine(hash, 0); // separator
    TF_FOR_ALL (it, shaderParameterBinding) {
        boost::hash_combine(hash, (int)it->first.GetType()); // binding
        ShaderParameterAccessor const &entry = it->second;
        boost::hash_combine(hash, entry.name.Hash());
        boost::hash_combine(hash, entry.dataType);
    }

    return hash;
}

PXR_NAMESPACE_CLOSE_SCOPE

