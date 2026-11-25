// Runs the in-game menu and its Vulkan backend over a synthetic image, with scripted input and PNG shots.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
extern "C" {
#include "cfg.h"
#include "input_map.h"
#include "platform/platform.h"
#include "ui/menu.h"
#include "ui_text.h"
}
#include "ui/option_rows.h"

extern unsigned preview_dpi;
extern bool preview_quit;
extern bool preview_restart;
extern uint64_t preview_buttons;
extern int32_t preview_lx, preview_ly;
extern int preview_capture_key;
extern int preview_capture_pad;

static void check(VkResult r, const char *what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + ": VkResult " + std::to_string(r));
}

static std::vector<std::string> conversions(const char *s) {
    std::vector<std::string> out;
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    for (size_t i = 0; s[i]; i++) {
        if (s[i] != '%') continue;
        size_t j = i + 1;
        if (!s[j]) continue;
        if (s[j] == '%') {
            out.emplace_back("%%");
            i = j;
            continue;
        }
        size_t pos = j;
        while (digit(s[pos])) pos++;
        if (s[pos] == '$') j = pos + 1;
        bool spaced = false;
        while (s[j] && strchr(" -+#0'", s[j])) { spaced |= s[j] == ' '; j++; }
        if (s[j] == '*') {
            j++;
            pos = j;
            while (digit(s[pos])) pos++;
            if (s[pos] == '$') j = pos + 1;
        } else while (digit(s[j])) j++;
        if (s[j] == '.') {
            j++;
            if (s[j] == '*') {
                j++;
                pos = j;
                while (digit(s[pos])) pos++;
                if (s[pos] == '$') j = pos + 1;
            } else while (digit(s[j])) j++;
        }
        while (s[j] && strchr("hljztL", s[j])) j++;
        if (s[j] && strchr("diuoxXfFeEgGaAcspn", s[j]) && !(spaced && letter(s[j + 1]))) {
            out.emplace_back(s + i, j - i + 1);
            i = j;
        }
    }
    return out;
}

static void text_check() {
    const std::vector<std::string> expected = {"%s", "%d", "%.1f", "%%"};
    if (conversions("100 % larger: %s %d %.1f%%") != expected ||
        conversions("%s %d") == conversions("%d %s") ||
        conversions("% s") != std::vector<std::string>{"% s"} ||
        conversions("%2$s") != std::vector<std::string>{"%2$s"})
        throw std::runtime_error("text: printf parser self-check failed");
    for (const char *lang : {"it", "ru", "zh-Hans"}) {
        int checked = 0, gaps = 0;
        for (int i = 0; i < UI_TEXT_COUNT; i++) {
            UiTextId id = (UiTextId)i;
            if (!ui_text_defined("en", id)) continue;
            if (!ui_text_defined(lang, id)) {
                if (!strcmp(lang, "ru") && id == UI_MENU_OV_UNMEASURED) {
                    fprintf(stderr, "text: pre-existing ru gap UI_MENU_OV_UNMEASURED (%d)\n", i);
                    gaps++;
                    continue;
                }
                throw std::runtime_error(std::string("text: missing ") + lang + " id " + std::to_string(i));
            }
            if (conversions(ui_text_in("en", id)) != conversions(ui_text_in(lang, id)))
                throw std::runtime_error(std::string("text: printf mismatch in ") + lang + " id " + std::to_string(i));
            checked++;
        }
        fprintf(stderr, "text: %s %d entries checked, %d allowed gaps\n", lang, checked, gaps);
    }
    std::string previous = cfg_value("TOTK_MENU_LANG");
    for (const char *lang : {"it", "ru", "en"}) {
        cfg_set("TOTK_MENU_LANG", lang);
        if (strcmp(menu_text(UI_MENU_CONTINUE), ui_text_in(lang, UI_MENU_CONTINUE)))
            throw std::runtime_error(std::string("text: live menu language did not change to ") + lang);
    }
    cfg_set("TOTK_MENU_LANG", previous.c_str());
}

static uint32_t crc(const unsigned char *p, size_t n) {
    uint32_t c = ~0u;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int j = 0; j < 8; j++) c = (c >> 1) ^ (0xedb88320u & -(c & 1u));
    }
    return ~c;
}

