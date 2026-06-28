#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

struct HdrImage;
struct Config;
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
        // Second UBO + descriptor for the sharp thumbnail (same texture, blur=0).
        VkBuffer thumbUbo = VK_NULL_HANDLE;
        VkDeviceMemory thumbUboMem = VK_NULL_HANDLE;
        void *thumbUboMapped = nullptr;
        VkDescriptorSet thumbDescriptor = VK_NULL_HANDLE;
        // Overlay (clock/date): UBO + descriptor (bound to the overlay texture) +
        // a small vertex buffer for the positioned quad.
        VkPipeline overlayPipeline = VK_NULL_HANDLE; // non-owning (format cache)
        VkBuffer overlayUbo = VK_NULL_HANDLE;
        VkDeviceMemory overlayUboMem = VK_NULL_HANDLE;
        void *overlayUboMapped = nullptr;
        VkDescriptorSet overlayDescriptor = VK_NULL_HANDLE;
        VkBuffer overlayVbo = VK_NULL_HANDLE;
        VkDeviceMemory overlayVboMem = VK_NULL_HANDLE;
        void *overlayVboMapped = nullptr;
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

    explicit Renderer(const Config &cfg);
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

    // (Re)upload the overlay panel (clock/date) as a shared sRGB texture. Must be
    // called once before createOutput, and again on each per-minute refresh. The
    // dimensions are fixed across refreshes so descriptors stay valid.
    bool uploadOverlay(const uint8_t *rgba, int w, int h);
    bool hasOverlay() const { return m_overlayReady; }

    // Acquire -> record -> submit -> present one frame for this output.
    void renderOutput(Output &out);

    void destroyOutput(Output &out);

private:
    bool pickPhysicalDevice(VkSurfaceKHR probe);
    bool createLogicalDevice();
    bool createSharedResources(); // descriptor layout/pool, pipeline layout, sampler
    bool chooseFormat(VkSurfaceKHR surface, bool wantHdr, VkSurfaceFormatKHR &out, bool &gotHdr);
    bool getOrCreatePipeline(VkFormat format, VkRenderPass &rp, VkPipeline &present, VkPipeline &overlay);
    bool uploadTexture(const HdrImage &img);
    // Create a 16-float UBO + a descriptor set bound to (that UBO, the given texture view).
    bool createUboSet(VkBuffer &buf, VkDeviceMemory &mem, void *&mapped, VkDescriptorSet &set,
                      VkImageView view);
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;

    // One render pass + present + overlay pipeline per distinct swapchain format.
    struct FormatPipeline {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipeline overlayPipeline = VK_NULL_HANDLE;
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

    // Shared overlay texture (clock/date panel), fixed size across refreshes.
    VkImage m_overlayTex = VK_NULL_HANDLE;
    VkDeviceMemory m_overlayMem = VK_NULL_HANDLE;
    VkImageView m_overlayView = VK_NULL_HANDLE;
    int m_overlayW = 0, m_overlayH = 0;
    bool m_overlayReady = false;

    bool m_wantHdr = true;
    bool m_deviceReady = false;
    float m_dim = 0.5f;   // background dim multiplier (linear); VANTALOCK_DIM
    float m_blur = 0.02f; // background blur radius in uv units; VANTALOCK_BLUR
    int m_blurType = 0;   // background blur style (see Config::blurType)
    bool m_thumb = true;        // draw the sharp thumbnail
    float m_thumbFrac = 0.24f;  // thumbnail height as fraction of output
    float m_thumbY = 0.55f;     // thumbnail vertical centre
    float m_thumbRadius = 0.0f; // corner rounding, fraction of thumb height (0 = square)

    // Rainbow overlay (static unless m_rainbowPhase is animated by the caller)
    bool m_rainbow = false;
    std::vector<float> m_rainbowStops; // flattened rgba in 0..1, 4 floats per stop (max 8 used)
    float m_rainbowPeriod = 0.0f;      // px per cycle (<=0 = span once across the diagonal)
    float m_rainbowPhase = 0.0f;       // 0..1 scroll offset; the animation loop advances this
};
