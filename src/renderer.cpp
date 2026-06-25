#include "renderer.h"

#include "config.h"
#include "hdr_image.h"

#include <vulkan/vulkan_wayland.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// SPIR-V for the fullscreen-triangle vertex + present fragment shaders,
// compiled by CMake (glslc -mfmt=c) into comma-separated initialisers.
static const uint32_t kVertSpv[] =
#include "present.vert.inc"
    ;
static const uint32_t kFragSpv[] =
#include "present.frag.inc"
    ;
static const uint32_t kOverlayVertSpv[] =
#include "overlay.vert.inc"
    ;
static const uint32_t kOverlayFragSpv[] =
#include "overlay.frag.inc"
    ;

#define VKCHECK(expr, msg)                                                   \
    do {                                                                     \
        VkResult _r = (expr);                                               \
        if (_r != VK_SUCCESS) {                                             \
            std::fprintf(stderr, "vantalock: %s (VkResult=%d)\n", msg, _r); \
            return false;                                                    \
        }                                                                    \
    } while (0)

namespace {
bool envOn(const char *name, bool dflt)
{
    const char *v = std::getenv(name);
    if (!v)
        return dflt;
    return !(v[0] == '0' && v[1] == '\0');
}
} // namespace

Renderer::Renderer(const Config &cfg)
{
    m_blur = cfg.blur;
    m_dim = cfg.dim;
    m_thumb = cfg.thumbShow;
    m_thumbFrac = cfg.thumbHeight;
    m_thumbY = cfg.thumbY;
    m_thumbRadius = cfg.thumbRadius;

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "vantalock";
    app.apiVersion = VK_API_VERSION_1_1;

    std::vector<const char *> exts = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
        VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME, // exposes HDR colour spaces
    };
    std::vector<const char *> layers;
    if (envOn("VANTALOCK_VK_VALIDATION", false))
        layers.push_back("VK_LAYER_KHRONOS_validation");

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = uint32_t(layers.size());
    ci.ppEnabledLayerNames = layers.data();

    if (vkCreateInstance(&ci, nullptr, &m_instance) != VK_SUCCESS) {
        std::fprintf(stderr, "vantalock: vkCreateInstance failed\n");
        m_instance = VK_NULL_HANDLE;
    }
}

Renderer::~Renderer()
{
    if (m_device) {
        vkDeviceWaitIdle(m_device);
        if (m_texView) vkDestroyImageView(m_device, m_texView, nullptr);
        if (m_texImage) vkDestroyImage(m_device, m_texImage, nullptr);
        if (m_texMem) vkFreeMemory(m_device, m_texMem, nullptr);
        if (m_overlayView) vkDestroyImageView(m_device, m_overlayView, nullptr);
        if (m_overlayTex) vkDestroyImage(m_device, m_overlayTex, nullptr);
        if (m_overlayMem) vkFreeMemory(m_device, m_overlayMem, nullptr);
        for (auto &fp : m_formatPipelines) {
            if (fp.pipeline) vkDestroyPipeline(m_device, fp.pipeline, nullptr);
            if (fp.renderPass) vkDestroyRenderPass(m_device, fp.renderPass, nullptr);
        }
        if (m_pipeLayout) vkDestroyPipelineLayout(m_device, m_pipeLayout, nullptr);
        if (m_descLayout) vkDestroyDescriptorSetLayout(m_device, m_descLayout, nullptr);
        if (m_descPool) vkDestroyDescriptorPool(m_device, m_descPool, nullptr);
        if (m_sampler) vkDestroySampler(m_device, m_sampler, nullptr);
        if (m_cmdPool) vkDestroyCommandPool(m_device, m_cmdPool, nullptr);
        vkDestroyDevice(m_device, nullptr);
    }
    if (m_instance)
        vkDestroyInstance(m_instance, nullptr);
}

VkSurfaceKHR Renderer::createWaylandSurface(wl_display *display, wl_surface *surface)
{
    VkWaylandSurfaceCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    ci.display = display;
    ci.surface = surface;
    VkSurfaceKHR s = VK_NULL_HANDLE;
    if (vkCreateWaylandSurfaceKHR(m_instance, &ci, nullptr, &s) != VK_SUCCESS) {
        std::fprintf(stderr, "vantalock: vkCreateWaylandSurfaceKHR failed\n");
        return VK_NULL_HANDLE;
    }
    return s;
}

