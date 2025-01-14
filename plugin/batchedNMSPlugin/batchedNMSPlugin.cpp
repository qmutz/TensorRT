/*
 * Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "batchedNMSPlugin.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

using namespace nvinfer1;
using nvinfer1::plugin::BatchedNMSBasePluginCreator;
using nvinfer1::plugin::BatchedNMSDynamicPlugin;
using nvinfer1::plugin::BatchedNMSDynamicPluginCreator;
using nvinfer1::plugin::BatchedNMSPlugin;
using nvinfer1::plugin::BatchedNMSPluginCreator;
using nvinfer1::plugin::NMSParameters;

namespace
{
const char* NMS_PLUGIN_VERSION{"1"};
const char* NMS_PLUGIN_NAMES[] = {"BatchedNMS_TRT", "BatchedNMSDynamic_TRT"};
} // namespace

PluginFieldCollection BatchedNMSBasePluginCreator::mFC{};
std::vector<PluginField> BatchedNMSBasePluginCreator::mPluginAttributes;

BatchedNMSPlugin::BatchedNMSPlugin(NMSParameters params)
    : param(params)
{
}

BatchedNMSPlugin::BatchedNMSPlugin(const void* data, size_t length)
{
    const char *d = reinterpret_cast<const char*>(data), *a = d;
    param = read<NMSParameters>(d);
    boxesSize = read<int>(d);
    scoresSize = read<int>(d);
    numPriors = read<int>(d);
    mClipBoxes = read<bool>(d);
    mPrecision = read<DataType>(d);
    ASSERT(d == a + length);
}

BatchedNMSDynamicPlugin::BatchedNMSDynamicPlugin(NMSParameters params)
    : param(params)
{
}

BatchedNMSDynamicPlugin::BatchedNMSDynamicPlugin(const void* data, size_t length)
{
    const char *d = reinterpret_cast<const char*>(data), *a = d;
    param = read<NMSParameters>(d);
    boxesSize = read<int>(d);
    scoresSize = read<int>(d);
    numPriors = read<int>(d);
    mClipBoxes = read<bool>(d);
    mPrecision = read<DataType>(d);
    ASSERT(d == a + length);
}

int BatchedNMSPlugin::getNbOutputs() const
{
    return 4;
}

int BatchedNMSDynamicPlugin::getNbOutputs() const
{
    return 4;
}

int BatchedNMSPlugin::initialize()
{
    return STATUS_SUCCESS;
}

int BatchedNMSDynamicPlugin::initialize()
{
    return STATUS_SUCCESS;
}

void BatchedNMSPlugin::terminate() {}

void BatchedNMSDynamicPlugin::terminate() {}

Dims BatchedNMSPlugin::getOutputDimensions(int index, const Dims* inputs, int nbInputDims)
{
    ASSERT(nbInputDims == 2);
    ASSERT(index >= 0 && index < this->getNbOutputs());
    ASSERT(inputs[0].nbDims == 3);
    ASSERT(inputs[1].nbDims == 2 || (inputs[1].nbDims == 3 && inputs[1].d[2] == 1));
    // boxesSize: number of box coordinates for one sample
    boxesSize = inputs[0].d[0] * inputs[0].d[1] * inputs[0].d[2];
    // scoresSize: number of scores for one sample
    scoresSize = inputs[1].d[0] * inputs[1].d[1];
    // num_detections
    if (index == 0)
    {
        Dims dim0{};
        dim0.nbDims = 0;
        return dim0;
    }
    // nmsed_boxes
    if (index == 1)
    {
        return DimsHW(param.keepTopK, 4);
    }
    // nmsed_scores or nmsed_classes
    Dims dim1{};
    dim1.nbDims = 1;
    dim1.d[0] = param.keepTopK;
    return dim1;
}

DimsExprs BatchedNMSDynamicPlugin::getOutputDimensions(
    int outputIndex, const DimsExprs* inputs, int nbInputs, IExprBuilder& exprBuilder)
{
    ASSERT(nbInputs == 2);
    ASSERT(outputIndex >= 0 && outputIndex < this->getNbOutputs());

    // Shape of boxes input should be
    // Constant shape: [batch_size, num_boxes, num_classes, 4] or [batch_size, num_boxes, 1, 4]
    //           shareLocation ==              0               or          1
    // or
    // Dynamic shape: some dimension values may be -1
    ASSERT(inputs[0].nbDims == 4);

    // Shape of scores input should be
    // Constant shape: [batch_size, num_boxes, num_classes] or [batch_size, num_boxes, num_classes, 1]
    // or
    // Dynamic shape: some dimension values may be -1
    ASSERT(inputs[1].nbDims == 3 || inputs[1].nbDims == 4);

    if (inputs[0].d[0]->isConstant() && inputs[0].d[1]->isConstant() && inputs[0].d[2]->isConstant()
        && inputs[0].d[3]->isConstant())
    {
        boxesSize = exprBuilder
                        .operation(DimensionOperation::kPROD,
                            *exprBuilder.operation(DimensionOperation::kPROD, *inputs[0].d[1], *inputs[0].d[2]),
                            *inputs[0].d[3])
                        ->getConstantValue();
    }

    if (inputs[1].d[0]->isConstant() && inputs[1].d[1]->isConstant() && inputs[1].d[2]->isConstant())
    {
        scoresSize
            = exprBuilder.operation(DimensionOperation::kPROD, *inputs[1].d[1], *inputs[1].d[2])->getConstantValue();
    }

    DimsExprs out_dim;
    // num_detections
    if (outputIndex == 0)
    {
        out_dim.nbDims = 2;
        out_dim.d[0] = inputs[0].d[0];
        out_dim.d[1] = exprBuilder.constant(1);
    }
    // nmsed_boxes
    else if (outputIndex == 1)
    {
        out_dim.nbDims = 3;
        out_dim.d[0] = inputs[0].d[0];
        out_dim.d[1] = exprBuilder.constant(param.keepTopK);
        out_dim.d[2] = exprBuilder.constant(4);
    }
    // nmsed_scores
    else if (outputIndex == 2)
    {
        out_dim.nbDims = 2;
        out_dim.d[0] = inputs[0].d[0];
        out_dim.d[1] = exprBuilder.constant(param.keepTopK);
    }
    // nmsed_classes
    else
    {
        out_dim.nbDims = 2;
        out_dim.d[0] = inputs[0].d[0];
        out_dim.d[1] = exprBuilder.constant(param.keepTopK);
    }

    return out_dim;
}

size_t BatchedNMSPlugin::getWorkspaceSize(int maxBatchSize) const
{
    return detectionInferenceWorkspaceSize(param.shareLocation, maxBatchSize, boxesSize, scoresSize, param.numClasses,
        numPriors, param.topK, mPrecision, mPrecision);
}

size_t BatchedNMSDynamicPlugin::getWorkspaceSize(
    const PluginTensorDesc* inputs, int nbInputs, const PluginTensorDesc* outputs, int nbOutputs) const
{
    return detectionInferenceWorkspaceSize(param.shareLocation, inputs[0].dims.d[0], boxesSize, scoresSize,
        param.numClasses, numPriors, param.topK, mPrecision, mPrecision);
}

int BatchedNMSPlugin::enqueue(
    int batchSize, const void* const* inputs, void** outputs, void* workspace, cudaStream_t stream)
{
    const void* const locData = inputs[0];
    const void* const confData = inputs[1];

    void* keepCount = outputs[0];
    void* nmsedBoxes = outputs[1];
    void* nmsedScores = outputs[2];
    void* nmsedClasses = outputs[3];

    pluginStatus_t status = nmsInference(stream, batchSize, boxesSize, scoresSize, param.shareLocation,
        param.backgroundLabelId, numPriors, param.numClasses, param.topK, param.keepTopK, param.scoreThreshold,
        param.iouThreshold, mPrecision, locData, mPrecision, confData, keepCount, nmsedBoxes, nmsedScores,
        nmsedClasses, workspace, param.isNormalized, false, mClipBoxes);
    ASSERT(status == STATUS_SUCCESS);
    return 0;
}

int BatchedNMSDynamicPlugin::enqueue(const PluginTensorDesc* inputDesc, const PluginTensorDesc* outputDesc,
    const void* const* inputs, void* const* outputs, void* workspace, cudaStream_t stream)
{
    const void* const locData = inputs[0];
    const void* const confData = inputs[1];

    void* keepCount = outputs[0];
    void* nmsedBoxes = outputs[1];
    void* nmsedScores = outputs[2];
    void* nmsedClasses = outputs[3];

    pluginStatus_t status = nmsInference(stream, inputDesc[0].dims.d[0], boxesSize, scoresSize, param.shareLocation,
        param.backgroundLabelId, numPriors, param.numClasses, param.topK, param.keepTopK, param.scoreThreshold,
        param.iouThreshold, mPrecision, locData, mPrecision, confData, keepCount, nmsedBoxes, nmsedScores,
        nmsedClasses, workspace, param.isNormalized, false, mClipBoxes);
    ASSERT(status == STATUS_SUCCESS);
    return 0;
}

size_t BatchedNMSPlugin::getSerializationSize() const
{
    // NMSParameters, boxesSize,scoresSize,numPriors
    return sizeof(NMSParameters) + sizeof(int) * 3 + sizeof(bool) + sizeof(DataType);
}

void BatchedNMSPlugin::serialize(void* buffer) const
{
    char *d = reinterpret_cast<char*>(buffer), *a = d;
    write(d, param);
    write(d, boxesSize);
    write(d, scoresSize);
    write(d, numPriors);
    write(d, mClipBoxes);
    write(d, mPrecision);
    ASSERT(d == a + getSerializationSize());
}

size_t BatchedNMSDynamicPlugin::getSerializationSize() const
{
    // NMSParameters, boxesSize,scoresSize,numPriors
    return sizeof(NMSParameters) + sizeof(int) * 3 + sizeof(bool) + sizeof(DataType);
}

void BatchedNMSDynamicPlugin::serialize(void* buffer) const
{
    char *d = reinterpret_cast<char*>(buffer), *a = d;
    write(d, param);
    write(d, boxesSize);
    write(d, scoresSize);
    write(d, numPriors);
    write(d, mClipBoxes);
    write(d, mPrecision);
    ASSERT(d == a + getSerializationSize());
}

void BatchedNMSPlugin::configurePlugin(const Dims* inputDims, int nbInputs, const Dims* outputDims, int nbOutputs,
    const DataType* inputTypes, const DataType* outputTypes, const bool* inputIsBroadcast,
    const bool* outputIsBroadcast, nvinfer1::PluginFormat format, int maxBatchSize)
{
    ASSERT(nbInputs == 2);
    ASSERT(nbOutputs == 4);
    ASSERT(inputDims[0].nbDims == 3);
    ASSERT(inputDims[1].nbDims == 2 || (inputDims[1].nbDims == 3 && inputDims[1].d[2] == 1));
    ASSERT(std::none_of(inputIsBroadcast, inputIsBroadcast + nbInputs, [](bool b) { return b; }));
    ASSERT(std::none_of(outputIsBroadcast, outputIsBroadcast + nbInputs, [](bool b) { return b; }));

    boxesSize = inputDims[0].d[0] * inputDims[0].d[1] * inputDims[0].d[2];
    scoresSize = inputDims[1].d[0] * inputDims[1].d[1];
    // num_boxes
    numPriors = inputDims[0].d[0];
    const int numLocClasses = param.shareLocation ? 1 : param.numClasses;
    // Third dimension of boxes must be either 1 or num_classes
    ASSERT(inputDims[0].d[1] == numLocClasses);
    ASSERT(inputDims[0].d[2] == 4);
}

void BatchedNMSDynamicPlugin::configurePlugin(
    const DynamicPluginTensorDesc* in, int nbInputs, const DynamicPluginTensorDesc* out, int nbOutputs)
{
    ASSERT(nbInputs == 2);
    ASSERT(nbOutputs == 4);

    // Shape of boxes input should be
    // Constant shape: [batch_size, num_boxes, num_classes, 4] or [batch_size, num_boxes, 1, 4]
    //           shareLocation ==              0               or          1
    const int numLocClasses = param.shareLocation ? 1 : param.numClasses;
    ASSERT(in[0].desc.dims.nbDims == 4);
    ASSERT(in[0].desc.dims.d[2] == numLocClasses);
    ASSERT(in[0].desc.dims.d[3] == 4);

    // Shape of scores input should be
    // Constant shape: [batch_size, num_boxes, num_classes] or [batch_size, num_boxes, num_classes, 1]
    ASSERT(in[1].desc.dims.nbDims == 3 || (in[1].desc.dims.nbDims == 4 && in[1].desc.dims.d[3] == 1));

    boxesSize = in[0].desc.dims.d[1] * in[0].desc.dims.d[2] * in[0].desc.dims.d[3];
    scoresSize = in[1].desc.dims.d[1] * in[1].desc.dims.d[2];
    // num_boxes
    numPriors = in[0].desc.dims.d[1];

    mPrecision = in[0].desc.type;
}

bool BatchedNMSPlugin::supportsFormat(DataType type, PluginFormat format) const
{
    return ((type == DataType::kHALF || type == DataType::kFLOAT || type == DataType::kINT32) && format == PluginFormat::kNCHW);
}

bool BatchedNMSDynamicPlugin::supportsFormatCombination(
    int pos, const PluginTensorDesc* inOut, int nbInputs, int nbOutputs)
{
    ASSERT(0 <= pos && pos < 6);
    const auto* in = inOut;
    const auto* out = inOut + nbInputs;
    const bool consistentFloatPrecision = in[0].type == in[pos].type;
    switch (pos)
    {
    case 0: return (in[0].type == DataType::kHALF || in[0].type == DataType::kFLOAT) && in[0].format == PluginFormat::kLINEAR && consistentFloatPrecision;
    case 1: return (in[1].type == DataType::kHALF || in[1].type == DataType::kFLOAT) && in[1].format == PluginFormat::kLINEAR && consistentFloatPrecision;
    case 2: return out[0].type == DataType::kINT32 && out[0].format == PluginFormat::kLINEAR;
    case 3: return (out[1].type == DataType::kHALF || out[1].type == DataType::kFLOAT) && out[1].format == PluginFormat::kLINEAR && consistentFloatPrecision;
    case 4: return (out[2].type == DataType::kHALF || out[2].type == DataType::kFLOAT) && out[2].format == PluginFormat::kLINEAR && consistentFloatPrecision;
    case 5: return (out[3].type == DataType::kHALF || out[3].type == DataType::kFLOAT) && out[3].format == PluginFormat::kLINEAR && consistentFloatPrecision;
    }
    return false;
}

const char* BatchedNMSPlugin::getPluginType() const
{
    return NMS_PLUGIN_NAMES[0];
}

const char* BatchedNMSDynamicPlugin::getPluginType() const
{
    return NMS_PLUGIN_NAMES[1];
}

const char* BatchedNMSPlugin::getPluginVersion() const
{
    return NMS_PLUGIN_VERSION;
}

const char* BatchedNMSDynamicPlugin::getPluginVersion() const
{
    return NMS_PLUGIN_VERSION;
}

void BatchedNMSPlugin::destroy()
{
    delete this;
}

void BatchedNMSDynamicPlugin::destroy()
{
    delete this;
}

IPluginV2Ext* BatchedNMSPlugin::clone() const
{
    auto* plugin = new BatchedNMSPlugin(param);
    plugin->boxesSize = boxesSize;
    plugin->scoresSize = scoresSize;
    plugin->numPriors = numPriors;
    plugin->setPluginNamespace(mNamespace.c_str());
    plugin->setClipParam(mClipBoxes);
    plugin->mPrecision = mPrecision;
    return plugin;
}

IPluginV2DynamicExt* BatchedNMSDynamicPlugin::clone() const
{
    auto* plugin = new BatchedNMSDynamicPlugin(param);
    plugin->boxesSize = boxesSize;
    plugin->scoresSize = scoresSize;
    plugin->numPriors = numPriors;
    plugin->setPluginNamespace(mNamespace.c_str());
    plugin->setClipParam(mClipBoxes);
    plugin->mPrecision = mPrecision;
    return plugin;
}

void BatchedNMSPlugin::setPluginNamespace(const char* pluginNamespace)
{
    mNamespace = pluginNamespace;
}

const char* BatchedNMSPlugin::getPluginNamespace() const
{
    return mNamespace.c_str();
}

void BatchedNMSDynamicPlugin::setPluginNamespace(const char* pluginNamespace)
{
    mNamespace = pluginNamespace;
}

const char* BatchedNMSDynamicPlugin::getPluginNamespace() const
{
    return mNamespace.c_str();
}

nvinfer1::DataType BatchedNMSPlugin::getOutputDataType(
    int index, const nvinfer1::DataType* inputTypes, int nbInputs) const
{
    if (index == 0)
    {
        return nvinfer1::DataType::kINT32;
    }
    return inputTypes[0];
}

nvinfer1::DataType BatchedNMSDynamicPlugin::getOutputDataType(
    int index, const nvinfer1::DataType* inputTypes, int nbInputs) const
{
    if (index == 0)
    {
        return nvinfer1::DataType::kINT32;
    }
    return inputTypes[0];
}

void BatchedNMSPlugin::setClipParam(bool clip)
{
    mClipBoxes = clip;
}

void BatchedNMSDynamicPlugin::setClipParam(bool clip)
{
    mClipBoxes = clip;
}

bool BatchedNMSPlugin::isOutputBroadcastAcrossBatch(int outputIndex, const bool* inputIsBroadcasted, int nbInputs) const
{
    return false;
}

bool BatchedNMSPlugin::canBroadcastInputAcrossBatch(int inputIndex) const
{
    return false;
}

BatchedNMSBasePluginCreator::BatchedNMSBasePluginCreator()
    : params{}
{
    mPluginAttributes.emplace_back(PluginField("shareLocation", nullptr, PluginFieldType::kINT32, 1));
    mPluginAttributes.emplace_back(PluginField("backgroundLabelId", nullptr, PluginFieldType::kINT32, 1));
    mPluginAttributes.emplace_back(PluginField("numClasses", nullptr, PluginFieldType::kINT32, 1));
    mPluginAttributes.emplace_back(PluginField("topK", nullptr, PluginFieldType::kINT32, 1));
    mPluginAttributes.emplace_back(PluginField("keepTopK", nullptr, PluginFieldType::kINT32, 1));
    mPluginAttributes.emplace_back(PluginField("scoreThreshold", nullptr, PluginFieldType::kFLOAT32, 1));
    mPluginAttributes.emplace_back(PluginField("iouThreshold", nullptr, PluginFieldType::kFLOAT32, 1));
    mPluginAttributes.emplace_back(PluginField("isNormalized", nullptr, PluginFieldType::kINT32, 1));
    mPluginAttributes.emplace_back(PluginField("clipBoxes", nullptr, PluginFieldType::kINT32, 1));

    mFC.nbFields = mPluginAttributes.size();
    mFC.fields = mPluginAttributes.data();
}

BatchedNMSPluginCreator::BatchedNMSPluginCreator()
{
    mPluginName = NMS_PLUGIN_NAMES[0];
}

BatchedNMSDynamicPluginCreator::BatchedNMSDynamicPluginCreator()
{
    mPluginName = NMS_PLUGIN_NAMES[1];
}

const char* BatchedNMSBasePluginCreator::getPluginName() const
{
    return mPluginName.c_str();
}

const char* BatchedNMSBasePluginCreator::getPluginVersion() const
{
    return NMS_PLUGIN_VERSION;
}

const PluginFieldCollection* BatchedNMSBasePluginCreator::getFieldNames()
{
    return &mFC;
}

IPluginV2Ext* BatchedNMSPluginCreator::createPlugin(const char* name, const PluginFieldCollection* fc)
{
    const PluginField* fields = fc->fields;
    mClipBoxes = true;

    for (int i = 0; i < fc->nbFields; ++i)
    {
        const char* attrName = fields[i].name;
        if (!strcmp(attrName, "shareLocation"))
        {
            params.shareLocation = *(static_cast<const bool*>(fields[i].data));
        }
        else if (!strcmp(attrName, "backgroundLabelId"))
        {
            ASSERT(fields[i].type == PluginFieldType::kINT32);
            params.backgroundLabelId = *(static_cast<const int*>(fields[i].data));
        }
        else if (!strcmp(attrName, "numClasses"))
        {
            ASSERT(fields[i].type == PluginFieldType::kINT32);
            params.numClasses = *(static_cast<const int*>(fields[i].data));
        }
        else if (!strcmp(attrName, "topK"))
        {
            ASSERT(fields[i].type == PluginFieldType::kINT32);
            params.topK = *(static_cast<const int*>(fields[i].data));
        }
        else if (!strcmp(attrName, "keepTopK"))
        {
            ASSERT(fields[i].type == PluginFieldType::kINT32);
            params.keepTopK = *(static_cast<const int*>(fields[i].data));
        }
        else if (!strcmp(attrName, "scoreThreshold"))
        {
            ASSERT(fields[i].type == PluginFieldType::kFLOAT32);
            params.scoreThreshold = *(static_cast<const float*>(fields[i].data));
        }
        else if (!strcmp(attrName, "iouThreshold"))
        {
            ASSERT(fields[i].type == PluginFieldType::kFLOAT32);
            params.iouThreshold = *(static_cast<const float*>(fields[i].data));
        }
        else if (!strcmp(attrName, "isNormalized"))
        {
            params.isNormalized = *(static_cast<const bool*>(fields[i].data));
        }
        else if (!strcmp(attrName, "clipBoxes"))
        {
            mClipBoxes = *(static_cast<const bool*>(fields[i].data));
        }
    }

    auto* plugin = new BatchedNMSPlugin(params);
    plugin->setClipParam(mClipBoxes);
    plugin->setPluginNamespace(mNamespace.c_str());
    return plugin;
}

IPluginV2DynamicExt* BatchedNMSDynamicPluginCreator::createPlugin(const char* name, const PluginFieldCollection* fc)
{
    const PluginField* fields = fc->fields;
    mClipBoxes = true;

    for (int i = 0; i < fc->nbFields; ++i)
    {
        const char* attrName = fields[i].name;
        if (!strcmp(attrName, "shareLocation"))
        {
            params.shareLocation = *(static_cast<const bool*>(fields[i].data));
        }
        else if (!strcmp(attrName, "backgroundLabelId"))
        {
            ASSERT(fields[i].type == PluginFieldType::kINT32);
            params.backgroundLabelId = *(static_cast<const int*>(fields[i].data));
        }
        else if (!strcmp(attrName, "numClasses"))
        {
            ASSERT(fields[i].type == PluginFieldType::kINT32);
            params.numClasses = *(static_cast<const int*>(fields[i].data));
        }
        else if (!strcmp(attrName, "topK"))
        {
            ASSERT(fields[i].type == PluginFieldType::kINT32);
            params.topK = *(static_cast<const int*>(fields[i].data));
        }
        else if (!strcmp(attrName, "keepTopK"))
        {
            ASSERT(fields[i].type == PluginFieldType::kINT32);
            params.keepTopK = *(static_cast<const int*>(fields[i].data));
        }
        else if (!strcmp(attrName, "scoreThreshold"))
        {
            ASSERT(fields[i].type == PluginFieldType::kFLOAT32);
            params.scoreThreshold = *(static_cast<const float*>(fields[i].data));
        }
        else if (!strcmp(attrName, "iouThreshold"))
        {
            ASSERT(fields[i].type == PluginFieldType::kFLOAT32);
            params.iouThreshold = *(static_cast<const float*>(fields[i].data));
        }
        else if (!strcmp(attrName, "isNormalized"))
        {
            params.isNormalized = *(static_cast<const bool*>(fields[i].data));
        }
        else if (!strcmp(attrName, "clipBoxes"))
        {
            mClipBoxes = *(static_cast<const bool*>(fields[i].data));
        }
    }

    auto* plugin = new BatchedNMSDynamicPlugin(params);
    plugin->setClipParam(mClipBoxes);
    plugin->setPluginNamespace(mNamespace.c_str());
    return plugin;
}

IPluginV2Ext* BatchedNMSPluginCreator::deserializePlugin(const char* name, const void* serialData, size_t serialLength)
{
    // This object will be deleted when the network is destroyed, which will
    // call NMS::destroy()
    auto* plugin = new BatchedNMSPlugin(serialData, serialLength);
    plugin->setPluginNamespace(mNamespace.c_str());
    return plugin;
}

IPluginV2DynamicExt* BatchedNMSDynamicPluginCreator::deserializePlugin(
    const char* name, const void* serialData, size_t serialLength)
{
    // This object will be deleted when the network is destroyed, which will
    // call NMS::destroy()
    auto* plugin = new BatchedNMSDynamicPlugin(serialData, serialLength);
    plugin->setPluginNamespace(mNamespace.c_str());
    return plugin;
}
