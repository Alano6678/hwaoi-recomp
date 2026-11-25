"""Numeric SPIR-V resource reflection and bit-preserving buffer-view policy."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
CC=Path(os.environ.get('HWAOI_TOOLS','F:')+'/llvm-mingw/bin/clang.exe')
DEPS=Path(os.environ.get('HWAOI_TEST_DEPS') or 'F:/TotKDependencies/runtime/deps')
GLSL=Path('F:/GCC/ucrt64/bin/glslangValidator.exe')

SOURCE=r'''
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "gfx.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static int failed;
static void *fail_calloc(size_t n,size_t size){if(failed&&!--failed)return NULL;return calloc(n,size);}
#define calloc fail_calloc
#include "gfx_texel_reflect.h"
#undef calloc
#include "vk_texel_policy.h"
static unsigned checks;
static void reflection(void){
 uint32_t spv[]={0x07230203,0x00010300,0,32,0,
  (4u<<16)|71,9,33,3, (4u<<16)|71,9,34,1,
  (3u<<16)|22,1,32, (9u<<16)|25,2,1,5,0,0,0,1,0,
  (3u<<16)|27,3,2, (4u<<16)|28,4,3,20, (4u<<16)|32,5,0,4,
  (4u<<16)|59,5,9,0};
 GfxShaderRes resource={.set=1,.binding=3,.type=2};
 CHECK(gfx_reflect_texel_types(spv,sizeof spv/4,&resource,1)&&resource.sampled_type==GFX_SAMPLED_FLOAT);checks++;
 for(unsigned fail=1;fail<=3;fail++){failed=(int)fail;resource.sampled_type=0;CHECK(!gfx_reflect_texel_types(spv,sizeof spv/4,&resource,1));}failed=0;
 // The same original descriptor chain, with independently authored signed and
 // unsigned scalar types. No translator reflection/format guess supplies it.
 uint32_t integer[64];memcpy(integer,spv,13*4);integer[13]=(4u<<16)|21;integer[14]=1;integer[15]=32;
 for(unsigned signedness=0;signedness<2;signedness++){
  integer[16]=signedness;memcpy(integer+17,spv+16,(sizeof spv/4-16)*4);resource.sampled_type=0;
  CHECK(gfx_reflect_texel_types(integer,sizeof spv/4+1,&resource,1));
  CHECK(resource.sampled_type==(signedness?GFX_SAMPLED_SINT:GFX_SAMPLED_UINT));checks++;
 }
 resource.sampled_type=0;resource.set=2;CHECK(!gfx_reflect_texel_types(spv,sizeof spv/4,&resource,1));resource.set=1;
 uint32_t original=spv[19];spv[19]=1;CHECK(!gfx_reflect_texel_types(spv,sizeof spv/4,&resource,1));spv[19]=original;
 CHECK(!gfx_reflect_texel_types(spv,sizeof spv/4-1,&resource,1));
 original=spv[5];spv[5]=0;CHECK(!gfx_reflect_texel_types(spv,sizeof spv/4,&resource,1));spv[5]=original;
 // Cyclic type chains and absent descriptors must never loop or invent a type.
 original=spv[35];spv[35]=5;CHECK(!gfx_reflect_texel_types(spv,sizeof spv/4,&resource,1));spv[35]=original;
 resource.type=3;CHECK(gfx_reflect_texel_types(NULL,0,&resource,1));
}
static void policy(void){
 const VkFormat floating[]={VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_R32G32B32A32_SFLOAT};
 const VkFormat unsign[]={VK_FORMAT_R32_UINT,VK_FORMAT_R32G32_UINT,VK_FORMAT_R32G32B32A32_UINT};
 const VkFormat sign[]={VK_FORMAT_R32_SINT,VK_FORMAT_R32G32_SINT,VK_FORMAT_R32G32B32A32_SINT};
 for(unsigned i=0;i<3;i++){
  CHECK(vk_texel_sample_format(unsign[i],GFX_SAMPLED_FLOAT,0)==floating[i]);
  CHECK(vk_texel_sample_format(sign[i],GFX_SAMPLED_FLOAT,0)==floating[i]);
  CHECK(vk_texel_sample_format(unsign[i],GFX_SAMPLED_UINT,0)==unsign[i]);
  CHECK(vk_texel_sample_format(sign[i],GFX_SAMPLED_SINT,1)==sign[i]);
  CHECK(!vk_texel_sample_format(unsign[i],GFX_SAMPLED_FLOAT,1));
  CHECK(!vk_texel_sample_format(floating[i],GFX_SAMPLED_UINT,0));checks+=6;
 }
 CHECK(!vk_texel_sample_format(VK_FORMAT_R8_UINT,GFX_SAMPLED_FLOAT,0));
 CHECK(!vk_texel_sample_format(VK_FORMAT_R16G16_SINT,GFX_SAMPLED_FLOAT,0));
 CHECK(!vk_texel_sample_format(VK_FORMAT_R32_UINT,GFX_SAMPLED_UNKNOWN,0));
 CHECK(vk_texel_zero_format(GFX_SAMPLED_FLOAT)==VK_FORMAT_R32G32B32A32_SFLOAT);
 CHECK(vk_texel_zero_format(GFX_SAMPLED_SINT)==VK_FORMAT_R32G32B32A32_SINT);
 CHECK(vk_texel_zero_format(GFX_SAMPLED_UINT)==VK_FORMAT_R32G32B32A32_UINT);
}
int main(void){reflection();policy();printf("texel numeric type/reflection cases passed (%u)\n",checks);return 0;}
'''


@unittest.skipUnless(os.name=='nt' and CC.is_file(),'Windows Clang required')
class TexelTypeTests(unittest.TestCase):
    def test_spirv_types_and_strict_view_selection(self):
        with tempfile.TemporaryDirectory(prefix='hwaoi-texel-types-') as temp:
            src=Path(temp)/'fixture.c';src.write_text(SOURCE,encoding='utf-8');exe=src.with_suffix('.exe')
            for opt in ('-O0','-O2'):
                built=subprocess.run([str(CC),opt,'-Wall','-Wextra','-Werror','-I',str(ROOT/'runtime'),
                    '-I',str(DEPS/'include'),str(src),'-o',str(exe)],capture_output=True,text=True,encoding='utf-8',errors='replace')
                self.assertEqual(built.returncode,0,built.stdout+built.stderr)
                run=subprocess.run([str(exe)],capture_output=True,text=True)
                self.assertEqual(run.returncode,0,run.stdout+run.stderr)

    @unittest.skipUnless(GLSL.is_file(),'local shader compiler required')
    def test_gpu_bit_preservation_and_validation_negative_control(self):
        with tempfile.TemporaryDirectory(prefix='hwaoi-texel-gpu-') as temp:
            folder=Path(temp);shader=folder/'fetch.comp';spv=folder/'fetch.spv';exe=folder/'fixture.exe'
            shader.write_text('''#version 450
layout(local_size_x=1) in;
layout(binding=0) uniform samplerBuffer source;
layout(binding=1,std430) buffer Results { uvec4 value[]; } result;
void main(){uint i=gl_GlobalInvocationID.x;result.value[i]=floatBitsToUint(texelFetch(source,int(i)));}
''',encoding='utf-8')
            built=subprocess.run([str(GLSL),'-V',str(shader),'-o',str(spv)],capture_output=True,text=True)
            self.assertEqual(built.returncode,0,built.stdout+built.stderr)
            built=subprocess.run([str(CC),'-O2','-Wall','-Wextra','-Werror','-I',str(ROOT/'runtime'),
                '-I',str(DEPS/'include'),str(ROOT/'tests/hwaoi/texel_type_gpu_fixture.c'),'-L',str(DEPS/'lib'),
                '-lvulkan-1','-o',str(exe)],capture_output=True,text=True,encoding='utf-8',errors='replace')
            self.assertEqual(built.returncode,0,built.stdout+built.stderr)
            env=dict(os.environ,VK_LAYER_PATH='F:/VulkanValidationLayers/ucrt64/bin')
            for mode in ('good','bad'):
                run=subprocess.run([str(exe),str(spv),mode],cwd=folder,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=30)
                self.assertEqual(run.returncode,0,run.stdout+run.stderr)
                self.assertIn('expected negative control' if mode=='bad' else '0 validation errors (passed)',run.stdout)


if __name__=='__main__':unittest.main()