uint32_t Renderer::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(m_phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

bool Renderer::pickPhysicalDevice(VkSurfaceKHR probe)
{
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(m_instance, &n, nullptr);
    if (!n) {
        std::fprintf(stderr, "vantalock: no Vulkan physical devices\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(m_instance, &n, devs.data());

    for (VkPhysicalDevice d : devs) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qf.data());
        for (uint32_t i = 0; i < qn; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, probe, &present);
            if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                m_phys = d;
                m_queueFamily = i;
                VkPhysicalDeviceProperties props;
                vkGetPhysicalDeviceProperties(d, &props);
                std::fprintf(stderr, "vantalock: GPU %s, queue family %u\n", props.deviceName, i);
                return true;
            }
        }
    }
    std::fprintf(stderr, "vantalock: no graphics+present queue family\n");
    return false;
}

bool Renderer::createLogicalDevice()
{
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = m_queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    const char *devExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExts;

    VKCHECK(vkCreateDevice(m_phys, &dci, nullptr, &m_device), "vkCreateDevice");
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = m_queueFamily;
    VKCHECK(vkCreateCommandPool(m_device, &pci, nullptr, &m_cmdPool), "vkCreateCommandPool");
    return true;
}

bool Renderer::chooseFormat(VkSurfaceKHR surface, bool wantHdr, VkSurfaceFormatKHR &out, bool &gotHdr)
{
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_phys, surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_phys, surface, &n, fmts.data());

    // HDR monitor: fp16 extended-linear scRGB (matches vantaviewer's path).
    if (wantHdr) {
        for (const auto &f : fmts) {
            if (f.format == VK_FORMAT_R16G16B16A16_SFLOAT
                && f.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) {
                out = f;
                gotHdr = true;
                return true;
            }
        }
        std::fprintf(stderr, "vantalock: no scRGB HDR format on this output; using SDR\n");
    }

    // SDR: a plain UNORM (the shader does sRGB encoding itself, so we must NOT
    // pick a _SRGB format or the hardware would double-encode).
    for (const auto &f : fmts) {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM)
            && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            out = f;
            gotHdr = false;
            return true;
        }
    }
    if (n) {
        out = fmts[0];
        gotHdr = false;
        return true;
    }
    std::fprintf(stderr, "vantalock: no surface formats\n");
    return false;
}

