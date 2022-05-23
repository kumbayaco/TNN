// Copyright 2019 Tencent. All Rights Reserved

#include "snpe_utils.h"
#include "core/macro.h"

#include "DlSystem/StringList.hpp"
#include "DlSystem/TensorShape.hpp"
#include "SNPE/SNPEFactory.hpp"

namespace TNN_NS {

size_t CalcSizeFromDims(const zdl::DlSystem::Dimension* dims, size_t rank,
                        size_t element_size) {
    if (rank == 0)
        return 0;
    size_t size = element_size;
    while (rank--) {
        (*dims == 0) ? size *= 0 : size *= *dims;
        dims++;
    }
    return size;
}

std::unique_ptr<zdl::SNPE::SNPE> SetBuilderOptions(
    std::unique_ptr<zdl::DlContainer::IDlContainer>& container,
    zdl::DlSystem::Runtime_t runtime, zdl::DlSystem::RuntimeList runtime_list,
    zdl::DlSystem::UDLBundle udlbundle, bool use_user_supplied_buffers,
    zdl::DlSystem::PlatformConfig platform_config, bool use_caching) {
    std::unique_ptr<zdl::SNPE::SNPE> snpe;
    zdl::SNPE::SNPEBuilder snpeBuilder(container.get());

    if (runtime_list.empty()) {
        runtime_list.add(runtime);
    }

    snpe = snpeBuilder.setOutputLayers({})
               .setRuntimeProcessorOrder(runtime_list)
               .setUdlBundle(udlbundle)
               .setUseUserSuppliedBuffers(use_user_supplied_buffers)
               .setPlatformConfig(platform_config)
               .setInitCacheMode(use_caching)
               .build();
    return snpe;
}

void CreateUserBuffer(
    zdl::DlSystem::UserBufferMap& user_buffer_map, BlobMap& blobmap,
    std::unordered_map<std::string, std::vector<uint8_t>>& application_buffers,
    std::vector<std::unique_ptr<zdl::DlSystem::IUserBuffer>>&
        snpe_userbacked_buffers,
    std::unique_ptr<zdl::SNPE::SNPE>& snpe, const char* name,
    const bool is_tf8_buffer) {
    // get attributes of buffer by name
    auto buffer_attr = snpe->getInputOutputBufferAttributes(name);
    if (!buffer_attr)
        throw std::runtime_error(
            std::string("Error obtaining attributes for input tensor ") + name);

    // calculate the size of buffer required by the input tensor
    const zdl::DlSystem::TensorShape& buffer_shape = (*buffer_attr)->getDims();

    // Calculate the stride based on buffer strides.
    // Note: Strides = Number of bytes to advance to the next element in each
    // dimension. For example, if a float tensor of dimension 2x4x3 is tightly
    // packed in a buffer of 96 bytes, then the strides would be (48,12,4) Note:
    // Buffer stride is usually known and does not need to be calculated.
    std::vector<size_t> strides(buffer_shape.rank());
    strides[strides.size() - 1] =
        is_tf8_buffer ? sizeof(uint8_t) : sizeof(float);
    size_t stride = strides[strides.size() - 1];
    for (size_t i = buffer_shape.rank() - 1; i > 0; i--) {
        (buffer_shape[i] == 0) ? stride *= 0 : stride *= buffer_shape[i];
        strides[i - 1] = stride;
    }

    const size_t buffer_element_size =
        is_tf8_buffer ? sizeof(uint8_t) : sizeof(float);
    size_t buf_size = CalcSizeFromDims(
        buffer_shape.getDimensions(), buffer_shape.rank(), buffer_element_size);

    // set the buffer encoding type
    std::unique_ptr<zdl::DlSystem::UserBufferEncoding> user_buffer_encoding;
    if (buffer_element_size == sizeof(uint8_t)) {
        user_buffer_encoding =
            std::move(std::unique_ptr<zdl::DlSystem::UserBufferEncodingTf8>(
                new zdl::DlSystem::UserBufferEncodingTf8(0, 1.0)));
    } else {
        user_buffer_encoding =
            std::move(std::unique_ptr<zdl::DlSystem::UserBufferEncodingFloat>(
                new zdl::DlSystem::UserBufferEncodingFloat()));
    }

    // create user-backed storage to load input data onto it
    application_buffers.emplace(name, std::vector<uint8_t>(buf_size));

    // create SNPE user buffer from the user-backed buffer
    zdl::DlSystem::IUserBufferFactory& ub_factory =
        zdl::SNPE::SNPEFactory::getUserBufferFactory();
    snpe_userbacked_buffers.push_back(ub_factory.createUserBuffer(
        application_buffers.at(name).data(), buf_size, strides,
        user_buffer_encoding.get()));
    if (snpe_userbacked_buffers.back() == nullptr) {
        LOGE("Error while creating user buffer.\n");
    }
    // add the user-backed buffer to the inputMap, which is later on fed to the
    // network for execution
    user_buffer_map.add(name, snpe_userbacked_buffers.back().get());

    // add blob
    BlobDesc desc;
    desc.data_format = DATA_FORMAT_NHWC;
    desc.name        = name;
    for (size_t i = 0; i < buffer_shape.rank(); i++) {
        desc.dims.push_back(buffer_shape[i]);
    }
    BlobHandle handle;
    handle.base   = application_buffers.at(name).data();
    blobmap[name] = new Blob(desc, handle);
}

void CreateInputBufferMap(
    zdl::DlSystem::UserBufferMap& input_map, BlobMap& input_blobmap,
    std::unordered_map<std::string, std::vector<uint8_t>>& application_buffers,
    std::vector<std::unique_ptr<zdl::DlSystem::IUserBuffer>>&
        snpe_userbacked_buffers,
    std::unique_ptr<zdl::SNPE::SNPE>& snpe, bool is_tf8_buffer) {
    // get input tensor names of the network that need to be populated
    const auto& input_names_opt = snpe->getInputTensorNames();
    if (!input_names_opt)
        throw std::runtime_error("Error obtaining input tensor names");
    const zdl::DlSystem::StringList& input_names = *input_names_opt;
    assert(input_names.size() > 0);

    // create SNPE user buffers for each application storage buffer
    for (const char* name : input_names) {
        CreateUserBuffer(input_map, input_blobmap, application_buffers,
                         snpe_userbacked_buffers, snpe, name, is_tf8_buffer);
    }
}

void CreateOutputBufferMap(
    zdl::DlSystem::UserBufferMap& output_map, BlobMap& output_blobmap,
    std::unordered_map<std::string, std::vector<uint8_t>>& application_buffers,
    std::vector<std::unique_ptr<zdl::DlSystem::IUserBuffer>>&
        snpe_userbacked_buffers,
    std::unique_ptr<zdl::SNPE::SNPE>& snpe, bool is_tf8_buffer) {
    // get input tensor names of the network that need to be populated
    const auto& output_names_opt = snpe->getOutputTensorNames();
    if (!output_names_opt)
        throw std::runtime_error("Error obtaining output tensor names");
    const zdl::DlSystem::StringList& output_names = *output_names_opt;

    // create SNPE user buffers for each application storage buffer
    for (const char* name : output_names) {
        CreateUserBuffer(output_map, output_blobmap, application_buffers,
                         snpe_userbacked_buffers, snpe, name, is_tf8_buffer);
    }
}

}  // namespace TNN_NS