static void be32(std::vector<unsigned char> &v, uint32_t n) {
    v.push_back(n >> 24); v.push_back(n >> 16); v.push_back(n >> 8); v.push_back(n);
}

static void chunk(std::vector<unsigned char> &png, const char *type, const std::vector<unsigned char> &data) {
    be32(png, (uint32_t)data.size());
    size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    be32(png, crc(png.data() + start, png.size() - start));
}

static void png_write(const std::filesystem::path &path, const unsigned char *bgra, unsigned w, unsigned h) {
    std::vector<unsigned char> raw;
    raw.reserve((size_t)(w * 4 + 1) * h);
    for (unsigned y = 0; y < h; y++) {
        raw.push_back(0);
        for (unsigned x = 0; x < w; x++) {
            const unsigned char *p = bgra + ((size_t)y * w + x) * 4;
            raw.push_back(p[2]); raw.push_back(p[1]); raw.push_back(p[0]); raw.push_back(p[3]);
        }
    }
    std::vector<unsigned char> z = {0x78, 0x01};
    for (size_t pos = 0; pos < raw.size();) {
        unsigned n = (unsigned)std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back(n); z.push_back(n >> 8);
        unsigned inv = (~n) & 65535;
        z.push_back(inv); z.push_back(inv >> 8);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    be32(z, (b << 16) | a);
    std::vector<unsigned char> png = {137, 80, 78, 71, 13, 10, 26, 10}, hdr;
    be32(hdr, w); be32(hdr, h);
    hdr.insert(hdr.end(), {8, 6, 0, 0, 0});
    chunk(png, "IHDR", hdr); chunk(png, "IDAT", z); chunk(png, "IEND", {});
    std::ofstream f(path, std::ios::binary);
    f.write((const char *)png.data(), png.size());
    if (!f) throw std::runtime_error("cannot write PNG: " + path.string());
}

class Preview {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory buffer_mem = VK_NULL_HANDLE;
    unsigned char *mapped = nullptr;
    std::vector<unsigned char> background;
    std::vector<unsigned char> last_shot;
    unsigned width = 0, height = 0;
    uint64_t frame_no = 0;
    std::filesystem::path out;

    uint32_t memory_type(uint32_t mask, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties p;
        vkGetPhysicalDeviceMemoryProperties(phys, &p);
        for (uint32_t i = 0; i < p.memoryTypeCount; i++)
            if ((mask & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("no compatible Vulkan memory type");
    }

    void barrier(VkImageLayout old, VkImageLayout next, VkAccessFlags src, VkAccessFlags dst,
                 VkPipelineStageFlags from, VkPipelineStageFlags to) {
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = old; b.newLayout = next; b.srcAccessMask = src; b.dstAccessMask = dst;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, from, to, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    void target_down() {
        if (mapped) vkUnmapMemory(device, buffer_mem);
        if (buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (buffer_mem) vkFreeMemory(device, buffer_mem, nullptr);
        if (view) vkDestroyImageView(device, view, nullptr);
        if (image) vkDestroyImage(device, image, nullptr);
        if (image_mem) vkFreeMemory(device, image_mem, nullptr);
        mapped = nullptr; buffer = VK_NULL_HANDLE; buffer_mem = VK_NULL_HANDLE;
        view = VK_NULL_HANDLE; image = VK_NULL_HANDLE; image_mem = VK_NULL_HANDLE;
    }

public:
    Preview(int gpu, const std::filesystem::path &dir) : out(dir) {
        std::filesystem::create_directories(out);
        VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Menu preview"; app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ic = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ic.pApplicationInfo = &app;
        check(vkCreateInstance(&ic, nullptr, &instance), "vkCreateInstance");
        uint32_t n = 0;
        check(vkEnumeratePhysicalDevices(instance, &n, nullptr), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> devices(n);
        check(vkEnumeratePhysicalDevices(instance, &n, devices.data()), "vkEnumeratePhysicalDevices");
        for (uint32_t i = 0; i < n; i++) {
            if (gpu >= 0 && (unsigned)gpu != i) continue;
            VkPhysicalDeviceProperties prop;
            vkGetPhysicalDeviceProperties(devices[i], &prop);
            VkPhysicalDeviceDynamicRenderingFeatures dynamic = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
            VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &dynamic;
            vkGetPhysicalDeviceFeatures2(devices[i], &features);
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qn, nullptr);
            std::vector<VkQueueFamilyProperties> q(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qn, q.data());
            if (prop.apiVersion < VK_API_VERSION_1_3 || !dynamic.dynamicRendering) continue;
            for (uint32_t j = 0; j < qn; j++) if (q[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                phys = devices[i]; family = j;
                fprintf(stderr, "GPU %u: %s\n", i, prop.deviceName);
                break;
            }
            if (phys) break;
        }
        if (!phys) throw std::runtime_error("no Vulkan 1.3 graphics device with dynamic rendering");
        float priority = 1;
        VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
        VkPhysicalDeviceDynamicRenderingFeatures dynamic = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
        dynamic.dynamicRendering = VK_TRUE;
        VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.pNext = &dynamic; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
        check(vkCreateDevice(phys, &di, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = family; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(device, &pi, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &ca, &cb), "vkAllocateCommandBuffers");
    }
    ~Preview() {
        if (device) {
            vkDeviceWaitIdle(device);
            menu_vk_release();
            target_down();
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
    void size(unsigned w, unsigned h) {
        if (!w || !h || w > 8192 || h > 8192) throw std::runtime_error("invalid size");
        check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
        target_down();
        width = w; height = h;
        VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_B8G8R8A8_UNORM;
        ii.extent = {w, h, 1}; ii.mipLevels = ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        check(vkCreateImage(device, &ii, nullptr, &image), "vkCreateImage");
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, image, &req);
        VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ma.allocationSize = req.size; ma.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device, &ma, nullptr, &image_mem), "vkAllocateMemory image");
        check(vkBindImageMemory(device, image, image_mem, 0), "vkBindImageMemory");
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ii.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device, &vi, nullptr, &view), "vkCreateImageView");
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = (VkDeviceSize)w * h * 4;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        check(vkCreateBuffer(device, &bi, nullptr, &buffer), "vkCreateBuffer");
        vkGetBufferMemoryRequirements(device, buffer, &req);
        ma.allocationSize = req.size;
        ma.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device, &ma, nullptr, &buffer_mem), "vkAllocateMemory buffer");
        check(vkBindBufferMemory(device, buffer, buffer_mem, 0), "vkBindBufferMemory");
        check(vkMapMemory(device, buffer_mem, 0, VK_WHOLE_SIZE, 0, (void **)&mapped), "vkMapMemory");
        background.resize((size_t)w * h * 4);
        for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) {
            float fx = (float)x / w, fy = (float)y / h;
            float disc = expf(-40 * ((fx - .72f) * (fx - .72f) + (fy - .30f) * (fy - .30f)));
            float stripe = 0.5f + 0.5f * sinf(fx * 15 + fy * 7);
            float grid = (x % std::max(1u, w / 24) < 2 || y % std::max(1u, h / 14) < 2) ? 7.f : 0.f;
            unsigned char *p = &background[((size_t)y * w + x) * 4];
            p[0] = (unsigned char)(55 + fy * 37 + disc * 26 + grid);
            p[1] = (unsigned char)(42 + fy * 70 + disc * 32 + grid);
            p[2] = (unsigned char)(23 + fy * 19 + stripe * 8 + disc * 28 + grid);
            p[3] = 255;
        }
    }
    void frame(const std::string &shot_name = "") {
        memcpy(mapped, background.data(), background.size());
        MenuTarget t = {instance, phys, device, family, queue, VK_FORMAT_B8G8R8A8_UNORM,
                        width, height, (int)width, (int)height, 0};
        const char *hook_shot = nullptr;
        int overlay = menu_overlay_frame(++frame_no, &t, &hook_shot);
        std::string shot = shot_name.empty() ? (hook_shot ? hook_shot : "") : (out / (shot_name + ".png")).string();
        check(vkResetCommandBuffer(cb, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(cb, &begin), "vkBeginCommandBuffer");
        barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cb, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        if (overlay) {
            barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            att.imageView = view; att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea.extent = {width, height}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &att;
            vkCmdBeginRendering(cb, &ri);
            menu_overlay_draw(cb);
            vkCmdEndRendering(cb);
        }
        if (!shot.empty()) {
            barrier(overlay ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, overlay ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, overlay ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferMemoryBarrier bb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            bb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; bb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb.buffer = buffer; bb.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &bb, 0, nullptr);
            vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        }
        check(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        check(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
        check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");
        if (!shot.empty()) {
            last_shot.assign(mapped, mapped + background.size());
            png_write(shot, mapped, width, height);
            fprintf(stderr, "shot: %s\n", shot.c_str());
        }
    }
    void frames(int n) { if (n < 0 || n > 100000) throw std::runtime_error("invalid frame count"); while (n--) frame(); }
    float coord(const std::string &s, bool horizontal) const {
        float x = std::stof(s);
        return !s.empty() && s.back() == '%' ? x * (horizontal ? width : height) / 100 : x;
    }
    void expect_color(unsigned rgb, int tol, size_t minimum) const {
        if (last_shot.empty()) throw std::runtime_error("expect-color needs a previous shot");
        size_t count = 0;
        for (size_t i = 0; i < last_shot.size(); i += 4) {
            int r = last_shot[i + 2], g = last_shot[i + 1], b = last_shot[i];
            if (abs(r - (int)((rgb >> 16) & 255)) <= tol && abs(g - (int)((rgb >> 8) & 255)) <= tol &&
                abs(b - (int)(rgb & 255)) <= tol) count++;
        }
        if (count < minimum)
            throw std::runtime_error("expect-color: found " + std::to_string(count) + ", expected at least " +
                                     std::to_string(minimum));
    }
};

static std::pair<unsigned, unsigned> parse_size(const std::string &s) {
    unsigned w = 0, h = 0;
    char tail;
    if (sscanf(s.c_str(), "%ux%u%c", &w, &h, &tail) != 2) throw std::runtime_error("expected WxH");
    return {w, h};
}

static ImGuiKey key_of(const std::string &s) {
    static const std::map<std::string, ImGuiKey> names = {
        {"DownArrow", ImGuiKey_DownArrow}, {"UpArrow", ImGuiKey_UpArrow},
        {"LeftArrow", ImGuiKey_LeftArrow}, {"RightArrow", ImGuiKey_RightArrow},
        {"Escape", ImGuiKey_Escape}, {"Enter", ImGuiKey_Enter}, {"Space", ImGuiKey_Space},
        {"Tab", ImGuiKey_Tab}, {"Backspace", ImGuiKey_Backspace}, {"Delete", ImGuiKey_Delete},
        {"F1", ImGuiKey_F1}, {"F10", ImGuiKey_F10}, {"F11", ImGuiKey_F11},
    };
    if (s.size() == 1 && s[0] >= 'A' && s[0] <= 'Z') return (ImGuiKey)(ImGuiKey_A + s[0] - 'A');
    if (s.size() == 1 && s[0] >= '0' && s[0] <= '9') return (ImGuiKey)(ImGuiKey_0 + s[0] - '0');
    auto i = names.find(s);
    if (i != names.end()) return i->second;
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++)
        if (s == ImGui::GetKeyName((ImGuiKey)k)) return (ImGuiKey)k;
    throw std::runtime_error("unknown ImGui key: " + s);
}

static int capture_key(ImGuiKey k) {
    if (k >= ImGuiKey_A && k <= ImGuiKey_Z) return KEY_A + (int)k - ImGuiKey_A;
    if (k >= ImGuiKey_0 && k <= ImGuiKey_9) return KEY_0 + (int)k - ImGuiKey_0;
    switch (k) {
    case ImGuiKey_Escape: return KEY_ESCAPE;
    case ImGuiKey_Enter: return KEY_ENTER;
    case ImGuiKey_Space: return KEY_SPACE;
    case ImGuiKey_Tab: return KEY_TAB;
    case ImGuiKey_DownArrow: return KEY_DOWN;
    case ImGuiKey_UpArrow: return KEY_UP;
    case ImGuiKey_LeftArrow: return KEY_LEFT;
    case ImGuiKey_RightArrow: return KEY_RIGHT;
    case ImGuiKey_Delete: return KEY_DELETE;
    case ImGuiKey_Backspace: return KEY_BACKSPACE;
    case ImGuiKey_LeftCtrl: return KEY_LCTRL;
    case ImGuiKey_RightCtrl: return KEY_RCTRL;
    case ImGuiKey_LeftShift: return KEY_LSHIFT;
    case ImGuiKey_RightShift: return KEY_RSHIFT;
    case ImGuiKey_LeftAlt: return KEY_LALT;
    case ImGuiKey_Insert: return KEY_INSERT;
    case ImGuiKey_Home: return KEY_HOME;
    case ImGuiKey_End: return KEY_END;
    case ImGuiKey_PageUp: return KEY_PAGEUP;
    case ImGuiKey_PageDown: return KEY_PAGEDOWN;
    case ImGuiKey_F9: return KEY_F9;
    case ImGuiKey_F12: return KEY_F12;
    case ImGuiKey_Minus: return KEY_MINUS;
    case ImGuiKey_Equal: return KEY_EQUALS;
    case ImGuiKey_LeftBracket: return KEY_LBRACKET;
    case ImGuiKey_RightBracket: return KEY_RBRACKET;
    case ImGuiKey_Semicolon: return KEY_SEMICOLON;
    case ImGuiKey_Apostrophe: return KEY_APOSTROPHE;
    case ImGuiKey_Comma: return KEY_COMMA;
    case ImGuiKey_Period: return KEY_PERIOD;
    case ImGuiKey_Slash: return KEY_SLASH;
    case ImGuiKey_Backslash: return KEY_BACKSLASH;
    case ImGuiKey_GraveAccent: return KEY_GRAVE;
    default:
        if (k >= ImGuiKey_F2 && k <= ImGuiKey_F7) return KEY_F2 + (int)k - ImGuiKey_F2;
        if (k >= ImGuiKey_Keypad0 && k <= ImGuiKey_Keypad9) return KEY_NUM0 + (int)k - ImGuiKey_Keypad0;
        return 0;
    }
}

static int capture_pad(int b) {
    switch (b) {
    case B_A: return PAD_EAST;
    case B_B: return PAD_SOUTH;
    case B_X: return PAD_NORTH;
    case B_Y: return PAD_WEST;
    case B_L: return PAD_L;
    case B_R: return PAD_R;
    case B_ZL: return PAD_ZL;
    case B_ZR: return PAD_ZR;
    case B_PLUS: return PAD_START;
    case B_MINUS: return PAD_BACK;
    case B_UP: return PAD_DPAD_UP;
    case B_DOWN: return PAD_DPAD_DOWN;
    case B_LEFT: return PAD_DPAD_LEFT;
    case B_RIGHT: return PAD_DPAD_RIGHT;
    default: return PAD_NONE;
    }
}

static int pad_of(const std::string &s) {
    static const std::map<std::string, int> names = {
        {"A", B_A}, {"B", B_B}, {"X", B_X}, {"Y", B_Y}, {"L", B_L}, {"R", B_R},
        {"ZL", B_ZL}, {"ZR", B_ZR}, {"UP", B_UP}, {"DOWN", B_DOWN}, {"LEFT", B_LEFT},
        {"RIGHT", B_RIGHT}, {"PLUS", B_PLUS}, {"MINUS", B_MINUS},
    };
    auto i = names.find(s);
    if (i == names.end()) throw std::runtime_error("unknown pad button: " + s);
    return i->second;
}

static std::pair<std::string, std::string> assignment(const std::string &s) {
    size_t pos = s.find('=');
    if (pos == std::string::npos || pos == 0) throw std::runtime_error("expected KEY=VALUE");
    std::string key = s.substr(0, pos);
    if (key.compare(0, 5, "TOTK_") != 0) key = "TOTK_" + key;
    return {key, s.substr(pos + 1)};
}

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static bool ini_has(const std::string &key, const std::string &value) {
    char exe[1024];
    if (!plat_exe_path(exe, sizeof exe)) throw std::runtime_error("cannot find executable path");
    std::filesystem::path p = std::filesystem::path(exe).parent_path() / "hwaoi.ini";
    std::ifstream f(p);
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return (char)toupper(c); });
        if (k == key.substr(5) && trim(line.substr(eq + 1)) == value) return true;
    }
    return false;
}