bool Renderer::createSharedResources()
{
    // Descriptor set layout: UBO (binding 0) + sampled texture (binding 1), frag stage.
    VkDescriptorSetLayoutBinding binds[2]{};
    binds[0].binding = 0;
    binds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binds[0].descriptorCount = 1;
    binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[1].binding = 1;
    binds[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[1].descriptorCount = 1;
    binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dl{};
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = 2;
    dl.pBindings = binds;
    VKCHECK(vkCreateDescriptorSetLayout(m_device, &dl, nullptr, &m_descLayout), "descriptor layout");

    // Pool large enough for several monitors.
    VkDescriptorPoolSize ps[2]{};
    ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ps[0].descriptorCount = 32;
    ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps[1].descriptorCount = 32;
    VkDescriptorPoolCreateInfo dp{};
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = 32;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = ps;
    VKCHECK(vkCreateDescriptorPool(m_device, &dp, nullptr, &m_descPool), "descriptor pool");

    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_descLayout;
    VKCHECK(vkCreatePipelineLayout(m_device, &pl, nullptr, &m_pipeLayout), "pipeline layout");

    // Linear sampler, clamp to edge.
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    VKCHECK(vkCreateSampler(m_device, &si, nullptr, &m_sampler), "sampler");
    return true;
}

bool Renderer::getOrCreatePipeline(VkFormat format, VkRenderPass &rp, VkPipeline &present, VkPipeline &overlay)
{
    for (const auto &fp : m_formatPipelines) {
        if (fp.format == format) {
            rp = fp.renderPass;
            present = fp.pipeline;
            overlay = fp.overlayPipeline;
            return true;
        }
    }

    // Render pass: single colour attachment in this swapchain format.
    VkAttachmentDescription colour{};
    colour.format = format;
    colour.samples = VK_SAMPLE_COUNT_1_BIT;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colour.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colour.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colour.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &colour;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;
    VkRenderPass newRp = VK_NULL_HANDLE;
    VKCHECK(vkCreateRenderPass(m_device, &rpci, nullptr, &newRp), "render pass");

    auto makeModule = [&](const uint32_t *code, size_t bytes, VkShaderModule *out) -> bool {
        VkShaderModuleCreateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mi.codeSize = bytes;
        mi.pCode = code;
        return vkCreateShaderModule(m_device, &mi, nullptr, out) == VK_SUCCESS;
    };
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    if (!makeModule(kVertSpv, sizeof(kVertSpv), &vert)
        || !makeModule(kFragSpv, sizeof(kFragSpv), &frag)) {
        std::fprintf(stderr, "vantalock: shader module creation failed\n");
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO; // none: gl_VertexIndex
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dst{};
    dst.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dst.dynamicStateCount = 2;
    dst.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dst;
    gp.layout = m_pipeLayout;
    gp.renderPass = newRp;
    gp.subpass = 0;

    VkPipeline newPipe = VK_NULL_HANDLE;
    VkResult pr = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &gp, nullptr, &newPipe);
    vkDestroyShaderModule(m_device, vert, nullptr);
    vkDestroyShaderModule(m_device, frag, nullptr);
    if (pr != VK_SUCCESS) {
        std::fprintf(stderr, "vantalock: vkCreateGraphicsPipelines (VkResult=%d)\n", pr);
        vkDestroyRenderPass(m_device, newRp, nullptr);
        return false;
    }

    // Overlay pipeline: same render pass + layout, but with a vertex buffer
    // (pos + uv), triangle strip, and alpha blending for the clock/date panel.
    VkShaderModule ovVert = VK_NULL_HANDLE, ovFrag = VK_NULL_HANDLE;
    if (!makeModule(kOverlayVertSpv, sizeof(kOverlayVertSpv), &ovVert)
        || !makeModule(kOverlayFragSpv, sizeof(kOverlayFragSpv), &ovFrag)) {
        std::fprintf(stderr, "vantalock: overlay shader module creation failed\n");
        vkDestroyPipeline(m_device, newPipe, nullptr);
        vkDestroyRenderPass(m_device, newRp, nullptr);
        return false;
    }
    VkPipelineShaderStageCreateInfo ovStages[2] = { stages[0], stages[1] };
    ovStages[0].module = ovVert;
    ovStages[1].module = ovFrag;

    VkVertexInputBindingDescription vbind{ 0, 4 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription vattr[2]{};
    vattr[0] = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
    vattr[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT, 2 * sizeof(float) };
    VkPipelineVertexInputStateCreateInfo ovVi{};
    ovVi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    ovVi.vertexBindingDescriptionCount = 1;
    ovVi.pVertexBindingDescriptions = &vbind;
    ovVi.vertexAttributeDescriptionCount = 2;
    ovVi.pVertexAttributeDescriptions = vattr;

    VkPipelineInputAssemblyStateCreateInfo ovIa = ia;
    ovIa.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkPipelineColorBlendAttachmentState ovCba = cba;
    ovCba.blendEnable = VK_TRUE;
    ovCba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    ovCba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ovCba.colorBlendOp = VK_BLEND_OP_ADD;
    ovCba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ovCba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ovCba.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo ovCb = cb;
    ovCb.pAttachments = &ovCba;

    VkGraphicsPipelineCreateInfo ovGp = gp;
    ovGp.pStages = ovStages;
    ovGp.pVertexInputState = &ovVi;
    ovGp.pInputAssemblyState = &ovIa;
    ovGp.pColorBlendState = &ovCb;

    VkPipeline newOverlay = VK_NULL_HANDLE;
    VkResult opr = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &ovGp, nullptr, &newOverlay);
    vkDestroyShaderModule(m_device, ovVert, nullptr);
    vkDestroyShaderModule(m_device, ovFrag, nullptr);
    if (opr != VK_SUCCESS) {
        std::fprintf(stderr, "vantalock: overlay vkCreateGraphicsPipelines (VkResult=%d)\n", opr);
        vkDestroyPipeline(m_device, newPipe, nullptr);
        vkDestroyRenderPass(m_device, newRp, nullptr);
        return false;
    }

    m_formatPipelines.push_back({ format, newRp, newPipe, newOverlay });
    rp = newRp;
    present = newPipe;
    overlay = newOverlay;
    return true;
}

bool Renderer::uploadTexture(const HdrImage &img)
{
    const VkDeviceSize bytes = VkDeviceSize(img.rgba16f.size()) * sizeof(uint16_t);

    // Staging buffer (host visible).
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKCHECK(vkCreateBuffer(m_device, &bi, nullptr, &staging), "staging buffer");
    VkMemoryRequirements mreq;
    vkGetBufferMemoryRequirements(m_device, staging, &mreq);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mreq.size;
    ai.memoryTypeIndex = findMemoryType(mreq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKCHECK(vkAllocateMemory(m_device, &ai, nullptr, &stagingMem), "staging memory");
    vkBindBufferMemory(m_device, staging, stagingMem, 0);
    void *p = nullptr;
    vkMapMemory(m_device, stagingMem, 0, bytes, 0, &p);
    std::memcpy(p, img.rgba16f.data(), size_t(bytes));
    vkUnmapMemory(m_device, stagingMem);

    // Device-local texture.
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ici.extent = { uint32_t(img.w), uint32_t(img.h), 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKCHECK(vkCreateImage(m_device, &ici, nullptr, &m_texImage), "texture image");
    vkGetImageMemoryRequirements(m_device, m_texImage, &mreq);
    ai.allocationSize = mreq.size;
    ai.memoryTypeIndex = findMemoryType(mreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKCHECK(vkAllocateMemory(m_device, &ai, nullptr, &m_texMem), "texture memory");
    vkBindImageMemory(m_device, m_texImage, m_texMem, 0);

    // One-time copy + layout transitions.
    VkCommandBufferAllocateInfo cba{};
    cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cba.commandPool = m_cmdPool;
    cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VKCHECK(vkAllocateCommandBuffers(m_device, &cba, &cmd), "upload cmd");
    VkCommandBufferBeginInfo bbi{};
    bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bbi);

    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = m_texImage;
    toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    toDst.srcAccessMask = 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toDst);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { uint32_t(img.w), uint32_t(img.h), 1 };
    vkCmdCopyBufferToImage(cmd, staging, m_texImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toShader = toDst;
    toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toShader);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(m_queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_queue);
    vkFreeCommandBuffers(m_device, m_cmdPool, 1, &cmd);
    vkDestroyBuffer(m_device, staging, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = m_texImage;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VKCHECK(vkCreateImageView(m_device, &vci, nullptr, &m_texView), "texture view");
    return true;
}

bool Renderer::uploadOverlay(const uint8_t *rgba, int w, int h)
{
    if (!m_device || !rgba || w <= 0 || h <= 0)
        return false;

    // First call (fixed size): create the device-local RGBA8 texture + view.
    if (!m_overlayReady) {
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = { uint32_t(w), uint32_t(h), 1 };
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VKCHECK(vkCreateImage(m_device, &ici, nullptr, &m_overlayTex), "overlay image");
        VkMemoryRequirements mreq;
        vkGetImageMemoryRequirements(m_device, m_overlayTex, &mreq);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mreq.size;
        ai.memoryTypeIndex = findMemoryType(mreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VKCHECK(vkAllocateMemory(m_device, &ai, nullptr, &m_overlayMem), "overlay memory");
        vkBindImageMemory(m_device, m_overlayTex, m_overlayMem, 0);

        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = m_overlayTex;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VKCHECK(vkCreateImageView(m_device, &vci, nullptr, &m_overlayView), "overlay view");
        m_overlayW = w;
        m_overlayH = h;
    }
    if (w != m_overlayW || h != m_overlayH) {
        std::fprintf(stderr, "vantalock: overlay size changed unexpectedly; skipping\n");
        return false;
    }

    // Refresh: idle the device (renders are infrequent), then stage + copy.
    vkDeviceWaitIdle(m_device);
    const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKCHECK(vkCreateBuffer(m_device, &bi, nullptr, &staging), "overlay staging");
    VkMemoryRequirements mreq;
    vkGetBufferMemoryRequirements(m_device, staging, &mreq);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mreq.size;
    ai.memoryTypeIndex = findMemoryType(mreq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKCHECK(vkAllocateMemory(m_device, &ai, nullptr, &stagingMem), "overlay staging memory");
    vkBindBufferMemory(m_device, staging, stagingMem, 0);
    void *p = nullptr;
    vkMapMemory(m_device, stagingMem, 0, bytes, 0, &p);
    std::memcpy(p, rgba, size_t(bytes));
    vkUnmapMemory(m_device, stagingMem);

    VkCommandBufferAllocateInfo cba{};
    cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cba.commandPool = m_cmdPool;
    cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VKCHECK(vkAllocateCommandBuffers(m_device, &cba, &cmd), "overlay cmd");
    VkCommandBufferBeginInfo bbi{};
    bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bbi);

    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // discard previous contents
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = m_overlayTex;
    toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    toDst.srcAccessMask = 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toDst);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { uint32_t(w), uint32_t(h), 1 };
    vkCmdCopyBufferToImage(cmd, staging, m_overlayTex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toShader = toDst;
    toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toShader);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(m_queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_queue);
    vkFreeCommandBuffers(m_device, m_cmdPool, 1, &cmd);
    vkDestroyBuffer(m_device, staging, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);

    m_overlayReady = true;
    return true;
}

bool Renderer::ensureDevice(VkSurfaceKHR probe, const HdrImage &img)
{
    if (m_deviceReady)
        return true;
    if (!pickPhysicalDevice(probe)) return false;
    if (!createLogicalDevice()) return false;
    if (!createSharedResources()) return false;
    if (!uploadTexture(img)) return false;
    m_deviceReady = true;
    return true;
}

bool Renderer::probe(VkSurfaceKHR surface)
{
    if (!pickPhysicalDevice(surface))
        return false;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_phys, surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_phys, surface, &n, fmts.data());
    std::fprintf(stderr, "vantalock: %u surface formats:\n", n);
    bool hdr = false;
    for (const auto &f : fmts) {
        const bool isScrgb = f.format == VK_FORMAT_R16G16B16A16_SFLOAT
            && f.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
        if (isScrgb)
            hdr = true;
        std::fprintf(stderr, "    format=%d colorSpace=%d%s\n",
            int(f.format), int(f.colorSpace), isScrgb ? "  <-- scRGB HDR" : "");
    }
    std::fprintf(stderr, "vantalock: scRGB HDR swapchain %s\n",
        hdr ? "AVAILABLE (true black path works)" : "NOT available (would fall back to SDR)");
    return true;
}

bool Renderer::createUboSet(VkBuffer &buf, VkDeviceMemory &mem, void *&mapped, VkDescriptorSet &set,
                            VkImageView view)
{
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = 20 * sizeof(float); // present UBO is 20 floats (incl. rounding vec4)
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKCHECK(vkCreateBuffer(m_device, &bi, nullptr, &buf), "ubo buffer");
    VkMemoryRequirements mreq;
    vkGetBufferMemoryRequirements(m_device, buf, &mreq);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mreq.size;
    ai.memoryTypeIndex = findMemoryType(mreq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKCHECK(vkAllocateMemory(m_device, &ai, nullptr, &mem), "ubo memory");
    vkBindBufferMemory(m_device, buf, mem, 0);
    vkMapMemory(m_device, mem, 0, bi.size, 0, &mapped);

    VkDescriptorSetAllocateInfo da{};
    da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    da.descriptorPool = m_descPool;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &m_descLayout;
    VKCHECK(vkAllocateDescriptorSets(m_device, &da, &set), "descriptor set");

    VkDescriptorBufferInfo dbi{ buf, 0, bi.size };
    VkDescriptorImageInfo dii{ m_sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &dbi;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &dii;
    vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);
    return true;
}

bool Renderer::createOutput(Output &out, VkSurfaceKHR surface, uint32_t w, uint32_t h,
                            const HdrImage &img, bool wantHdr)
{
    out.surface = surface;
    out.imgW = img.w;
    out.imgH = img.h;
    out.imgHdr = img.hdr;
    out.imgPrimaries = int(img.primaries);

    // Per-output format + mode: scRGB on HDR monitors, sRGB on SDR ones.
    VkSurfaceFormatKHR sfmt{};
    bool gotHdr = false;
    if (!chooseFormat(surface, wantHdr, sfmt, gotHdr))
        return false;
    out.format = sfmt.format;
    out.hdr = gotHdr;
    VkPipeline overlayPipe = VK_NULL_HANDLE;
    if (!getOrCreatePipeline(sfmt.format, out.renderPass, out.pipeline, overlayPipe))
        return false;
    std::fprintf(stderr, "vantalock: output swapchain = %s (format=%d)\n",
        gotHdr ? "scRGB (HDR)" : "sRGB (SDR)", int(sfmt.format));

    VkSurfaceCapabilitiesKHR caps;
    VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_phys, surface, &caps), "surface caps");

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = w;
        extent.height = h;
    }
    out.extent = extent;

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        for (uint32_t b = 1; b <= caps.supportedCompositeAlpha; b <<= 1) {
            if (caps.supportedCompositeAlpha & b) {
                alpha = VkCompositeAlphaFlagBitsKHR(b);
                break;
            }
        }
    }

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface;
    sci.minImageCount = imageCount;
    sci.imageFormat = sfmt.format;
    sci.imageColorSpace = sfmt.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = alpha;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    VKCHECK(vkCreateSwapchainKHR(m_device, &sci, nullptr, &out.swapchain), "swapchain");

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(m_device, out.swapchain, &n, nullptr);
    std::vector<VkImage> images(n);
    vkGetSwapchainImagesKHR(m_device, out.swapchain, &n, images.data());

    out.views.resize(n);
    out.framebuffers.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = sfmt.format;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VKCHECK(vkCreateImageView(m_device, &vci, nullptr, &out.views[i]), "swapchain view");

        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = out.renderPass;
        fci.attachmentCount = 1;
        fci.pAttachments = &out.views[i];
        fci.width = extent.width;
        fci.height = extent.height;
        fci.layers = 1;
        VKCHECK(vkCreateFramebuffer(m_device, &fci, nullptr, &out.framebuffers[i]), "framebuffer");
    }

    // UBO (host visible, persistently mapped).
    if (!createUboSet(out.ubo, out.uboMem, out.uboMapped, out.descriptor, m_texView))
        return false;
    if (!createUboSet(out.thumbUbo, out.thumbUboMem, out.thumbUboMapped, out.thumbDescriptor, m_texView))
        return false;

    // Overlay (clock/date): pipeline handle, UBO/descriptor bound to the overlay
    // texture, and a vertex buffer for the positioned quad.
    out.overlayPipeline = overlayPipe;
    if (m_overlayReady) {
        if (!createUboSet(out.overlayUbo, out.overlayUboMem, out.overlayUboMapped,
                          out.overlayDescriptor, m_overlayView))
            return false;
        VkBufferCreateInfo vbi{};
        vbi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        vbi.size = 16 * sizeof(float); // 4 verts * (vec2 pos + vec2 uv)
        vbi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        vbi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VKCHECK(vkCreateBuffer(m_device, &vbi, nullptr, &out.overlayVbo), "overlay vbo");
        VkMemoryRequirements vmr;
        vkGetBufferMemoryRequirements(m_device, out.overlayVbo, &vmr);
        VkMemoryAllocateInfo vai{};
        vai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        vai.allocationSize = vmr.size;
        vai.memoryTypeIndex = findMemoryType(vmr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VKCHECK(vkAllocateMemory(m_device, &vai, nullptr, &out.overlayVboMem), "overlay vbo memory");
        vkBindBufferMemory(m_device, out.overlayVbo, out.overlayVboMem, 0);
        vkMapMemory(m_device, out.overlayVboMem, 0, vbi.size, 0, &out.overlayVboMapped);
    }

    // Command buffer + sync.
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = m_cmdPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VKCHECK(vkAllocateCommandBuffers(m_device, &cbi, &out.cmd), "output cmd");

    VkSemaphoreCreateInfo sem{};
    sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VKCHECK(vkCreateSemaphore(m_device, &sem, nullptr, &out.acquireSem), "acquire sem");
    VKCHECK(vkCreateSemaphore(m_device, &sem, nullptr, &out.renderSem), "render sem");
    VkFenceCreateInfo fen{};
    fen.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fen.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VKCHECK(vkCreateFence(m_device, &fen, nullptr, &out.inFlight), "fence");

    out.ready = true;
    return true;
}

