#pragma once

#include "GpuCapabilities.h"
#include "GpuTypes.h"

#include <span>
#include <string_view>

namespace ParticleSaturn::Gpu {

class CommandList {
public:
    virtual ~CommandList() = default;
    virtual void Transition(BufferHandle buffer, ResourceUsage before, ResourceUsage after) = 0;
    virtual void Transition(TextureHandle texture, ResourceUsage before, ResourceUsage after) = 0;
    virtual void DrawIndirect(BufferHandle arguments, std::size_t offset) = 0;
    virtual void Dispatch(std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ) = 0;
};

class GpuDevice {
public:
    virtual ~GpuDevice() = default;
    virtual std::string_view Name() const noexcept = 0;
    virtual const GpuCapabilities& Capabilities() const noexcept = 0;
    virtual BufferHandle CreateBuffer(const BufferDesc& desc, std::span<const std::byte> initialData) = 0;
    virtual void UpdateBuffer(BufferHandle buffer, std::size_t offset, std::span<const std::byte> data) = 0;
    virtual void DestroyBuffer(BufferHandle buffer, FrameToken afterFrame) = 0;
    virtual CommandList& BeginCommands() = 0;
    virtual FrameToken Submit(CommandList& commands) = 0;
};

} // namespace ParticleSaturn::Gpu
