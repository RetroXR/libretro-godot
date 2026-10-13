#include "VulkanContext.hpp"

#include "Debug.hpp"

#ifdef __APPLE__

namespace Xenu
{

VulkanContext::~VulkanContext() = default;

bool VulkanContext::Init(retro_hw_render_context_negotiation_interface_vulkan*,
                         int32_t, int32_t)
{
    LogWarning("Vulkan hardware rendering is unavailable on macOS; using software rendering");
    return false;
}

void VulkanContext::Destroy()
{
    m_initialized = false;
}

void VulkanContext::SetImage(const retro_vulkan_image*, uint32_t,
                             const VkSemaphore*, uint32_t)
{
}

void VulkanContext::CompleteFrameWithoutReadback()
{
}

bool VulkanContext::ReadbackToPixels(uint32_t, uint32_t,
                                     godot::PackedByteArray&, VideoTransferStats*)
{
    return false;
}

} // namespace Xenu

#endif // __APPLE__
