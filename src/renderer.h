#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

struct HdrImage;
struct wl_display;
struct wl_surface;

// Raw-Vulkan renderer for VantaLock. One shared VkInstance/VkDevice plus the
// pipeline + the single HDR texture (the wallpaper, uploaded once); one
// OutputSwapchain per monitor. The proven HDR path from vantaviewer is reused:
// scRGB extended-linear swapchain (R16G16B16A16_SFLOAT) + windows-scRGB surface
// tag (done separately via cm::SurfaceColor) -> true black on OLED.
class Renderer
{
public:
    // Per-monitor swapchain + descriptor state. Populated by createOutput().
    struct Output {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkExtent2D extent{};
        std::vector<VkImageView> views;
        std::vector<VkFramebuffer> framebuffers;
        VkBuffer ubo = VK_NULL_HANDLE;
        VkDeviceMemory uboMem = VK_NULL_HANDLE;
        void *uboMapped = nullptr;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore acquireSem = VK_NULL_HANDLE;
        VkSemaphore renderSem = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        bool ready = false;
        // Dimensions of the source image (for cover-fit UV maths).
        int imgW = 0, imgH = 0;
        bool imgHdr = false;
        int imgPrimaries = 0;
        float dim = 1.0f;
    };

    explicit Renderer(bool wantHdr);
    ~Renderer();

    bool ok() const { return m_instance != VK_NULL_HANDLE; }
    VkInstance instance() const { return m_instance; }
    bool hdrActive() const { return m_hdr; }

    // Wrap a wl_surface as a VkSurfaceKHR (no commit happens here).
    VkSurfaceKHR createWaylandSurface(wl_display *display, wl_surface *surface);

    // Lazily create the device + pipeline + upload the texture, using probe as a
    // present-capable reference surface. Safe to call repeatedly (no-op after first).
    bool ensureDevice(VkSurfaceKHR probe, const HdrImage &img);

    // Non-locking diagnostic: pick a GPU and dump the surface formats, reporting
    // whether the scRGB HDR swapchain format is available. Used by --probe.
    bool probe(VkSurfaceKHR surface);

    // Build the swapchain + per-output resources for one monitor.
    bool createOutput(Output &out, VkSurfaceKHR surface, uint32_t w, uint32_t h, const HdrImage &img);

    // Acquire -> record -> submit -> present one frame for this output.
    void renderOutput(Output &out);

    void destroyOutput(Output &out);

private:
    bool pickPhysicalDevice(VkSurfaceKHR probe);
    bool createLogicalDevice();
    bool chooseFormat(VkSurfaceKHR probe);
    bool createRenderPipeline();
    bool uploadTexture(const HdrImage &img);
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_phys = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    VkQueue m_queue = VK_NULL_HANDLE;
    VkCommandPool m_cmdPool = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipeLayout = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;

    // The single HDR texture (wallpaper), shared by all outputs.
    VkImage m_texImage = VK_NULL_HANDLE;
    VkDeviceMemory m_texMem = VK_NULL_HANDLE;
    VkImageView m_texView = VK_NULL_HANDLE;

    VkSurfaceFormatKHR m_format{};
    bool m_wantHdr = true;
    bool m_hdr = false;
    bool m_deviceReady = false;
};
