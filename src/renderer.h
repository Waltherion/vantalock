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
        // This output's chosen swapchain format + mode + (non-owning) pipeline.
        VkFormat format = VK_FORMAT_UNDEFINED;
        bool hdr = false;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
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

    // Wrap a wl_surface as a VkSurfaceKHR (no commit happens here).
    VkSurfaceKHR createWaylandSurface(wl_display *display, wl_surface *surface);

    // Lazily create the device + shared resources + upload the texture, using
    // probe as a present-capable reference surface. No-op after the first call.
    bool ensureDevice(VkSurfaceKHR probe, const HdrImage &img);

    // Non-locking diagnostic: pick a GPU and dump the surface formats, reporting
    // whether the scRGB HDR swapchain format is available. Used by --probe.
    bool probe(VkSurfaceKHR surface);

    // Build the swapchain + per-output resources for one monitor. wantHdr selects
    // an scRGB swapchain (HDR monitor) vs an sRGB one (SDR monitor); the actual
    // mode used is reported in out.hdr (falls back to SDR if scRGB is unavailable).
    bool createOutput(Output &out, VkSurfaceKHR surface, uint32_t w, uint32_t h,
                      const HdrImage &img, bool wantHdr);

    // Acquire -> record -> submit -> present one frame for this output.
    void renderOutput(Output &out);

    void destroyOutput(Output &out);

private:
    bool pickPhysicalDevice(VkSurfaceKHR probe);
    bool createLogicalDevice();
    bool createSharedResources(); // descriptor layout/pool, pipeline layout, sampler
    bool chooseFormat(VkSurfaceKHR surface, bool wantHdr, VkSurfaceFormatKHR &out, bool &gotHdr);
    bool getOrCreatePipeline(VkFormat format, VkRenderPass &rp, VkPipeline &pipe);
    bool uploadTexture(const HdrImage &img);
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;

    // One render pass + pipeline per distinct swapchain format (HDR scRGB vs SDR).
    struct FormatPipeline {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };
    std::vector<FormatPipeline> m_formatPipelines;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_phys = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    VkQueue m_queue = VK_NULL_HANDLE;
    VkCommandPool m_cmdPool = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipeLayout = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;

    // The single HDR texture (wallpaper), shared by all outputs.
    VkImage m_texImage = VK_NULL_HANDLE;
    VkDeviceMemory m_texMem = VK_NULL_HANDLE;
    VkImageView m_texView = VK_NULL_HANDLE;

    bool m_wantHdr = true;
    bool m_deviceReady = false;
    float m_dim = 0.5f;   // background dim multiplier (linear); VANTALOCK_DIM
    float m_blur = 0.02f; // background blur radius in uv units; VANTALOCK_BLUR
};
