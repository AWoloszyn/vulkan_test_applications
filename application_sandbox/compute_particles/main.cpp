// Copyright 2017 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "application_sandbox/sample_application_framework/sample_application.h"
#include "support/containers/deque.h"
#include "support/entry/entry.h"
#include "vulkan_helpers/buffer_frame_data.h"
#include "vulkan_helpers/helper_functions.h"
#include "vulkan_helpers/vulkan_application.h"
#include "vulkan_helpers/vulkan_model.h"
#include "vulkan_helpers/vulkan_texture.h"

#include "particle_data_shared.h"

#include <chrono>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

const VkCommandBufferBeginInfo kBeginCommandBuffer = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,  // sType
    nullptr,                                      // pNext
    0,                                            // flags
    nullptr                                       // pInheritanceInfo
};

namespace quad_model {
#include "fullscreen_quad.obj.h"
}
const auto& quad_data = quad_model::model;

uint32_t simulation_shader[] =
#include "particle_update.comp.spv"
    ;

uint32_t velocity_shader[] =
#include "particle_velocity_update.comp.spv"
    ;

uint32_t particle_fragment_shader[] =
#include "particle.frag.spv"
    ;

uint32_t particle_vertex_shader[] =
#include "particle.vert.spv"
    ;

namespace particle_texture {
#include "particle.png.h"
}

const auto& texture_data = particle_texture::texture;

struct time_data {
  int32_t frame_number;
  float time;
};

