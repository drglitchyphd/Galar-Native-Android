// FORK: lsfg-vk's src/extract/trans.cpp (MIT, PancakeTAS/lsfg-vk), changed for the standalone DXVK dxbc library used here (exyorha/dxbc, zlib):
// its DxbcModule::compile returns a ShaderCreateInfo, whose .code is the SPIR-V buffer the original took directly.
#include "extract/trans.hpp"

#include <spirv/unified1/spirv.hpp>   // FORK: the Khronos SPIR-V headers

#include <dxbc_modinfo.h>
#include <dxbc_module.h>
#include <dxbc_reader.h>

#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <vector>

using namespace Extract;

struct BindingOffsets {
  uint32_t bindingIndex{};
  uint32_t bindingOffset{};
  uint32_t setIndex{};
  uint32_t setOffset{};
};

std::vector<uint8_t> Extract::translateShader(std::vector<uint8_t> bytecode) {
    // compile the shader
    dxvk::DxbcReader reader(reinterpret_cast<const char*>(bytecode.data()), bytecode.size());
    dxvk::DxbcModule module(reader);
    const dxvk::DxbcModuleInfo info{};
    auto code = module.compile(info, "CS").code;   // FORK: .code (see above)

    // find all bindings
    std::vector<BindingOffsets> bindingOffsets;
    std::vector<uint32_t> varIds;
    for (auto ins : code) {
        if (ins.opCode() == spv::OpDecorate) {
            if (ins.arg(2) == spv::DecorationBinding) {
                const uint32_t varId = ins.arg(1);
                bindingOffsets.resize(std::max(bindingOffsets.size(), size_t(varId + 1)));
                bindingOffsets[varId].bindingIndex = ins.arg(3);
                bindingOffsets[varId].bindingOffset = ins.offset() + 3;
                varIds.push_back(varId);
            }

            if (ins.arg(2) == spv::DecorationDescriptorSet) {
                const uint32_t varId = ins.arg(1);
                bindingOffsets.resize(std::max(bindingOffsets.size(), size_t(varId + 1)));
                bindingOffsets[varId].setIndex = ins.arg(3);
                bindingOffsets[varId].setOffset = ins.offset() + 3;
            }
        }

        if (ins.opCode() == spv::OpFunction)
            break;
    }

    std::vector<BindingOffsets> validBindings;
    for (const auto varId : varIds) {
        auto info = bindingOffsets[varId];

        if (info.bindingOffset)
            validBindings.push_back(info);
    }

    // patch binding offset
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
        for (size_t i = 0; i < validBindings.size(); i++)
            code.data()[validBindings.at(i).bindingOffset] // NOLINT
                = static_cast<uint8_t>(i);
    #pragma clang diagnostic pop

    // return the new bytecode
    std::vector<uint8_t> spirvBytecode(code.size());
    std::copy_n(reinterpret_cast<uint8_t*>(code.data()),
        code.size(), spirvBytecode.data());
    return spirvBytecode;
}

// FORK: the dxbc library's logger instance (DXVK defines it in its own DLL; the standalone library leaves it to the program)
#include <log/log.h>
namespace dxvk { Logger Logger::s_instance("forklsfg.log"); }