void Renderer::renderOutput(Output &out)
{
    if (!out.ready)
        return;
    vkWaitForFences(m_device, 1, &out.inFlight, VK_TRUE, UINT64_MAX);

    uint32_t idx = 0;
    VkResult acq = vkAcquireNextImageKHR(m_device, out.swapchain, UINT64_MAX,
        out.acquireSem, VK_NULL_HANDLE, &idx);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        std::fprintf(stderr, "vantalock: swapchain out of date on acquire\n");
        return; // Fase 0: skip; recreate handled on next configure
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        std::fprintf(stderr, "vantalock: vkAcquireNextImageKHR failed (%d)\n", acq);
        return;
    }
    vkResetFences(m_device, 1, &out.inFlight);

    // Cover-fit: fill the output, cropping overflow (no letterbox).
    const float as = float(out.extent.width) / float(out.extent.height);
    const float ai = (out.imgH > 0) ? float(out.imgW) / float(out.imgH) : as;
    float usx = 1.0f, usy = 1.0f;
    if (as > ai) usy = ai / as;
    else         usx = as / ai;

    const float scale = out.hdr ? (203.0f / 80.0f) : 1.0f;
    const float sdr = out.hdr ? 0.0f : 1.0f;
    const float imageHdr = out.imgHdr ? 1.0f : 0.0f;
    const float prim = float(out.imgPrimaries);

    float ubo[20] = {
        scale, sdr, imageHdr, 0.0f,      // scale, sdr, imageHdr, rot
        usx, usy, 0.0f, 0.0f,            // uvScale, uvOffset
        prim, 1.0f, m_dim, m_blur,       // primaries, exposure, dim, blur
        0.0f, 0.0f, 0.0f, 1.0f,          // bgColor (true black, opaque)
        0.0f, 0.0f, 0.0f, 0.0f           // rounding (disabled for background)
    };
    std::memcpy(out.uboMapped, ubo, sizeof(ubo));

    // Thumbnail: same texture, sharp (blur=0), undimmed, exact-fit (its viewport
    // matches the image aspect, so uvScale=1 shows the whole image, no letterbox).
    // Corner rounding: radius is a fraction of the thumb height; convert to per-axis
    // window-uv radii so the corners read as circular despite the rect's aspect.
    float rr = m_thumbRadius; // fraction of height
    if (rr > 0.5f) rr = 0.5f;
    const float aspect = (out.imgW > 0 && out.imgH > 0)
        ? float(out.imgW) / float(out.imgH) : 1.0f;
    const float ry = rr;             // uv-y radius (fraction of height)
    const float rx = rr / aspect;    // uv-x radius (narrower because width > height)
    const float roundEnable = (m_thumbRadius > 0.0001f) ? 1.0f : 0.0f;
    float thumbUbo[20] = {
        scale, sdr, imageHdr, 0.0f,
        1.0f, 1.0f, 0.0f, 0.0f,
        prim, 1.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
        rx, ry, roundEnable, 0.0f        // rounding
    };
    std::memcpy(out.thumbUboMapped, thumbUbo, sizeof(thumbUbo));

    vkResetCommandBuffer(out.cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(out.cmd, &bi);

    VkClearValue clear{};
    clear.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = out.renderPass;
    rp.framebuffer = out.framebuffers[idx];
    rp.renderArea.extent = out.extent;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(out.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vpt{ 0, 0, float(out.extent.width), float(out.extent.height), 0.0f, 1.0f };
    VkRect2D sc{ { 0, 0 }, out.extent };
    vkCmdSetViewport(out.cmd, 0, 1, &vpt);
    vkCmdSetScissor(out.cmd, 0, 1, &sc);
    vkCmdBindPipeline(out.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, out.pipeline);
    vkCmdBindDescriptorSets(out.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeLayout,
        0, 1, &out.descriptor, 0, nullptr);
    vkCmdDraw(out.cmd, 3, 1, 0, 0);

    // Sharp thumbnail in a centred sub-viewport sized to the image aspect.
    // Skipped for the 1x1 black fallback image (no real wallpaper).
    if (m_thumb && out.imgW > 1 && out.imgH > 1) {
        const float ow = float(out.extent.width), oh = float(out.extent.height);
        float thh = m_thumbFrac * oh;
        float thw = thh * (float(out.imgW) / float(out.imgH));
        if (thw > 0.85f * ow) { thw = 0.85f * ow; thh = thw * (float(out.imgH) / float(out.imgW)); }
        const float tx = (ow - thw) * 0.5f;
        const float ty = m_thumbY * oh - thh * 0.5f;
        VkViewport tvp{ tx, ty, thw, thh, 0.0f, 1.0f };
        VkRect2D tsc{ { int32_t(tx), int32_t(ty) }, { uint32_t(thw), uint32_t(thh) } };
        vkCmdSetViewport(out.cmd, 0, 1, &tvp);
        vkCmdSetScissor(out.cmd, 0, 1, &tsc);
        vkCmdBindDescriptorSets(out.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeLayout,
            0, 1, &out.thumbDescriptor, 0, nullptr);
        vkCmdDraw(out.cmd, 3, 1, 0, 0);
    }

    // Overlay (clock/date/password): a full-screen alpha-blended quad. UI is
    // positioned within the canvas itself (clock top, password bottom).
    if (m_overlayReady && out.overlayPipeline && out.overlayDescriptor && out.overlayVbo) {
        float ovUbo[16] = { scale, sdr, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        std::memcpy(out.overlayUboMapped, ovUbo, sizeof(ovUbo));

        const float verts[16] = {
            -1.0f, -1.0f, 0.0f, 0.0f, // TL
             1.0f, -1.0f, 1.0f, 0.0f, // TR
            -1.0f,  1.0f, 0.0f, 1.0f, // BL
             1.0f,  1.0f, 1.0f, 1.0f, // BR
        };
        std::memcpy(out.overlayVboMapped, verts, sizeof(verts));

        vkCmdSetViewport(out.cmd, 0, 1, &vpt);
        vkCmdSetScissor(out.cmd, 0, 1, &sc);
        vkCmdBindPipeline(out.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, out.overlayPipeline);
        vkCmdBindDescriptorSets(out.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeLayout,
            0, 1, &out.overlayDescriptor, 0, nullptr);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(out.cmd, 0, 1, &out.overlayVbo, &off);
        vkCmdDraw(out.cmd, 4, 1, 0, 0);
    }

    vkCmdEndRenderPass(out.cmd);
    vkEndCommandBuffer(out.cmd);

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &out.acquireSem;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &out.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &out.renderSem;
    vkQueueSubmit(m_queue, 1, &si, out.inFlight);

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &out.renderSem;
    pi.swapchainCount = 1;
    pi.pSwapchains = &out.swapchain;
    pi.pImageIndices = &idx;
    vkQueuePresentKHR(m_queue, &pi);
}

void Renderer::destroyOutput(Output &out)
{
    if (!m_device)
        return;
    vkDeviceWaitIdle(m_device);
    if (out.inFlight) vkDestroyFence(m_device, out.inFlight, nullptr);
    if (out.acquireSem) vkDestroySemaphore(m_device, out.acquireSem, nullptr);
    if (out.renderSem) vkDestroySemaphore(m_device, out.renderSem, nullptr);
    if (out.cmd) vkFreeCommandBuffers(m_device, m_cmdPool, 1, &out.cmd);
    for (auto fb : out.framebuffers) vkDestroyFramebuffer(m_device, fb, nullptr);
    for (auto v : out.views) vkDestroyImageView(m_device, v, nullptr);
    if (out.swapchain) vkDestroySwapchainKHR(m_device, out.swapchain, nullptr);
    if (out.uboMapped) vkUnmapMemory(m_device, out.uboMem);
    if (out.ubo) vkDestroyBuffer(m_device, out.ubo, nullptr);
    if (out.uboMem) vkFreeMemory(m_device, out.uboMem, nullptr);
    if (out.thumbUboMapped) vkUnmapMemory(m_device, out.thumbUboMem);
    if (out.thumbUbo) vkDestroyBuffer(m_device, out.thumbUbo, nullptr);
    if (out.thumbUboMem) vkFreeMemory(m_device, out.thumbUboMem, nullptr);
    if (out.overlayUboMapped) vkUnmapMemory(m_device, out.overlayUboMem);
    if (out.overlayUbo) vkDestroyBuffer(m_device, out.overlayUbo, nullptr);
    if (out.overlayUboMem) vkFreeMemory(m_device, out.overlayUboMem, nullptr);
    if (out.overlayVboMapped) vkUnmapMemory(m_device, out.overlayVboMem);
    if (out.overlayVbo) vkDestroyBuffer(m_device, out.overlayVbo, nullptr);
    if (out.overlayVboMem) vkFreeMemory(m_device, out.overlayVboMem, nullptr);
    if (out.surface) vkDestroySurfaceKHR(m_instance, out.surface, nullptr);
    out = Output{};
}