int main_entry(const entry::EntryData* data) {
  auto* allocator = data->allocator();
  data->logger()->LogInfo("Application Startup");
  vulkan::VulkanApplication app(data->allocator(), data->logger(), data, {},
                                VkPhysicalDeviceFeatures{}, 1024 * 1024 * 128,
                                1024 * 1024 * 128, 1024 * 1024 * 128,
                                1024 * 1024 * 128);
  // So we don't have to type app.device every time.
  vulkan::VkDevice& device = app.device();
  vulkan::VkQueue& render_queue = app.render_queue();

  VkDescriptorSetLayoutBinding compute_descriptor_set_layouts[3];

  // Both compute passes use the same set of descriptors for simplicity.
  // Technically we don't have to pass the draw_data SSBO to the velocity
  // update shader, but we don't want to have to do twice the work.
  compute_descriptor_set_layouts[0] = {
      1,                                  // binding
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  // descriptorType
      1,                                  // descriptorCount
      VK_SHADER_STAGE_COMPUTE_BIT,        // stageFlags
      nullptr                             // pImmutableSamplers
  };
  compute_descriptor_set_layouts[1] = {
      2,                                  // binding
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  // descriptorType
      1,                                  // descriptorCount
      VK_SHADER_STAGE_COMPUTE_BIT,        // stageFlags
      nullptr                             // pImmutableSamplers
  };
  // This should ideally be a UBO, but I was getting hangs in the shader
  // when using it as a UBO, switching to an SSBO worked.
  compute_descriptor_set_layouts[2] = {
      0,                                  // binding
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  // descriptorType
      1,                                  // descriptorCount
      VK_SHADER_STAGE_COMPUTE_BIT,        // stageFlags
      nullptr                             // pImmutableSamplers
  };

  auto compute_pipeline_layout =
      containers::make_unique<vulkan::PipelineLayout>(
          allocator,
          app.CreatePipelineLayout({{compute_descriptor_set_layouts[0],
                                     compute_descriptor_set_layouts[1],
                                     compute_descriptor_set_layouts[2]}}));

  auto simulation_pipeline =
      containers::make_unique<vulkan::VulkanComputePipeline>(
          allocator,
          app.CreateComputePipeline(
              compute_pipeline_layout.get(),
              VkShaderModuleCreateInfo{
                  VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                  sizeof(simulation_shader), simulation_shader},
              "main"));

  auto velocity_pipeline =
      containers::make_unique<vulkan::VulkanComputePipeline>(
          allocator,
          app.CreateComputePipeline(
              compute_pipeline_layout.get(),
              VkShaderModuleCreateInfo{
                  VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                  sizeof(velocity_shader), velocity_shader},
              "main"));

  // Create the single SSBO for simulation
  VkBufferCreateInfo buffer_create_info = {
      VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,       // sType
      nullptr,                                    // pNext
      0,                                          // createFlags
      sizeof(simulation_data) * TOTAL_PARTICLES,  // size
      VK_BUFFER_USAGE_TRANSFER_DST_BIT |
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,  // usageFlags
      VK_SHARING_MODE_EXCLUSIVE,               // sharingMode
      0,                                       // queueFamilyIndexCount
      nullptr                                  // pQueueFamilyIndices
  };

  auto setup_command_buffer = app.GetCommandBuffer();
  setup_command_buffer->vkBeginCommandBuffer(setup_command_buffer,
                                             &kBeginCommandBuffer);

  auto simulation_ssbo = app.CreateAndBindDeviceBuffer(&buffer_create_info);
  srand(0);
  containers::vector<simulation_data> fill_data(allocator);
  fill_data.resize(TOTAL_PARTICLES);
  for (auto& particle : fill_data) {
    float distance = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
    float angle = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
    angle = angle * 3.1415f * 2.0f;
    float x = sin(angle);
    float y = cos(angle);

    particle.position_velocity[0] = x * (1 - (distance * distance));
    particle.position_velocity[1] = y * (1 - (distance * distance));
    float posx = particle.position_velocity[0];
    float posy = particle.position_velocity[1];
    particle.position_velocity[2] = -posy * 0.05f;
    particle.position_velocity[3] = posx * 0.05f;
  }

  const size_t kNBuffers = 2;
  // Double-buffer the universe
  containers::unique_ptr<vulkan::VulkanApplication::Buffer>
      draw_buffers[kNBuffers];
  containers::unique_ptr<vulkan::DescriptorSet>
      compute_descriptor_sets[kNBuffers];

  vulkan::VkCommandBuffer compute_command_buffers[kNBuffers] = {
      app.GetCommandBuffer(),
      app.GetCommandBuffer(),
  };
  vulkan::VkSemaphore compute_ready_semaphores[kNBuffers] = {
      vulkan::CreateSemaphore(&app.device()),
      vulkan::CreateSemaphore(&app.device()),
  };
  vulkan::VkFence frame_ready_fence[kNBuffers] = {
      vulkan::CreateFence(&app.device()),
      vulkan::CreateFence(&app.device()),
  };

  auto update_time_data =
      containers::make_unique<vulkan::BufferFrameData<Mat44>>(
          allocator, &app, kNBuffers, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  auto aspect_buffer =
      containers::make_unique<vulkan::BufferFrameData<Vector4>>(
          allocator, &app, kNBuffers, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

  // Fill the buffer. Technically we probably want to use a staging buffer
  // and fill from that, since this is not really a "small" buffer.
  // However, we have this helper function, so might as well use it.
  app.FillSmallBuffer(simulation_ssbo.get(), fill_data.data(),
                      fill_data.size() * sizeof(simulation_data), 0,
                      &setup_command_buffer,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

  buffer_create_info = {
      VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,  // sType
      nullptr,                               // pNext
      0,                                     // createFlags
      sizeof(draw_data) * TOTAL_PARTICLES,   // size
      VK_BUFFER_USAGE_TRANSFER_DST_BIT |
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,  // usageFlags
      VK_SHARING_MODE_EXCLUSIVE,               // sharingMode
      0,                                       // queueFamilyIndexCount
      nullptr                                  // pQueueFamilyIndices
  };
  for (size_t i = 0; i < kNBuffers; ++i) {
    draw_buffers[i] = app.CreateAndBindDeviceBuffer(&buffer_create_info);
    compute_descriptor_sets[i] = containers::make_unique<vulkan::DescriptorSet>(
        allocator,
        app.AllocateDescriptorSet({compute_descriptor_set_layouts[0],
                                   compute_descriptor_set_layouts[1],
                                   compute_descriptor_set_layouts[2]}));
    VkDescriptorBufferInfo buffer_infos[3] = {
        {
            update_time_data->get_buffer(),             // buffer
            update_time_data->get_offset_for_frame(i),  // offset
            update_time_data->size(),                   // range
        },
        {
            *simulation_ssbo,         // buffer
            0,                        // offset
            simulation_ssbo->size(),  // range
        },
        {
            *draw_buffers[i],         // buffer
            0,                        // offset
            draw_buffers[i]->size(),  // range
        },
    };

    VkWriteDescriptorSet write = {
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,  // sType
        nullptr,                                 // pNext
        *compute_descriptor_sets[i],             // dstSet
        0,                                       // dstbinding
        0,                                       // dstArrayElement
        3,                                       // descriptorCount
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,       // descriptorType
        nullptr,                                 // pImageInfo
        buffer_infos,                            // pBufferInfo
        nullptr,                                 // pTexelBufferView
    };

    app.device()->vkUpdateDescriptorSets(app.device(), 1, &write, 0, nullptr);
  }

  // All of the compute stuff is now done:
  // Rendering stuff time
  vulkan::VulkanModel quad_model(allocator, data->logger(), quad_data);
  vulkan::VulkanTexture particle_texture(allocator, data->logger(),
                                         texture_data);

  quad_model.InitializeData(&app, &setup_command_buffer);
  particle_texture.InitializeData(&app, &setup_command_buffer);
  auto sampler = containers::make_unique<vulkan::VkSampler>(
      allocator,
      vulkan::CreateSampler(&app.device(), VK_FILTER_LINEAR, VK_FILTER_LINEAR));

  VkDescriptorSetLayoutBinding render_descriptor_set_layouts[4];

  render_descriptor_set_layouts[0] = {
      0,                                  // binding
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  // descriptorType
      1,                                  // descriptorCount
      VK_SHADER_STAGE_VERTEX_BIT,         // stageFlags
      nullptr                             // pImmutableSamplers
  };
  render_descriptor_set_layouts[3] = {
      3,                                  // binding
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  // descriptorType
      1,                                  // descriptorCount
      VK_SHADER_STAGE_VERTEX_BIT,         // stageFlags
      nullptr                             // pImmutableSamplers
  };
  render_descriptor_set_layouts[1] = {
      1,                             // binding
      VK_DESCRIPTOR_TYPE_SAMPLER,    // descriptorType
      1,                             // descriptorCount
      VK_SHADER_STAGE_FRAGMENT_BIT,  // stageFlags
      nullptr                        // pImmutableSamplers
  };
  render_descriptor_set_layouts[2] = {
      2,                                 // binding
      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  // descriptorType
      1,                                 // descriptorCount
      VK_SHADER_STAGE_FRAGMENT_BIT,      // stageFlags
      nullptr                            // pImmutableSamplers
  };

  auto render_pipeline_layout = containers::make_unique<vulkan::PipelineLayout>(
      allocator,
      app.CreatePipelineLayout(
          {{render_descriptor_set_layouts[0], render_descriptor_set_layouts[1],
            render_descriptor_set_layouts[2],
            render_descriptor_set_layouts[3]}}));
  VkAttachmentReference color_attachment = {
      0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

  auto render_pass = containers::make_unique<vulkan::VkRenderPass>(
      allocator,
      app.CreateRenderPass(
          {{
              0,                                         // flags
              app.swapchain().format(),                  // format
              VK_SAMPLE_COUNT_1_BIT,                     // samples
              VK_ATTACHMENT_LOAD_OP_CLEAR,               // loadOp
              VK_ATTACHMENT_STORE_OP_STORE,              // storeOp
              VK_ATTACHMENT_LOAD_OP_DONT_CARE,           // stenilLoadOp
              VK_ATTACHMENT_STORE_OP_DONT_CARE,          // stenilStoreOp
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,  // initialLayout
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL   // finalLayout
          }},  // AttachmentDescriptions
          {{
              0,                                // flags
              VK_PIPELINE_BIND_POINT_GRAPHICS,  // pipelineBindPoint
              0,                                // inputAttachmentCount
              nullptr,                          // pInputAttachments
              1,                                // colorAttachmentCount
              &color_attachment,                // colorAttachment
              nullptr,                          // pResolveAttachments
              nullptr,                          // pDepthStencilAttachment
              0,                                // preserveAttachmentCount
              nullptr                           // pPreserveAttachments
          }},                                   // SubpassDescriptions
          {}                                    // SubpassDependencies
          ));

  auto render_pipeline =
      containers::make_unique<vulkan::VulkanGraphicsPipeline>(
          allocator, app.CreateGraphicsPipeline(render_pipeline_layout.get(),
                                                render_pass.get(), 0));
  render_pipeline->AddShader(VK_SHADER_STAGE_VERTEX_BIT, "main",
                             particle_vertex_shader);
  render_pipeline->AddShader(VK_SHADER_STAGE_FRAGMENT_BIT, "main",
                             particle_fragment_shader);
  render_pipeline->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
  render_pipeline->SetInputStreams(&quad_model);
  render_pipeline->SetViewport(
      {0.0f, 0.f, static_cast<float>(app.swapchain().width()),
       static_cast<float>(app.swapchain().height()), 0.0f, 1.0f});
  render_pipeline->SetScissor(
      {{0, 0}, app.swapchain().width(), app.swapchain().height()});
  render_pipeline->SetSamples(VK_SAMPLE_COUNT_1_BIT);
  render_pipeline->AddAttachment(VkPipelineColorBlendAttachmentState{
      VK_TRUE, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD,
      VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD,
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT});
  render_pipeline->Commit();

  containers::unique_ptr<vulkan::DescriptorSet>
      render_descriptor_sets[kNBuffers];
  for (size_t i = 0; i < kNBuffers; ++i) {
    render_descriptor_sets[i] = containers::make_unique<vulkan::DescriptorSet>(
        allocator,
        app.AllocateDescriptorSet({render_descriptor_set_layouts[0],
                                   render_descriptor_set_layouts[1],
                                   render_descriptor_set_layouts[2],
                                   render_descriptor_set_layouts[3]}));

    // Write that buffer into the descriptor sets.
    VkDescriptorBufferInfo buffer_infos[2] = {
        {
            *draw_buffers[i],         // buffer
            0,                        // offset
            draw_buffers[i]->size(),  // range
        },
        {
            aspect_buffer->get_buffer(),             // buffer
            aspect_buffer->get_offset_for_frame(i),  // offset
            aspect_buffer->size(),                   // range
        }};

    VkDescriptorImageInfo sampler_info = {
        *sampler,                  // sampler
        VK_NULL_HANDLE,            // imageView
        VK_IMAGE_LAYOUT_UNDEFINED  //  imageLayout
    };

    VkDescriptorImageInfo texture_info = {
        VK_NULL_HANDLE,                            // sampler
        particle_texture.view(),                   // imageView
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,  // imageLayout
    };

    VkWriteDescriptorSet writes[4]{
        {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,  // sType
            nullptr,                                 // pNext
            *render_descriptor_sets[i],              // dstSet
            0,                                       // dstbinding
            0,                                       // dstArrayElement
            1,                                       // descriptorCount
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,       // descriptorType
            nullptr,                                 // pImageInfo
            &buffer_infos[0],                        // pBufferInfo
            nullptr,                                 // pTexelBufferView
        },
        {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,  // sType
            nullptr,                                 // pNext
            *render_descriptor_sets[i],              // dstSet
            3,                                       // dstbinding
            0,                                       // dstArrayElement
            1,                                       // descriptorCount
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,       // descriptorType
            nullptr,                                 // pImageInfo
            &buffer_infos[1],                        // pBufferInfo
            nullptr,                                 // pTexelBufferView
        },
        {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,  // sType
            nullptr,                                 // pNext
            *render_descriptor_sets[i],              // dstSet
            1,                                       // dstbinding
            0,                                       // dstArrayElement
            1,                                       // descriptorCount
            VK_DESCRIPTOR_TYPE_SAMPLER,              // descriptorType
            &sampler_info,                           // pImageInfo
            nullptr,                                 // pBufferInfo
            nullptr,                                 // pTexelBufferView
        },
        {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,  // sType
            nullptr,                                 // pNext
            *render_descriptor_sets[i],              // dstSet
            2,                                       // dstbinding
            0,                                       // dstArrayElement
            1,                                       // descriptorCount
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,        // descriptorType
            &texture_info,                           // pImageInfo
            nullptr,                                 // pBufferInfo
            nullptr,                                 // pTexelBufferView
        },
    };

    app.device()->vkUpdateDescriptorSets(app.device(), 4, writes, 0, nullptr);
  }

  vulkan::VkCommandBuffer render_command_buffers[kNBuffers] = {
      app.GetCommandBuffer(),
      app.GetCommandBuffer(),
  };
  vulkan::VkSemaphore render_ready_semaphores[kNBuffers] = {
      vulkan::CreateSemaphore(&app.device()),
      vulkan::CreateSemaphore(&app.device()),
  };
  vulkan::VkSemaphore swap_ready_semaphores[kNBuffers] = {
      vulkan::CreateSemaphore(&app.device()),
      vulkan::CreateSemaphore(&app.device()),
  };

  containers::vector<containers::unique_ptr<vulkan::VkFramebuffer>>
      framebuffers(allocator);
  framebuffers.resize(app.swapchain_images().size());
  containers::vector<containers::unique_ptr<vulkan::VkImageView>> image_views(
      allocator);
  containers::vector<VkImageLayout> layouts(allocator);
  layouts.resize(app.swapchain_images().size());
  image_views.resize(app.swapchain_images().size());

  VkImageViewCreateInfo view_create_info = {
      VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,  // sType
      nullptr,                                   // pNext
      0,                                         // flags
      VK_NULL_HANDLE,                            // image
      VK_IMAGE_VIEW_TYPE_2D,                     // viewType
      app.swapchain().format(),                  // format
      {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
       VK_COMPONENT_SWIZZLE_A},
      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};

  for (size_t i = 0; i < app.swapchain_images().size(); ++i) {
    layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;

    ::VkImageView raw_image_view;
    view_create_info.image = app.swapchain_images()[i];
    LOG_ASSERT(==, data->logger(), VK_SUCCESS,
               app.device()->vkCreateImageView(app.device(), &view_create_info,
                                               nullptr, &raw_image_view));
    image_views[i] = containers::make_unique<vulkan::VkImageView>(
        allocator, vulkan::VkImageView(raw_image_view, nullptr, &app.device()));
    VkFramebufferCreateInfo framebuffer_create_info{
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,  // sType
        nullptr,                                    // pNext
        0,                                          // flags
        *render_pass,                               // renderPass
        1,                                          // attachmentCount
        &(image_views[i]->get_raw_object()),        // attachments
        app.swapchain().width(),                    // width
        app.swapchain().height(),                   // height
        1                                           // layers
    };

    ::VkFramebuffer raw_framebuffer;
    LOG_ASSERT(
        ==, data->logger(), VK_SUCCESS,
        app.device()->vkCreateFramebuffer(
            app.device(), &framebuffer_create_info, nullptr, &raw_framebuffer));

    framebuffers[i] = containers::make_unique<vulkan::VkFramebuffer>(
        allocator,
        vulkan::VkFramebuffer(raw_framebuffer, nullptr, &app.device()));
  }
  setup_command_buffer->vkEndCommandBuffer(setup_command_buffer);
  VkSubmitInfo init_submit_info{
      VK_STRUCTURE_TYPE_SUBMIT_INFO,  // sType
      nullptr,                        // pNext
      0,                              // waitSemaphoreCount
      nullptr,                        // pWaitSemaphores
      nullptr,                        // pWaitDstStageMask,
      1,                              // commandBufferCount
      &(setup_command_buffer.get_command_buffer()),
      0,       // signalSemaphoreCount
      nullptr  // pSignalSemaphores
  };
  app.render_queue()->vkQueueSubmit(app.render_queue(), 1, &init_submit_info,
                                    static_cast<VkFence>(VK_NULL_HANDLE));

  app.render_queue()->vkQueueWaitIdle(app.render_queue());

  auto last_update_time = std::chrono::high_resolution_clock::now();
  auto last_print_time = last_update_time;
  uint64_t frame_idx = 0;
  uint64_t current_frame = 0;

  // Actually draw stuff
  for (;;) {
    // Next swapchain
    uint32_t i = frame_idx % kNBuffers;

    auto& computeBuff = compute_command_buffers[i];

    aspect_buffer->data()[0] =
        (float)app.swapchain().width() / (float)app.swapchain().height();
    aspect_buffer->UpdateBuffer(&app.render_queue(), i);

    auto current_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<float> elapsed_time = current_time - last_update_time;
    last_update_time = current_time;

    update_time_data->data()[0] = static_cast<float>(current_frame);
    update_time_data->data()[1] = elapsed_time.count();
    if (++current_frame >= TOTAL_PARTICLES) {
      current_frame = 0;
    }
    if ((frame_idx % 1000) == 0) {
      auto t = std::chrono::duration_cast<std::chrono::milliseconds>(
          current_time - last_print_time);
      last_print_time = current_time;

      // data->logger()->LogInfo("Frame time: ", (double)t.count() /
      // (double)1000,
      //                        "ms");
    }
    update_time_data->UpdateBuffer(&app.render_queue(), i);

    ::VkSemaphore wait_semaphore = compute_ready_semaphores[i];
    ::VkFence wait_fence = frame_ready_fence[i];

    VkBufferMemoryBarrier simulation_barriers[2] = {
        {
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,  // sType
            nullptr,                                  // pNext
            VK_ACCESS_SHADER_WRITE_BIT,               // srcAccessMask
            VK_ACCESS_SHADER_READ_BIT,                // dstAccessMask
            VK_QUEUE_FAMILY_IGNORED,                  // srcQueueFamilyIndex
            VK_QUEUE_FAMILY_IGNORED,                  // dstQueueFamilyIndex
            *simulation_ssbo,                         // buffer
            0,                                        //  offset
            simulation_ssbo->size(),                  // size
        },
        {
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,  // sType
            nullptr,                                  // pNext
            VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,      // srcAccessMask
            VK_ACCESS_SHADER_WRITE_BIT,               // dstAccessMask
            VK_QUEUE_FAMILY_IGNORED,                  // srcQueueFamilyIndex
            VK_QUEUE_FAMILY_IGNORED,                  // dstQueueFamilyIndex
            *draw_buffers[i],                         // buffer
            0,                                        //  offset
            draw_buffers[i]->size(),                  // size
        }};

    if (++frame_idx > kNBuffers) {
      app.device()->vkWaitForFences(app.device(), 1, &wait_fence, false,
                                    0xFFFFFFFFFFFFFFFF);
      app.device()->vkResetFences(app.device(), 1, &wait_fence);
    } else {
      wait_semaphore = ::VkSemaphore(VK_NULL_HANDLE);
    }

    computeBuff->vkBeginCommandBuffer(computeBuff, &kBeginCommandBuffer);

    computeBuff->vkCmdPipelineBarrier(
        computeBuff, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
        &simulation_barriers[0], 0, nullptr);
    computeBuff->vkCmdPipelineBarrier(
        computeBuff, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
        &simulation_barriers[1], 0, nullptr);
    computeBuff->vkCmdBindDescriptorSets(
        computeBuff, VK_PIPELINE_BIND_POINT_COMPUTE, *compute_pipeline_layout,
        0, 1, &compute_descriptor_sets[i]->raw_set(), 0, nullptr);
    computeBuff->vkCmdBindPipeline(computeBuff, VK_PIPELINE_BIND_POINT_COMPUTE,
                                   *velocity_pipeline);
    computeBuff->vkCmdDispatch(
        computeBuff, TOTAL_PARTICLES / COMPUTE_SHADER_LOCAL_SIZE, 1, 1);
    computeBuff->vkCmdBindPipeline(computeBuff, VK_PIPELINE_BIND_POINT_COMPUTE,
                                   *simulation_pipeline);
    computeBuff->vkCmdDispatch(
        computeBuff, TOTAL_PARTICLES / COMPUTE_SHADER_LOCAL_SIZE, 1, 1);
    simulation_barriers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    simulation_barriers[1].dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    computeBuff->vkCmdPipelineBarrier(
        computeBuff, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr, 1,
        &simulation_barriers[1], 0, nullptr);
    computeBuff->vkEndCommandBuffer(computeBuff);

    auto& render_buffer = render_command_buffers[i];
    render_buffer->vkBeginCommandBuffer(render_buffer, &kBeginCommandBuffer);

    uint32_t swapchain_idx;
    LOG_ASSERT(==, app.GetLogger(), VK_SUCCESS,
               app.device()->vkAcquireNextImageKHR(
                   app.device(), app.swapchain(), 0xFFFFFFFFFFFFFFFF,
                   swap_ready_semaphores[i].get_raw_object(),
                   static_cast<::VkFence>(VK_NULL_HANDLE), &swapchain_idx));

    VkImageMemoryBarrier barrier = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,    // sType
        nullptr,                                   // pNext
        VK_ACCESS_MEMORY_READ_BIT,                 // srcAccessMask
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,      // dstAccessMask
        layouts[swapchain_idx],                    // oldLayout
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,  // newLayout
        0,                                         // srcQueueFamilyIndex
        0,                                         // dstQueueFamilyIndex
        app.swapchain_images()[swapchain_idx],     // image
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};

    render_buffer->vkCmdPipelineBarrier(
        render_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
        nullptr, 1, &barrier);
    VkClearValue clear{};
    vulkan::MemoryClear(&clear);
    clear.color.float32[3] = 1.0f;
    // The rest of the normal drawing.
    VkRenderPassBeginInfo pass_begin = {
        VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,  // sType
        nullptr,                                   // pNext
        *render_pass,                              // renderPass
        *framebuffers[swapchain_idx],              // framebuffer
        {{0, 0},
         {app.swapchain().width(), app.swapchain().height()}},  // renderArea
        1,      // clearValueCount
        &clear  // clears
    };
    render_buffer->vkCmdBeginRenderPass(render_buffer, &pass_begin,
                                        VK_SUBPASS_CONTENTS_INLINE);
    render_buffer->vkCmdBindPipeline(
        render_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, *render_pipeline);
    render_buffer->vkCmdBindDescriptorSets(
        render_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        ::VkPipelineLayout(*render_pipeline_layout), 0, 1,
        &render_descriptor_sets[i]->raw_set(), 0, nullptr);
    quad_model.DrawInstanced(&render_buffer, TOTAL_PARTICLES);
    render_buffer->vkCmdEndRenderPass(render_buffer);

    VkImageMemoryBarrier present_barrier = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,    // sType
        nullptr,                                   // pNext
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,      // srcAccessMask
        VK_ACCESS_MEMORY_READ_BIT,                 // dstAccessMask
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,  // newLayout
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,           // oldLayout
        0,                                         // srcQueueFamilyIndex
        0,                                         // dstQueueFamilyIndex
        app.swapchain_images()[swapchain_idx],     // image
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    layouts[swapchain_idx] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    render_buffer->vkCmdPipelineBarrier(
        render_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
        &present_barrier);

    render_buffer->vkEndCommandBuffer(render_buffer);

    VkPipelineStageFlags stage_mask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    VkPipelineStageFlags stage_mask2 =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo render_submit_infos[2]{
        {
            VK_STRUCTURE_TYPE_SUBMIT_INFO,  // sType
            nullptr,                        // pNext
            1,                              // waitSemaphoreCount
            &wait_semaphore,                // pWaitSemaphores
            &stage_mask,                    // pWaitDstStageMask,
            1,                              // commandBufferCount
            &(computeBuff.get_command_buffer()),
            1,  // signalSemaphoreCount
            &compute_ready_semaphores[i].get_raw_object()  // pSignalSemaphores
        },
        {
            VK_STRUCTURE_TYPE_SUBMIT_INFO,               // sType
            nullptr,                                     // pNext
            1,                                           // waitSemaphoreCount
            &swap_ready_semaphores[i].get_raw_object(),  // pWaitSemaphores
            &stage_mask2,                                // pWaitDstStageMask,
            1,                                           // commandBufferCount
            &(render_buffer.get_command_buffer()),
            1,  // signalSemaphoreCount
            &render_ready_semaphores[i].get_raw_object()  // pSignalSemaphores
        }};

    if (wait_semaphore == VK_NULL_HANDLE) {
      render_submit_infos[0].waitSemaphoreCount = 0;
      render_submit_infos[0].pWaitSemaphores = nullptr;
      render_submit_infos[0].pWaitDstStageMask = nullptr;
    }

    app.render_queue()->vkQueueSubmit(app.render_queue(), 2,
                                      &render_submit_infos[0], wait_fence);

    VkPresentInfoKHR present_info{
        VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,            // sType
        nullptr,                                       // pNext
        1,                                             // waitSemaphoreCount
        &render_ready_semaphores[i].get_raw_object(),  // pWaitSemaphores
        1,                                             // swapchainCount
        &app.swapchain().get_raw_object(),             // pSwapchains
        &swapchain_idx,                                // pImageIndices
        nullptr,                                       // pResults
    };
    LOG_ASSERT(==, app.GetLogger(),
               app.render_queue()->vkQueuePresentKHR(app.present_queue(),
                                                     &present_info),
               VK_SUCCESS);
  }
  data->logger()->LogInfo("Application Shutdown");
  return 0;
}
