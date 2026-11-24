"""Regression tests for slot_resolve() format/storage/subresource/layout alias safety."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests"))
from nvn2_compute_c0_test import function


def _find_cc():
    if os.name == "nt":
        for candidate in (
            Path(os.environ.get("HWAOI_TOOLS", "F:")) / "llvm-mingw/bin/clang.exe",
            ROOT / "work/toolchain/llvm/bin/clang.exe",
        ):
            if candidate.is_file():
                return candidate
        found = shutil.which("clang")
        return Path(found) if found else Path(os.environ.get("HWAOI_TOOLS", "F:")) / "llvm-mingw/bin/clang.exe"
    found = shutil.which("clang")
    return Path(found) if found else Path("/usr/bin/clang")


def _find_deps():
    candidates = [
        Path("F:/TotKDependencies/runtime/deps/include"),
        ROOT / "work/toolchain/deps/include",
        Path("/usr/include"),
    ]
    for inc in candidates:
        if (inc / "vulkan/vulkan.h").is_file() and (
            (inc / "vma/vk_mem_alloc.h").is_file() or (inc / "vk_mem_alloc.h").is_file()
        ):
            return inc
    return candidates[0]


CC = _find_cc()
DEPS = _find_deps()


def typedef(text, name):
    end = text.index("} " + name + ";") + len("} " + name + ";")
    starts = list(re.finditer(r"typedef struct(?:\s+\w+)?\s*\{", text[:end]))
    return text[starts[-1].start():end]


def source():
    tex = (ROOT / "runtime/vk_texture.c").read_text(encoding="utf-8")
    draw = (ROOT / "runtime/vk_exec_draw.c").read_text(encoding="utf-8")
    start = draw.index("typedef struct {\n    VkImage image;\n    VkFormat format;")
    view_key = draw[start:draw.index("typedef struct { uint64_t addr, size; } Range;", start)]
    start = draw.index("typedef struct ViewNode")
    view_nodes = draw[start:draw.index("// An image view of a registered texture", start)]

    preamble = r'''
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tex.h"
#include "vk_exec.h"
#include "nvn_hmap.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static VkCtx context;
static unsigned entry_of_calls, view_creates;
static int astc_bc_on;
static VkImageViewCreateInfo last_view_ci;
VkCtx *vk_ctx(void) { return &context; }
const char *cfg_get(const char *k) { (void)k; return NULL; }
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_fatal(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); exit(73); }
int vk_astc_native(VkFormat f) { (void)f; return 0; }
int vk_astc_bc(void) { return astc_bc_on; }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice device, VkFormat f, VkFormatProperties *p) {
    (void)device; memset(p, 0, sizeof *p);
    p->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (f == VK_FORMAT_R8G8B8A8_UNORM) p->optimalTilingFeatures |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice device, const VkImageViewCreateInfo *ci,
    const VkAllocationCallbacks *allocator, VkImageView *view) {
    (void)device; (void)allocator;
    last_view_ci = *ci;
    *view = (VkImageView)(uintptr_t)(++view_creates);
    return VK_SUCCESS;
}
'''
    types = "\n".join(typedef(tex, n) for n in ("TexKey", "Tex", "TexSlot"))
    globals_and_stub = r'''
static nvn_hmap by_key = {.klen = sizeof(TexKey)};
static nvn_hmap by_addr = {.klen = 8};
static Tex *stub_created;
static Tex *entry_of(const tex_t *t) {
    entry_of_calls++;
    Tex *e = calloc(1, sizeof *e);
    e->t = *t;
    e->key.addr = (uintptr_t)t->mem;
    e->key.d = t->d;
    TexVkFormat f;
    CHECK(tex_vk_format(t->d.format, &f));
    e->pub.image = (VkImage)(uintptr_t)(0x9000 + entry_of_calls);
    e->pub.addr = e->key.addr;
    e->pub.format = f.vk;
    e->pub.type = VK_IMAGE_TYPE_2D;
    e->pub.view_type = VK_IMAGE_VIEW_TYPE_2D;
    e->pub.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    e->pub.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (f.vk == VK_FORMAT_R8G8B8A8_UNORM ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    e->pub.create_flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    e->pub.width = t->d.width; e->pub.height = t->d.height; e->pub.depth = 1;
    e->pub.levels = t->d.levels ? t->d.levels : 1; e->pub.layers = t->L.layers ? t->L.layers : 1; e->pub.samples = 1;
    e->pub.swz[0] = (uint8_t)t->d.swizzle_r; e->pub.swz[1] = (uint8_t)t->d.swizzle_g;
    e->pub.swz[2] = (uint8_t)t->d.swizzle_b; e->pub.swz[3] = (uint8_t)t->d.swizzle_a;
    stub_created = e;
    return e;
}
'''
    main = r'''
static Tex *make_tex(uint64_t addr, int fmt, uint64_t serial, int released, uintptr_t handle) {
    Tex *e = calloc(1, sizeof *e);
    e->t.mem = (void *)(uintptr_t)addr;
    e->t.serial = serial;
    e->t.d.format = fmt;
    e->t.d.target = NVN_TEXTURE_TARGET_2D;
    e->t.d.width = 256; e->t.d.height = 256; e->t.d.depth = 1; e->t.d.levels = 1; e->t.d.samples = 1;
    e->t.d.swizzle_r = 2; e->t.d.swizzle_g = 3; e->t.d.swizzle_b = 4; e->t.d.swizzle_a = 5;
    nvn_tex_layout(&e->t.d, &e->t.L);
    make_key(&e->t, &e->key);
    TexVkFormat f;
    CHECK(tex_vk_format(fmt, &f));
    e->pub.image = (VkImage)handle;
    e->pub.addr = addr;
    e->pub.format = f.vk;
    e->pub.type = VK_IMAGE_TYPE_2D;
    e->pub.view_type = VK_IMAGE_VIEW_TYPE_2D;
    e->pub.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    e->pub.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    e->pub.create_flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    e->pub.width = 256; e->pub.height = 256; e->pub.depth = 1;
    e->pub.levels = 1; e->pub.layers = 1; e->pub.samples = 1;
    e->pub.swz[0] = 2; e->pub.swz[1] = 3; e->pub.swz[2] = 4; e->pub.swz[3] = 5;
    e->released = released;
    return e;
}

static void insert_tex(Tex *e) {
    nvn_hm_put(&by_key, &e->key, e);
    e->next_at = nvn_hm_get(&by_addr, &e->key.addr);
    nvn_hm_put(&by_addr, &e->key.addr, e);
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    const char *mode = argv[1];
    uint64_t addr = 0x40000000ull;

    if (!strcmp(mode, "evicted-or-lazy-bc1-over-live-bc3")) {
        // Evicted or never-finalized lazy BC1 slot over live BC3 at the same address:
        // must NOT hijack BC3 and must materialize BC1 cleanly via entry_of().
        Tex *bc3 = make_tex(addr, NVN_FORMAT_RGBA_DXT5, 200, 0, 0x1370);
        insert_tex(bc3);
        TexSlot s = {0};
        s.t = make_tex(addr, NVN_FORMAT_RGBA_DXT1, 100, 0, 0)->t;
        s.used = 1; s.has_view = 1; s.v.set_format = 1; s.v.format = NVN_FORMAT_RGBA_DXT1;
        view_t out_v = {0}; int out_hv = 0;
        Tex *r = slot_resolve(&s, &out_v, &out_hv);
        CHECK(r == stub_created && entry_of_calls == 1 && r->pub.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
        CHECK(get_view(NULL, &r->pub, &out_v, out_hv, 0x102, 0) != VK_NULL_HANDLE);
        CHECK(last_view_ci.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
        puts("PASS evicted-or-lazy-bc1-over-live-bc3");
        return 0;
    }
    if (!strcmp(mode, "released-bc1-over-live-bc3")) {
        // Released BC1 still present alongside newer live BC3: keeps its own released BC1 entry and creates valid view.
        Tex *bc1 = make_tex(addr, NVN_FORMAT_RGBA_DXT1, 100, 1, 0x1330);
        Tex *bc3 = make_tex(addr, NVN_FORMAT_RGBA_DXT5, 200, 0, 0x1370);
        insert_tex(bc1);
        insert_tex(bc3);
        TexSlot s = {0};
        s.t = bc1->t; s.used = 1; s.has_view = 1; s.v.set_format = 1; s.v.format = NVN_FORMAT_RGBA_DXT1;
        view_t out_v = {0}; int out_hv = 0;
        Tex *r = slot_resolve(&s, &out_v, &out_hv);
        CHECK(r == bc1 && entry_of_calls == 0);
        CHECK(get_view(NULL, &r->pub, &out_v, out_hv, 0x102, 0) != VK_NULL_HANDLE);
        CHECK(last_view_ci.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
        puts("PASS released-bc1-over-live-bc3");
        return 0;
    }
    if (!strcmp(mode, "srgb-slot-no-view-follows-linear-preserves-srgb-and-swizzle")) {
        // Finalized RGBA8_SRGB slot with no explicit view_t (and custom swizzle, samples=0 default) following
        // live linear RGBA8 (samples=1): must preserve BOTH sRGB format (43, NOT 37) and slot swizzle.
        Tex *rt = make_tex(addr, NVN_FORMAT_RGBA8, 200, 0, 0x2500);
        rt->t.d.levels = 4; nvn_tex_layout(&rt->t.d, &rt->t.L); make_key(&rt->t, &rt->key); rt->pub.levels = 4;
        insert_tex(rt);
        TexSlot s = {0};
        s.t = make_tex(addr, NVN_FORMAT_RGBA8_SRGB, 100, 0, 0)->t;
        s.t.d.levels = 4;
        s.t.d.samples = 0;   // NVN default for single-sample textures
        s.t.d.swizzle_a = NVN_TEXTURE_SWIZZLE_ONE;
        nvn_tex_layout(&s.t.d, &s.t.L);
        s.used = 1; s.has_view = 0;
        view_t out_v = {0}; int out_hv = 0;
        Tex *r = slot_resolve(&s, &out_v, &out_hv);
        CHECK(r == rt && entry_of_calls == 0 && out_hv == 1 && out_v.set_format == 1);
        CHECK(out_v.format == NVN_FORMAT_RGBA8_SRGB && out_v.set_swizzle == 1 && out_v.swz[3] == NVN_TEXTURE_SWIZZLE_ONE);
        CHECK(get_view(NULL, &r->pub, &out_v, out_hv, 0x102, 0) != VK_NULL_HANDLE);
        CHECK(last_view_ci.format == VK_FORMAT_R8G8B8A8_SRGB);
        CHECK(last_view_ci.components.a == VK_COMPONENT_SWIZZLE_ONE);
        CHECK(last_view_ci.subresourceRange.baseMipLevel == 0 && last_view_ci.subresourceRange.levelCount == 4);
        // Partial view (set_levels=1, set_format=0) preserves levels alongside synthesized sRGB format + swizzle.
        s.has_view = 1; s.v.set_levels = 1; s.v.base_level = 1; s.v.nlevels = 2;
        out_v = (view_t){0}; out_hv = 0;
        r = slot_resolve(&s, &out_v, &out_hv);
        CHECK(r == rt && out_hv == 1 && out_v.set_format == 1 && out_v.format == NVN_FORMAT_RGBA8_SRGB);
        CHECK(out_v.set_levels == 1 && out_v.base_level == 1 && out_v.nlevels == 2 && out_v.set_swizzle == 1);
        CHECK(get_view(NULL, &r->pub, &out_v, out_hv, 0x102, 0) != VK_NULL_HANDLE);
        CHECK(last_view_ci.format == VK_FORMAT_R8G8B8A8_SRGB);
        CHECK(last_view_ci.subresourceRange.baseMipLevel == 1 && last_view_ci.subresourceRange.levelCount == 2);
        // 1-mip slot following 4-mip alias clamps implicit levelCount to 1 (not 4).
        s.t.d.levels = 1; nvn_tex_layout(&s.t.d, &s.t.L); s.has_view = 0; memset(&s.v, 0, sizeof s.v);
        out_v = (view_t){0}; out_hv = 0;
        r = slot_resolve(&s, &out_v, &out_hv);
        CHECK(r == rt && out_hv == 1 && out_v.set_levels == 1 && out_v.base_level == 0 && out_v.nlevels == 1);
        CHECK(get_view(NULL, &r->pub, &out_v, out_hv, 0x102, 0) != VK_NULL_HANDLE);
        CHECK(last_view_ci.format == VK_FORMAT_R8G8B8A8_SRGB && last_view_ci.subresourceRange.levelCount == 1);
        puts("PASS srgb-slot-no-view-follows-linear-preserves-srgb-and-swizzle");
        return 0;
    }
    if (!strcmp(mode, "storage-rgba8-skips-sampled-only-srgb")) {
        // Released RGBA8 storage texture alongside live sampled-only RGBA8_SRGB (no STORAGE_BIT):
        // must NOT follow sampled-only sRGB (which would fatal get_view(..., storage=1)); keeps own RGBA8 storage image.
        Tex *own_lin = make_tex(addr, NVN_FORMAT_RGBA8, 100, 1, 0x2501);
        own_lin->pub.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        Tex *srgb_no_stor = make_tex(addr, NVN_FORMAT_RGBA8_SRGB, 200, 0, 0x3801);
        srgb_no_stor->pub.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        insert_tex(own_lin);
        insert_tex(srgb_no_stor);
        TexSlot s = {0};
        s.t = own_lin->t; s.used = 1; s.has_view = 0;
        view_t out_v = {0}; int out_hv = 0;
        Tex *r = slot_resolve(&s, &out_v, &out_hv);
        CHECK(r == own_lin && entry_of_calls == 0);
        CHECK(get_view(NULL, &r->pub, &out_v, out_hv, 0x102, 1) != VK_NULL_HANDLE);
        CHECK(last_view_ci.format == VK_FORMAT_R8G8B8A8_UNORM);
        // When live sRGB image DOES carry STORAGE_BIT + EXTENDED_USAGE_BIT, aliasing succeeds for both storage and sampled.
        srgb_no_stor->pub.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        srgb_no_stor->pub.create_flags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        out_v = (view_t){0}; out_hv = 0;
        r = slot_resolve(&s, &out_v, &out_hv);
        CHECK(r == srgb_no_stor && out_hv == 1 && out_v.set_format == 1 && out_v.format == NVN_FORMAT_RGBA8);
        CHECK(get_view(NULL, &r->pub, &out_v, out_hv, 0x102, 1) != VK_NULL_HANDLE);
        CHECK(last_view_ci.format == VK_FORMAT_R8G8B8A8_UNORM);
        CHECK(get_view(NULL, &r->pub, &out_v, out_hv, 0x102, 0) != VK_NULL_HANDLE);
        CHECK(last_view_ci.format == VK_FORMAT_R8G8B8A8_UNORM);
        puts("PASS storage-rgba8-skips-sampled-only-srgb");
        return 0;
    }
    if (!strcmp(mode, "mip-layer-extent-coverage")) {
        // 4-mip slot requesting mip 2 must NOT follow a 1-mip live alias (which would yield VK_NULL_HANDLE / dummy).
        Tex *own_4mip = make_tex(addr, NVN_FORMAT_RGBA8, 100, 1, 0x4001);
        own_4mip->t.d.levels = 4; nvn_tex_layout(&own_4mip->t.d, &own_4mip->t.L); make_key(&own_4mip->t, &own_4mip->key); own_4mip->pub.levels = 4;
        Tex *live_1mip = make_tex(addr, NVN_FORMAT_RGBA8, 200, 0, 0x4002);
        live_1mip->t.d.levels = 1; nvn_tex_layout(&live_1mip->t.d, &live_1mip->t.L); make_key(&live_1mip->t, &live_1mip->key); live_1mip->pub.levels = 1;
        Tex *live_wrong_ext = make_tex(addr, NVN_FORMAT_RGBA8, 201, 0, 0x4003);
        live_wrong_ext->t.d.width = 128; live_wrong_ext->t.d.height = 128; live_wrong_ext->t.d.levels = 4;
        nvn_tex_layout(&live_wrong_ext->t.d, &live_wrong_ext->t.L);
        make_key(&live_wrong_ext->t, &live_wrong_ext->key);
        live_wrong_ext->pub.width = 128; live_wrong_ext->pub.height = 128; live_wrong_ext->pub.levels = 4;
        insert_tex(own_4mip);
        insert_tex(live_1mip);
        insert_tex(live_wrong_ext);
        TexSlot s = {0};
        s.t = own_4mip->t; s.used = 1; s.has_view = 1;
        s.v.set_levels = 1; s.v.base_level = 2; s.v.nlevels = 1;
        view_t out_v = {0}; int out_hv = 0;
        Tex *r = slot_resolve(&s, &out_v, &out_hv);
        CHECK(r == own_4mip && entry_of_calls == 0);
        CHECK(get_view(NULL, &r->pub, &out_v, out_hv, 0x102, 0) != VK_NULL_HANDLE);
        CHECK(last_view_ci.subresourceRange.baseMipLevel == 2 && last_view_ci.subresourceRange.levelCount == 1);
        // 6-layer 2D array slot requesting layers 3..4 must NOT follow a 1-layer 2D array alias.
        uint64_t addr2 = 0x50000000ull;
        Tex *live_1layer = make_tex(addr2, NVN_FORMAT_RGBA8, 300, 0, 0x5001);
        live_1layer->t.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; live_1layer->t.d.depth = 1;
        nvn_tex_layout(&live_1layer->t.d, &live_1layer->t.L);
        make_key(&live_1layer->t, &live_1layer->key);
        live_1layer->pub.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; live_1layer->pub.layers = 1;
        insert_tex(live_1layer);
        TexSlot s2 = {0};
        s2.t = make_tex(addr2, NVN_FORMAT_RGBA8, 250, 0, 0)->t;
        s2.t.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; s2.t.d.depth = 6;
        nvn_tex_layout(&s2.t.d, &s2.t.L);
        s2.used = 1; s2.has_view = 1; s2.v.set_layers = 1; s2.v.min_layer = 3; s2.v.nlayers = 2;
        out_v = (view_t){0}; out_hv = 0;
        r = slot_resolve(&s2, &out_v, &out_hv);
        CHECK(r == stub_created && entry_of_calls == 1 && r->pub.layers == 6);
        CHECK(get_view(NULL, &r->pub, &out_v, out_hv, 0x102, 0) != VK_NULL_HANDLE);
        CHECK(last_view_ci.subresourceRange.baseArrayLayer == 3 && last_view_ci.subresourceRange.layerCount == 2);
        puts("PASS mip-layer-extent-coverage");
        return 0;
    }
    if (!strcmp(mode, "array-layer-offset-and-tiling-layout")) {
        // Released 2-layer RGBA8 (1 mip, layer_size=0x40000) vs live 2-layer RGBA8 (4 mips, layer_size=0x56000):
        // Accessing layer 1 must NOT follow the 4-mip alias because layer 1 is at a different guest byte offset.
        Tex *own_2l_1m = make_tex(addr, NVN_FORMAT_RGBA8, 100, 1, 0x6001);
        own_2l_1m->t.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; own_2l_1m->t.d.depth = 2; own_2l_1m->t.d.levels = 1;
        nvn_tex_layout(&own_2l_1m->t.d, &own_2l_1m->t.L);
        make_key(&own_2l_1m->t, &own_2l_1m->key);
        own_2l_1m->pub.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; own_2l_1m->pub.layers = 2; own_2l_1m->pub.levels = 1;
        Tex *live_2l_4m = make_tex(addr, NVN_FORMAT_RGBA8, 200, 0, 0x6002);
        live_2l_4m->t.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; live_2l_4m->t.d.depth = 2; live_2l_4m->t.d.levels = 4;
        nvn_tex_layout(&live_2l_4m->t.d, &live_2l_4m->t.L);
        make_key(&live_2l_4m->t, &live_2l_4m->key);
        live_2l_4m->pub.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; live_2l_4m->pub.layers = 2; live_2l_4m->pub.levels = 4;
        CHECK(own_2l_1m->t.L.layer_size == 0x40000 && live_2l_4m->t.L.layer_size == 0x56000);
        insert_tex(own_2l_1m);
        insert_tex(live_2l_4m);
        TexSlot s = {0};
        s.t = own_2l_1m->t; s.used = 1; s.has_view = 0;
        view_t out_v = {0}; int out_hv = 0;
        Tex *r = slot_resolve(&s, &out_v, &out_hv);
        CHECK(r == own_2l_1m && entry_of_calls == 0);
        // Layer 1 explicitly requested must also stay on own_2l_1m.
        s.has_view = 1; s.v.set_layers = 1; s.v.min_layer = 1; s.v.nlayers = 1;
        out_v = (view_t){0}; out_hv = 0;
        r = slot_resolve(&s, &out_v, &out_hv);
        CHECK(r == own_2l_1m && entry_of_calls == 0);
        // Layer 0 only (offset 0 in both, same tileH/tileD) safely follows live_2l_4m with levelCount clamped to 1.
        s.v.min_layer = 0; s.v.nlayers = 1;
        out_v = (view_t){0}; out_hv = 0;
        r = slot_resolve(&s, &out_v, &out_hv);
        CHECK(r == live_2l_4m && out_hv == 1 && out_v.set_levels == 1 && out_v.base_level == 0 && out_v.nlevels == 1);
        CHECK(get_view(NULL, &r->pub, &out_v, out_hv, 0x102, 0) != VK_NULL_HANDLE);
        CHECK(last_view_ci.subresourceRange.levelCount == 1 && last_view_ci.subresourceRange.baseArrayLayer == 0 && last_view_ci.subresourceRange.layerCount == 1);
        // Matching 2-layer 4-mip sRGB slot (same layer_size=0x56000) follows live_2l_4m cleanly.
        TexSlot s_match = {0};
        s_match.t = live_2l_4m->t; s_match.t.d.format = NVN_FORMAT_RGBA8_SRGB;
        nvn_tex_layout(&s_match.t.d, &s_match.t.L);
        CHECK(s_match.t.L.layer_size == 0x56000);
        s_match.used = 1; s_match.has_view = 0;
        out_v = (view_t){0}; out_hv = 0;
        r = slot_resolve(&s_match, &out_v, &out_hv);
        CHECK(r == live_2l_4m && out_hv == 1 && out_v.set_format == 1 && out_v.format == NVN_FORMAT_RGBA8_SRGB);
        // Linear vs block-linear and MINIMAL_LAYOUT tileH mismatch at a second address must NOT alias.
        uint64_t addr2 = 0x60000000ull;
        Tex *live_lin = make_tex(addr2, NVN_FORMAT_RGBA8, 300, 0, 0x6010);
        live_lin->t.d.flags = 0x10; nvn_tex_layout(&live_lin->t.d, &live_lin->t.L); make_key(&live_lin->t, &live_lin->key);
        Tex *live_min = make_tex(addr2, NVN_FORMAT_RGBA8, 301, 0, 0x6011);
        live_min->t.d.flags = 0x200; live_min->t.d.height = 100;
        nvn_tex_layout(&live_min->t.d, &live_min->t.L); make_key(&live_min->t, &live_min->key); live_min->pub.height = 100;
        insert_tex(live_lin);
        insert_tex(live_min);
        TexSlot s_blk = {0};
        s_blk.t = make_tex(addr2, NVN_FORMAT_RGBA8, 250, 0, 0)->t;
        s_blk.t.d.height = 100; nvn_tex_layout(&s_blk.t.d, &s_blk.t.L);
        CHECK(live_min->t.L.tileH != s_blk.t.L.tileH);
        s_blk.used = 1; s_blk.has_view = 0;
        out_v = (view_t){0}; out_hv = 0;
        r = slot_resolve(&s_blk, &out_v, &out_hv);
        CHECK(r == stub_created && entry_of_calls == 1);
        puts("PASS array-layer-offset-and-tiling-layout");
        return 0;
    }
    if (!strcmp(mode, "astc-bc3-not-matched-by-bc3-slot")) {
        // Under astc_bc=1, an ASTC image is backed by VK_FORMAT_BC3_UNORM_BLOCK; a BC3/BC3_SRGB slot must not match it.
        astc_bc_on = 1;
        Tex *astc = make_tex(addr, NVN_FORMAT_RGBA_ASTC_4x4, 200, 0, 0x7900);
        CHECK(astc->pub.format == VK_FORMAT_BC3_UNORM_BLOCK);
        insert_tex(astc);
        TexSlot s = {0};
        s.t = make_tex(addr, NVN_FORMAT_RGBA_DXT5_SRGB, 100, 0, 0)->t;
        s.used = 1; s.has_view = 0;
        view_t out_v = {0}; int out_hv = 0;
        Tex *r = slot_resolve(&s, &out_v, &out_hv);
        CHECK(r == stub_created && entry_of_calls == 1 && r->pub.format == VK_FORMAT_BC3_SRGB_BLOCK);
        CHECK(get_view(NULL, &r->pub, &out_v, out_hv, 0x102, 0) != VK_NULL_HANDLE);
        CHECK(last_view_ci.format == VK_FORMAT_BC3_SRGB_BLOCK);
        puts("PASS astc-bc3-not-matched-by-bc3-slot");
        return 0;
    }
    if (!strcmp(mode, "bc3-unorm-to-srgb-accepted")) {
        // Supported BC3_UNORM -> BC3_SRGB direction follows live BC3_UNORM and creates a BC3_SRGB view.
        Tex *bc3 = make_tex(addr, NVN_FORMAT_RGBA_DXT5, 200, 0, 0x1370);
        insert_tex(bc3);
        TexSlot s = {0};
        s.t = make_tex(addr, NVN_FORMAT_RGBA_DXT5_SRGB, 100, 0, 0)->t;
        s.used = 1; s.has_view = 0;
        view_t out_v = {0}; int out_hv = 0;
        Tex *r = slot_resolve(&s, &out_v, &out_hv);
        CHECK(r == bc3 && entry_of_calls == 0 && out_hv == 1 && out_v.set_format == 1 && out_v.format == NVN_FORMAT_RGBA_DXT5_SRGB);
        CHECK(get_view(NULL, &r->pub, &out_v, out_hv, 0x102, 0) != VK_NULL_HANDLE);
        CHECK(last_view_ci.format == VK_FORMAT_BC3_SRGB_BLOCK);
        puts("PASS bc3-unorm-to-srgb-accepted");
        return 0;
    }
    if (!strcmp(mode, "bc3-srgb-with-unorm-view-still-fatals")) {
        // Unsupported reverse BC3_SRGB -> BC3_UNORM direction must NOT silently follow live BC3_UNORM.
        Tex *own_srgb = make_tex(addr, NVN_FORMAT_RGBA_DXT5_SRGB, 100, 1, 0x1380);
        Tex *live_unorm = make_tex(addr, NVN_FORMAT_RGBA_DXT5, 200, 0, 0x1370);
        insert_tex(own_srgb);
        insert_tex(live_unorm);
        TexSlot s = {0};
        s.t = own_srgb->t; s.used = 1; s.has_view = 1; s.v.set_format = 1; s.v.format = NVN_FORMAT_RGBA_DXT5;
        view_t out_v = {0}; int out_hv = 0;
        Tex *r = slot_resolve(&s, &out_v, &out_hv);
        CHECK(r == own_srgb && entry_of_calls == 0);
        (void)get_view(NULL, &r->pub, &out_v, out_hv, 0x102, 0);
        return 0;
    }
    if (!strcmp(mode, "swizzle-match-bc3-with-bc1-view-still-fatals")) {
        // Direct guest mismatch (BC3 slot with swizzle-only live BC3 match + explicit BC1 view):
        // same_but_swizzle must not accept incompatible view; direct mismatch still fails visibly in get_view().
        Tex *bc3 = make_tex(addr, NVN_FORMAT_RGBA_DXT5, 200, 0, 0x1370);
        insert_tex(bc3);
        TexSlot s = {0};
        s.t = make_tex(addr, NVN_FORMAT_RGBA_DXT5, 100, 0, 0)->t;
        s.t.d.swizzle_a = NVN_TEXTURE_SWIZZLE_ONE;
        s.used = 1; s.has_view = 1; s.v.set_format = 1; s.v.format = NVN_FORMAT_RGBA_DXT1;
        view_t out_v = {0}; int out_hv = 0;
        Tex *r = slot_resolve(&s, &out_v, &out_hv);
        CHECK(r == stub_created && entry_of_calls == 1);
        (void)get_view(NULL, &r->pub, &out_v, out_hv, 0x102, 0);
        return 0;
    }
    if (!strcmp(mode, "bc3-slot-with-bc1-view-skips-live-bc1-and-fatals")) {
        // Released 2-mip BC3 texture (mip 1 at 0x10000) with explicit BC1 view over live 2-mip BC1 (mip 1 at 0x8000):
        // Must NOT hijack live BC1 despite matching view format and tileH/tileD; keeps own BC3 and fails visibly in get_view().
        Tex *own_bc3 = make_tex(addr, NVN_FORMAT_RGBA_DXT5, 100, 1, 0x7001);
        own_bc3->t.d.levels = 2; nvn_tex_layout(&own_bc3->t.d, &own_bc3->t.L); make_key(&own_bc3->t, &own_bc3->key); own_bc3->pub.levels = 2;
        Tex *live_bc1 = make_tex(addr, NVN_FORMAT_RGBA_DXT1, 200, 0, 0x7002);
        live_bc1->t.d.levels = 2; nvn_tex_layout(&live_bc1->t.d, &live_bc1->t.L); make_key(&live_bc1->t, &live_bc1->key); live_bc1->pub.levels = 2;
        CHECK(nvn_tex_level_offset(&own_bc3->t.d, 1) == 0x10000 && nvn_tex_level_offset(&live_bc1->t.d, 1) == 0x8000);
        insert_tex(own_bc3);
        insert_tex(live_bc1);
        TexSlot s = {0};
        s.t = own_bc3->t; s.used = 1; s.has_view = 1; s.v.set_format = 1; s.v.format = NVN_FORMAT_RGBA_DXT1;
        view_t out_v = {0}; int out_hv = 0;
        Tex *r = slot_resolve(&s, &out_v, &out_hv);
        CHECK(r == own_bc3 && entry_of_calls == 0);
        (void)get_view(NULL, &r->pub, &out_v, out_hv, 0x102, 0);
        return 0;
    }
    if (!strcmp(mode, "direct-incompatible-still-fatals")) {
        Tex *bc3 = make_tex(addr, NVN_FORMAT_RGBA_DXT5, 200, 0, 0x1370);
        view_t bad = {.set_format = 1, .format = NVN_FORMAT_RGBA_DXT1};
        (void)get_view(NULL, &bc3->pub, &bad, 1, 0x102, 0);
        return 0;
    }
    return 2;
}
'''
    return (preamble + types + view_key + view_nodes + globals_and_stub +
            function(tex, "static void make_key(") + "\n" +
            function(tex, "static int same_but_swizzle(") + "\n" +
            function(tex, "static Tex *slot_resolve(") + "\n" +
            function(draw, "static VkImageViewType res_view_type(") + "\n" +
            function(draw, "static VkComponentSwizzle swz_of(") + "\n" +
            function(draw, "VkImageAspectFlags vk_exec_view_aspect(") + "\n" +
            function(draw, "static int sampled_format_compatible(") + "\n" +
            function(draw, "static VkImageView get_view(") + "\n" + main)


@unittest.skipUnless(
    CC.is_file() and (DEPS / "vulkan/vulkan.h").is_file() and
    ((DEPS / "vma/vk_mem_alloc.h").is_file() or (DEPS / "vk_mem_alloc.h").is_file()),
    "Clang and Vulkan/VMA headers required",
)
class SlotResolveFormatTests(unittest.TestCase):
    def test_slot_resolve_format_and_alias_safety(self):
        with tempfile.TemporaryDirectory(prefix="hwaoi-slot-resolve-") as tmp:
            folder = Path(tmp)
            src = folder / "fixture.c"
            src.write_text(source(), encoding="utf-8")
            includes = ["-I", str(ROOT / "runtime"), "-I", str(DEPS)]
            if (DEPS / "vma").is_dir():
                includes += ["-I", str(DEPS / "vma")]
            suffix = ".exe" if os.name == "nt" else ""
            for opt in ("-O0", "-O2"):
                exe = folder / f"fixture{opt}{suffix}"
                build = subprocess.run(
                    [str(CC), opt, "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                     "-march=x86-64-v3", *includes,
                     str(src), str(ROOT / "runtime/vk_formats.c"), str(ROOT / "runtime/nvn_tex.c"), "-o", str(exe)],
                    capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60,
                )
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                for case, expect_rc, expect_msg in (
                    ("evicted-or-lazy-bc1-over-live-bc3", 0, "PASS evicted-or-lazy-bc1-over-live-bc3"),
                    ("released-bc1-over-live-bc3", 0, "PASS released-bc1-over-live-bc3"),
                    ("srgb-slot-no-view-follows-linear-preserves-srgb-and-swizzle", 0, "PASS srgb-slot-no-view-follows-linear-preserves-srgb-and-swizzle"),
                    ("storage-rgba8-skips-sampled-only-srgb", 0, "PASS storage-rgba8-skips-sampled-only-srgb"),
                    ("mip-layer-extent-coverage", 0, "PASS mip-layer-extent-coverage"),
                    ("array-layer-offset-and-tiling-layout", 0, "PASS array-layer-offset-and-tiling-layout"),
                    ("astc-bc3-not-matched-by-bc3-slot", 0, "PASS astc-bc3-not-matched-by-bc3-slot"),
                    ("bc3-unorm-to-srgb-accepted", 0, "PASS bc3-unorm-to-srgb-accepted"),
                    ("bc3-srgb-with-unorm-view-still-fatals", 73, "vk: unsupported sampled view reinterpretation nvn=68 Vulkan 138 -> 137"),
                    ("swizzle-match-bc3-with-bc1-view-still-fatals", 73, "vk: unsupported sampled view reinterpretation nvn=66 Vulkan 137 -> 133"),
                    ("bc3-slot-with-bc1-view-skips-live-bc1-and-fatals", 73, "vk: unsupported sampled view reinterpretation nvn=66 Vulkan 137 -> 133"),
                    ("direct-incompatible-still-fatals", 73, "vk: unsupported sampled view reinterpretation nvn=66 Vulkan 137 -> 133"),
                ):
                    with self.subTest(opt=opt, case=case):
                        run = subprocess.run([str(exe), case], capture_output=True, text=True,
                                             encoding="utf-8", errors="replace", timeout=15)
                        self.assertEqual(run.returncode, expect_rc, run.stdout + run.stderr)
                        if expect_rc == 0:
                            self.assertIn(expect_msg, run.stdout)
                        else:
                            self.assertIn(expect_msg, run.stderr)
                            self.assertIn("addr=0x40000000", run.stderr)


if __name__ == "__main__":
    unittest.main()