static void run_script(Preview &p, const std::filesystem::path &path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open script: " + path.string());
    std::string line;
    int line_no = 0;
    while (std::getline(f, line)) {
        line_no++;
        try {
            size_t hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            std::istringstream in(line);
            std::string cmd, a, b;
            if (!(in >> cmd)) continue;
            if (cmd == "frames") {
                int n; if (!(in >> n)) throw std::runtime_error("expected frame count"); p.frames(n);
            } else if (cmd == "sleep") {
                int ms; if (!(in >> ms) || ms < 0 || ms > 60000) throw std::runtime_error("invalid sleep");
                auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
                while (std::chrono::steady_clock::now() < end) {
                    p.frame();
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
            } else if (cmd == "open" || cmd == "close") {
                if (menu_is_open() != (cmd == "open")) menu_toggle();
                p.frames(3);
                if (cmd == "open") {
                    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(160);
                    while (std::chrono::steady_clock::now() < end) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(16));
                        p.frame();
                    }
                }
            } else if (cmd == "key") {
                if (!(in >> a >> b) || (b != "tap" && b != "down" && b != "up")) throw std::runtime_error("expected key NAME tap|down|up");
                ImGuiKey k = key_of(a);
                auto event = [&](bool down) {
                    if (k == ImGuiKey_F10 && down) menu_perf_cycle();
                    else menu_event_key(k, down);
                    if (down && menu_binding_capture()) preview_capture_key = capture_key(k);
                    p.frame();
                };
                if (b != "up") event(true);
                if (b != "down") event(false);
                if (b == "tap") p.frames(2);
            } else if (cmd == "pad" || cmd == "padtap") {
                if (!(in >> a)) throw std::runtime_error("expected pad button");
                uint64_t old = preview_buttons;
                preview_buttons = 0;
                if (a != "none") {
                    preview_buttons |= 1ull << pad_of(a);
                    while (in >> a) preview_buttons |= 1ull << pad_of(a);
                }
                if (menu_binding_capture()) {
                    uint64_t pressed = preview_buttons & ~old;
                    for (int bit = 0; bit < 16; bit++) if (pressed & (1ull << bit)) {
                        preview_capture_pad = capture_pad(bit);
                        break;
                    }
                }
                p.frame();
                if (cmd == "padtap") { preview_buttons = 0; p.frames(3); }
            } else if (cmd == "stick") {
                float x, y;
                if (!(in >> x >> y) || x < -1 || x > 1 || y < -1 || y > 1) throw std::runtime_error("expected stick X Y in -1..1");
                preview_lx = (int32_t)(x * 32767); preview_ly = (int32_t)(y * 32767); p.frame();
            } else if (cmd == "mouse" || cmd == "click") {
                if (in >> a) {
                    if (!(in >> b)) throw std::runtime_error("expected X Y");
                    menu_event_mouse_pos(p.coord(a, true), p.coord(b, false));
                    p.frames(2);
                } else if (cmd == "mouse") throw std::runtime_error("expected X Y");
                if (cmd == "click") {
                    if (menu_binding_capture()) preview_capture_key = KEY_MOUSE_L;
                    menu_event_mouse_button(0, 1); p.frame();
                    menu_event_mouse_button(0, 0); p.frames(3);
                }
            } else if (cmd == "wheel") {
                float dy; if (!(in >> dy)) throw std::runtime_error("expected wheel DY");
                menu_event_wheel(0, dy); p.frames(3);
            } else if (cmd == "drag") {
                std::string x0, y0, x1, y1;
                if (!(in >> x0 >> y0 >> x1 >> y1)) throw std::runtime_error("expected drag X0 Y0 X1 Y1");
                float ax = p.coord(x0, true), ay = p.coord(y0, false);
                float bx = p.coord(x1, true), by = p.coord(y1, false);
                menu_event_mouse_pos(ax, ay); p.frames(2);
                menu_event_mouse_button(0, 1); p.frame();
                for (int i = 1; i <= 6; i++) {
                    float t = i / 6.0f;
                    menu_event_mouse_pos(ax + (bx - ax) * t, ay + (by - ay) * t);
                    p.frame();
                }
                menu_event_mouse_button(0, 0); p.frames(3);
            } else if (cmd == "shot") {
                if (!(in >> a) || a.find_first_of("/\\:") != std::string::npos || a == "." || a == "..")
                    throw std::runtime_error("expected shot name");
                p.frame(a);
            } else if (cmd == "text-check") {
                text_check();
            } else if (cmd == "expect-color") {
                unsigned rgb; int tol; size_t minimum;
                if (!(in >> a >> tol >> minimum) || a.size() != 6 || tol < 0 || tol > 255)
                    throw std::runtime_error("expected RRGGBB TOL MIN");
                size_t used = 0;
                rgb = std::stoul(a, &used, 16);
                if (used != 6) throw std::runtime_error("invalid RRGGBB");
                p.expect_color(rgb, tol, minimum);
            } else if (cmd == "set" || cmd == "expect" || cmd == "expect-ini") {
                std::getline(in, a);
                a = trim(a);
                if (a.empty()) throw std::runtime_error("expected KEY=VALUE");
                auto [key, value] = assignment(a);
                if (cmd == "set") cfg_set(key.c_str(), value.c_str());
                else if (cmd == "expect") {
                    const char *got = cfg_value(key.c_str());
                    if (!got || value != got) throw std::runtime_error(key + ": expected " + value + ", got " + (got ? got : "(null)"));
                } else if (!ini_has(key, value)) throw std::runtime_error("ini missing " + key.substr(5) + "=" + value);
            } else if (cmd == "expect-env" || cmd == "expect-env-name") {
                std::getline(in, a);
                auto [key, value] = assignment(trim(a));
                char name[96];
                if (!cfg_env_name(key.c_str(), name, sizeof name) ||
                    value != (cmd == "expect-env" ? cfg_value(key.c_str()) : name))
                    throw std::runtime_error(key + ": unexpected environment override " + value);
            } else if (cmd == "expect-open" || cmd == "expect-quit" || cmd == "expect-capture" ||
                       cmd == "expect-popup" || cmd == "expect-status" || cmd == "expect-restart") {
                int expected;
                if (!(in >> expected) || expected < 0 || expected > (cmd == "expect-status" ? 3 : 1))
                    throw std::runtime_error("invalid expected state");
                int actual = cmd == "expect-open" ? menu_is_open()
                           : cmd == "expect-quit" ? preview_quit
                           : cmd == "expect-restart" ? preview_restart
                           : cmd == "expect-capture" ? menu_binding_capture()
                           : cmd == "expect-popup" ? ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                                                                 ImGuiPopupFlags_AnyPopupLevel)
                           : opt_status();
                if (actual != expected) throw std::runtime_error(cmd + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
            } else if (cmd == "size") {
                if (!(in >> a)) throw std::runtime_error("expected WxH");
                auto [w, h] = parse_size(a); p.size(w, h); p.frames(3);
            } else if (cmd == "lang") {
                if (!(in >> a) || (a != "en" && a != "ru" && a != "it")) throw std::runtime_error("expected en|ru|it");
                cfg_set("TOTK_MENU_LANG", a.c_str()); p.frames(3);
            } else throw std::runtime_error("unknown command: " + cmd);
        } catch (const std::exception &e) {
            throw std::runtime_error(path.string() + ":" + std::to_string(line_no) + ": " + e.what());
        }
    }
}

int main(int argc, char **argv) {
    try {
        std::string size = "1920x1080", lang = "en", script;
        std::filesystem::path out = ".";
        int gpu = -1;
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + arg);
            const char *value = argv[++i];
            if (arg == "--size") size = value;
            else if (arg == "--dpi") { int d = std::stoi(value); if (d < 1 || d > 1000) throw std::runtime_error("invalid dpi"); preview_dpi = d; }
            else if (arg == "--lang") lang = value;
            else if (arg == "--script") script = value;
            else if (arg == "--out") out = value;
            else if (arg == "--gpu") { gpu = std::stoi(value); if (gpu < 0) throw std::runtime_error("invalid gpu"); }
            else throw std::runtime_error("unknown argument " + arg);
        }
        if (script.empty() || (lang != "en" && lang != "ru" && lang != "it")) throw std::runtime_error("pass --script and --lang en|ru|it");
        cfg_init();
        cfg_set("TOTK_MENU_LANG", lang.c_str());
        input_map_configure();
        auto [w, h] = parse_size(size);
        Preview p(gpu, out);
        p.size(w, h);
        run_script(p, script);
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "menu preview: %s\n", e.what());
        return 1;
    }
}
