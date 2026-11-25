// SPDX-License-Identifier: MIT
// Authored buffer bytes fetched on a Vulkan GPU through production view policy.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "gfx.h"
#include "vk_texel_policy.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define VK(x) CHECK((x)==VK_SUCCESS)
static unsigned errors;
static VkBool32 VKAPI_PTR message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT type,
                                  const VkDebugUtilsMessengerCallbackDataEXT *data,void *user){
    (void)type;(void)user;if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT){errors++;fprintf(stderr,"%s\n",data->pMessage);}return VK_FALSE;
}
typedef struct {VkBuffer buffer;VkDeviceMemory memory;uint32_t *map;} Buffer;
static VkDevice device;static VkPhysicalDeviceMemoryProperties memory;
static Buffer make_buffer(size_t bytes,VkBufferUsageFlags usage){
    Buffer b={0};VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,.usage=usage};
    VK(vkCreateBuffer(device,&info,NULL,&b.buffer));VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,b.buffer,&req);
    unsigned type=0;for(;type<memory.memoryTypeCount;type++)if((req.memoryTypeBits&(1u<<type))&&
        (memory.memoryTypes[type].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==
        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))break;
    CHECK(type<memory.memoryTypeCount);VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
    VK(vkAllocateMemory(device,&allocation,NULL,&b.memory));VK(vkBindBufferMemory(device,b.buffer,b.memory,0));
    VK(vkMapMemory(device,b.memory,0,bytes,0,(void**)&b.map));return b;
}
int main(int argc,char **argv){
    CHECK(argc==3);int bad=!strcmp(argv[2],"bad");
    VkApplicationInfo application={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
    const char *layer="VK_LAYER_KHRONOS_validation",*extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkDebugUtilsMessengerCreateInfoEXT debug={.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
        .pfnUserCallback=message};
    VkInstanceCreateInfo instance_info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pNext=&debug,.pApplicationInfo=&application,
        .enabledLayerCount=1,.ppEnabledLayerNames=&layer,.enabledExtensionCount=1,.ppEnabledExtensionNames=&extension};
    VkInstance instance;VK(vkCreateInstance(&instance_info,NULL,&instance));VkDebugUtilsMessengerEXT messenger;
    PFN_vkCreateDebugUtilsMessengerEXT create_debug=(PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT");
    VK(create_debug(instance,&debug,NULL,&messenger));
    uint32_t count=0;VK(vkEnumeratePhysicalDevices(instance,&count,NULL));CHECK(count);VkPhysicalDevice *devices=calloc(count,sizeof *devices);
    VK(vkEnumeratePhysicalDevices(instance,&count,devices));VkPhysicalDevice physical=devices[0];free(devices);
    vkGetPhysicalDeviceMemoryProperties(physical,&memory);vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,NULL);
    VkQueueFamilyProperties *families=calloc(count,sizeof *families);vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families);
    unsigned family=0;while(family<count&&!(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))family++;CHECK(family<count);free(families);
    float priority=1;VkDeviceQueueCreateInfo queue_info={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo device_info={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&queue_info};
    VK(vkCreateDevice(physical,&device_info,NULL,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    VkDescriptorSetLayoutBinding bindings[2]={{.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo set_info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};
    VkDescriptorSetLayout layout;VK(vkCreateDescriptorSetLayout(device,&set_info,NULL,&layout));
    VkPipelineLayoutCreateInfo pipeline_layout_info={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&layout};
    VkPipelineLayout pipeline_layout;VK(vkCreatePipelineLayout(device,&pipeline_layout_info,NULL,&pipeline_layout));
    VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
    VkDescriptorPoolCreateInfo pool_info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=sizes};
    VkDescriptorPool pool;VK(vkCreateDescriptorPool(device,&pool_info,NULL,&pool));
    VkDescriptorSetAllocateInfo set_allocate={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&layout};
    VkDescriptorSet set;VK(vkAllocateDescriptorSets(device,&set_allocate,&set));
    FILE *file=fopen(argv[1],"rb");CHECK(file);fseek(file,0,SEEK_END);long bytes=ftell(file);rewind(file);uint32_t *code=malloc((size_t)bytes);CHECK(fread(code,1,(size_t)bytes,file)==(size_t)bytes);fclose(file);
    VkShaderModuleCreateInfo module_info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)bytes,.pCode=code};
    VkShaderModule module;VK(vkCreateShaderModule(device,&module_info,NULL,&module));free(code);
    VkComputePipelineCreateInfo pipeline_info={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},.layout=pipeline_layout};
    VkPipeline pipeline;VK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pipeline_info,NULL,&pipeline));
    Buffer input=make_buffer(256,VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT),output=make_buffer(1024,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const uint32_t patterns[]={0,1,17,0x7fffffff,0x80000000,0xffffffff,0x3f800000,0xbf800000,0x7f800000,0x7fc12345,0x00800000,0x007fffff,0x12345678};
    for(unsigned i=0;i<64;i++)input.map[i]=patterns[i%(sizeof patterns/sizeof *patterns)];
    VkCommandPoolCreateInfo command_pool_info={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};
    VkCommandPool command_pool;VK(vkCreateCommandPool(device,&command_pool_info,NULL,&command_pool));
    VkCommandBufferAllocateInfo command_allocate={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=command_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command;VK(vkAllocateCommandBuffers(device,&command_allocate,&command));
    const VkFormat source_formats[]={VK_FORMAT_R32_UINT,VK_FORMAT_R32_SINT,VK_FORMAT_R32G32_UINT,VK_FORMAT_R32G32_SINT,VK_FORMAT_R32G32B32A32_UINT,VK_FORMAT_R32G32B32A32_SINT};
    unsigned checked=0;
    for(unsigned round=0;round<6;round++){
        unsigned components=round<2?1:round<4?2:4;
        VkFormat format=bad?source_formats[round]:vk_texel_sample_format(source_formats[round],GFX_SAMPLED_FLOAT,0);CHECK(format);
        VkBufferViewCreateInfo view_info={.sType=VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,.buffer=input.buffer,.format=format,.range=256};
        VkBufferView view;VK(vkCreateBufferView(device,&view_info,NULL,&view));VkDescriptorBufferInfo results={output.buffer,0,1024};
        VkWriteDescriptorSet writes[2]={{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,.pTexelBufferView=&view},
            {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&results}};
        vkUpdateDescriptorSets(device,2,writes,0,NULL);memset(output.map,0xcd,1024);
        VK(vkResetCommandBuffer(command,0));VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VK(vkBeginCommandBuffer(command,&begin));
        VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_UNIFORM_READ_BIT};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,NULL,0,NULL);
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,NULL);
        vkCmdDispatch(command,64/components,1,1);
        barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);VK(vkEndCommandBuffer(command));
        if(bad){CHECK(errors);vkDestroyBufferView(device,view,NULL);break;} // recording negative control; never submit invalid work
        CHECK(!errors);VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};VK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));VK(vkQueueWaitIdle(queue));
        for(unsigned element=0;element<64/components;element++)for(unsigned lane=0;lane<4;lane++){
            uint32_t expected=lane<components?input.map[element*components+lane]:lane==3?0x3f800000:0;
            if(output.map[element*4+lane]!=expected){fprintf(stderr,"raw texel mismatch round=%u element=%u lane=%u actual=%08x expected=%08x\n",round,element,lane,output.map[element*4+lane],expected);return 1;}checked++;
        }
        vkDestroyBufferView(device,view,NULL);
    }
    VK(vkDeviceWaitIdle(device));vkDestroyCommandPool(device,command_pool,NULL);vkDestroyPipeline(device,pipeline,NULL);vkDestroyShaderModule(device,module,NULL);
    vkDestroyDescriptorPool(device,pool,NULL);vkDestroyPipelineLayout(device,pipeline_layout,NULL);vkDestroyDescriptorSetLayout(device,layout,NULL);
    vkUnmapMemory(device,input.memory);vkDestroyBuffer(device,input.buffer,NULL);vkFreeMemory(device,input.memory,NULL);
    vkUnmapMemory(device,output.memory);vkDestroyBuffer(device,output.buffer,NULL);vkFreeMemory(device,output.memory,NULL);vkDestroyDevice(device,NULL);
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug=(PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT");destroy_debug(instance,messenger,NULL);vkDestroyInstance(instance,NULL);
    CHECK(bad?errors>0:errors==0);printf("texel GPU: %u exact bit checks; %u validation errors (%s)\n",checked,errors,bad?"expected negative control":"passed");return 0;
}
