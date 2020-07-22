#include "pch.h"

#include "Vulkan.h"

#include <kinc/graphics5/pipeline.h>
#include <kinc/graphics5/shader.h>

#include <assert.h>
#include <malloc.h>

#include <map>
#include <string.h>
#include <string>

extern VkDevice device;
extern VkDescriptorSet desc_set;
VkDescriptorSetLayout desc_layout;
extern kinc_g5_texture_t *vulkanTextures[8];
extern kinc_g5_render_target_t *vulkanRenderTargets[8];
extern uint32_t swapchainImageCount;
extern uint32_t current_buffer;
bool memory_type_from_properties(uint32_t typeBits, VkFlags requirements_mask, uint32_t *typeIndex);

VkDescriptorPool desc_pools[3];

kinc_g5_pipeline_t *currentPipeline = NULL;

static bool has_number(kinc_internal_named_number *named_numbers, const char *name) {
	for (int i = 0; i < KINC_INTERNAL_NAMED_NUMBER_COUNT; ++i) {
		if (strcmp(named_numbers[i].name, name) == 0) {
			return true;
		}
	}
	return false;
}

static uint32_t find_number(kinc_internal_named_number *named_numbers, const char *name) {
	for (int i = 0; i < KINC_INTERNAL_NAMED_NUMBER_COUNT; ++i) {
		if (strcmp(named_numbers[i].name, name) == 0) {
			return named_numbers[i].number;
		}
	}
	return 0;
}

static void set_number(kinc_internal_named_number *named_numbers, const char *name, uint32_t number) {
	for (int i = 0; i < KINC_INTERNAL_NAMED_NUMBER_COUNT; ++i) {
		if (strcmp(named_numbers[i].name, name) == 0) {
			named_numbers[i].number = number;
			return;
		}
	}

	for (int i = 0; i < KINC_INTERNAL_NAMED_NUMBER_COUNT; ++i) {
		if (named_numbers[i].name[0] == 0) {
			strcpy(named_numbers[i].name, name);
			named_numbers[i].number = number;
			return;
		}
	}

	assert(false);
}

namespace {
	void parseShader(kinc_g5_shader_t *shader, kinc_internal_named_number *locations, kinc_internal_named_number *textureBindings,
	                 kinc_internal_named_number *uniformOffsets) {
		memset(locations, 0, sizeof(kinc_internal_named_number) * KINC_INTERNAL_NAMED_NUMBER_COUNT);
		memset(textureBindings, 0, sizeof(kinc_internal_named_number) * KINC_INTERNAL_NAMED_NUMBER_COUNT);
		memset(uniformOffsets, 0, sizeof(kinc_internal_named_number) * KINC_INTERNAL_NAMED_NUMBER_COUNT);

		uint32_t *spirv = (uint32_t *)shader->impl.source;
		int spirvsize = shader->impl.length / 4;
		int index = 0;

		unsigned magicNumber = spirv[index++];
		unsigned version = spirv[index++];
		unsigned generator = spirv[index++];
		unsigned bound = spirv[index++];
		index++;

		std::map<uint32_t, std::string> names;
		std::map<uint32_t, std::string> memberNames;
		std::map<uint32_t, uint32_t> locs;
		std::map<uint32_t, uint32_t> bindings;
		std::map<uint32_t, uint32_t> offsets;

		while (index < spirvsize) {
			int wordCount = spirv[index] >> 16;
			uint32_t opcode = spirv[index] & 0xffff;

			uint32_t *operands = wordCount > 1 ? &spirv[index + 1] : nullptr;
			uint32_t length = wordCount - 1;

			switch (opcode) {
			case 5: { // OpName
				uint32_t id = operands[0];
				char *string = (char *)&operands[1];
				names[id] = string;
				break;
			}
			case 6: { // OpMemberName
				uint32_t type = operands[0];
				if (names[type] == "_k_global_uniform_buffer_type") {
					uint32_t member = operands[1];
					char *string = (char *)&operands[2];
					memberNames[member] = string;
				}
				break;
			}
			case 71: { // OpDecorate
				uint32_t id = operands[0];
				uint32_t decoration = operands[1];
				if (decoration == 30) { // location
					uint32_t location = operands[2];
					locs[id] = location;
				}
				if (decoration == 33) { // binding
					uint32_t binding = operands[2];
					bindings[id] = binding;
				}
				break;
			}
			case 72: { // OpMemberDecorate
				uint32_t type = operands[0];
				if (names[type] == "_k_global_uniform_buffer_type") {
					uint32_t member = operands[1];
					uint32_t decoration = operands[2];
					if (decoration == 35) { // offset
						uint32_t offset = operands[3];
						offsets[member] = offset;
					}
				}
			}
			}

			index += wordCount;
		}

		for (std::map<uint32_t, uint32_t>::iterator it = locs.begin(); it != locs.end(); ++it) {
			set_number(locations, names[it->first].c_str(), it->second);
		}

		for (std::map<uint32_t, uint32_t>::iterator it = bindings.begin(); it != bindings.end(); ++it) {
			set_number(textureBindings, names[it->first].c_str(), it->second);
		}

		for (std::map<uint32_t, uint32_t>::iterator it = offsets.begin(); it != offsets.end(); ++it) {
			set_number(uniformOffsets, memberNames[it->first].c_str(), it->second);
		}
	}

	VkShaderModule demo_prepare_shader_module(const void *code, size_t size) {
		VkShaderModuleCreateInfo moduleCreateInfo;
		VkShaderModule module;
		VkResult err;

		moduleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		moduleCreateInfo.pNext = NULL;

		moduleCreateInfo.codeSize = size;
		moduleCreateInfo.pCode = (const uint32_t *)code;
		moduleCreateInfo.flags = 0;
		err = vkCreateShaderModule(device, &moduleCreateInfo, NULL, &module);
		assert(!err);

		return module;
	}

	VkShaderModule demo_prepare_vs(VkShaderModule &vert_shader_module, kinc_g5_shader_t *vertexShader) {
		vert_shader_module = demo_prepare_shader_module(vertexShader->impl.source, vertexShader->impl.length);
		return vert_shader_module;
	}

	VkShaderModule demo_prepare_fs(VkShaderModule &frag_shader_module, kinc_g5_shader_t *fragmentShader) {
		frag_shader_module = demo_prepare_shader_module(fragmentShader->impl.source, fragmentShader->impl.length);
		return frag_shader_module;
	}
}

void kinc_g5_pipeline_init(kinc_g5_pipeline_t *pipeline) {
	kinc_g5_internal_pipeline_init(pipeline);
}

void kinc_g5_pipeline_destroy(kinc_g5_pipeline_t *pipeline) {}

kinc_g5_constant_location_t kinc_g5_pipeline_get_constant_location(kinc_g5_pipeline_t *pipeline, const char *name) {
	kinc_g5_constant_location_t location;
	location.impl.vertexOffset = -1;
	location.impl.fragmentOffset = -1;
	if (has_number(pipeline->impl.vertexOffsets, name)) {
		location.impl.vertexOffset = find_number(pipeline->impl.vertexOffsets, name);
	}
	if (has_number(pipeline->impl.fragmentOffsets, name)) {
		location.impl.fragmentOffset = find_number(pipeline->impl.fragmentOffsets, name);
	}
	return location;
}

kinc_g5_texture_unit_t kinc_g5_pipeline_get_texture_unit(kinc_g5_pipeline_t *pipeline, const char *name) {
	kinc_g5_texture_unit_t unit;
	unit.impl.binding = find_number(pipeline->impl.textureBindings, name);
	return unit;
}

namespace {
	VkCullModeFlagBits convert_cull_mode(kinc_g5_cull_mode_t cullMode) {
		switch (cullMode) {
		case KINC_G5_CULL_MODE_CLOCKWISE:
			return VK_CULL_MODE_BACK_BIT;
		case KINC_G5_CULL_MODE_COUNTERCLOCKWISE:
			return VK_CULL_MODE_FRONT_BIT;
		case KINC_G5_CULL_MODE_NEVER:
		default:
			return VK_CULL_MODE_NONE;
		}
	}

	VkCompareOp convert_compare_mode(kinc_g5_compare_mode_t compare) {
		switch (compare) {
		default:
		case KINC_G5_COMPARE_MODE_ALWAYS:
			return VK_COMPARE_OP_ALWAYS;
		case KINC_G5_COMPARE_MODE_NEVER:
			return VK_COMPARE_OP_NEVER;
		case KINC_G5_COMPARE_MODE_EQUAL:
			return VK_COMPARE_OP_EQUAL;
		case KINC_G5_COMPARE_MODE_NOT_EQUAL:
			return VK_COMPARE_OP_NOT_EQUAL;
		case KINC_G5_COMPARE_MODE_LESS:
			return VK_COMPARE_OP_LESS;
		case KINC_G5_COMPARE_MODE_LESS_EQUAL:
			return VK_COMPARE_OP_LESS_OR_EQUAL;
		case KINC_G5_COMPARE_MODE_GREATER:
			return VK_COMPARE_OP_GREATER;
		case KINC_G5_COMPARE_MODE_GREATER_EQUAL:
			return VK_COMPARE_OP_GREATER_OR_EQUAL;
		}
	}
}

void kinc_g5_pipeline_compile(kinc_g5_pipeline_t *pipeline) {
	parseShader(pipeline->vertexShader, pipeline->impl.vertexLocations, pipeline->impl.textureBindings, pipeline->impl.vertexOffsets);
	parseShader(pipeline->fragmentShader, pipeline->impl.fragmentLocations, pipeline->impl.textureBindings, pipeline->impl.fragmentOffsets);

	//

	VkPipelineLayoutCreateInfo pPipelineLayoutCreateInfo = {};
	pPipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pPipelineLayoutCreateInfo.pNext = NULL;
	pPipelineLayoutCreateInfo.setLayoutCount = 1;
	pPipelineLayoutCreateInfo.pSetLayouts = &desc_layout;

	VkResult err = vkCreatePipelineLayout(device, &pPipelineLayoutCreateInfo, NULL, &pipeline->impl.pipeline_layout);
	assert(!err);

	//

	VkGraphicsPipelineCreateInfo pipeline_info = {};
	VkPipelineCacheCreateInfo pipelineCache_info = {};

	VkPipelineInputAssemblyStateCreateInfo ia = {};
	VkPipelineRasterizationStateCreateInfo rs = {};
	VkPipelineColorBlendStateCreateInfo cb = {};
	VkPipelineDepthStencilStateCreateInfo ds = {};
	VkPipelineViewportStateCreateInfo vp = {};
	VkPipelineMultisampleStateCreateInfo ms = {};
	const int dynamicStatesCount = 2;
	VkDynamicState dynamicStateEnables[dynamicStatesCount];
	VkPipelineDynamicStateCreateInfo dynamicState = {};

	memset(dynamicStateEnables, 0, sizeof(dynamicStateEnables));
	memset(&dynamicState, 0, sizeof(dynamicState));
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.pDynamicStates = dynamicStateEnables;

	memset(&pipeline_info, 0, sizeof(pipeline_info));
	pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeline_info.layout = pipeline->impl.pipeline_layout;

	uint32_t stride = 0;
	for (int i = 0; i < pipeline->inputLayout[0]->size; ++i) {
		kinc_g5_vertex_element_t element = pipeline->inputLayout[0]->elements[i];
		switch (element.data) {
		case KINC_G4_VERTEX_DATA_COLOR:
			stride += 1 * 4;
			break;
		case KINC_G4_VERTEX_DATA_FLOAT1:
			stride += 1 * 4;
			break;
		case KINC_G4_VERTEX_DATA_FLOAT2:
			stride += 2 * 4;
			break;
		case KINC_G4_VERTEX_DATA_FLOAT3:
			stride += 3 * 4;
			break;
		case KINC_G4_VERTEX_DATA_FLOAT4:
			stride += 4 * 4;
			break;
		case KINC_G4_VERTEX_DATA_FLOAT4X4:
			stride += 4 * 4 * 4;
			break;
		case KINC_G4_VERTEX_DATA_SHORT2_NORM:
			stride += 2 * 2;
			break;
		case KINC_G4_VERTEX_DATA_SHORT4_NORM:
			stride += 4 * 2;
			break;
		}
	}

	VkVertexInputBindingDescription vi_bindings[1];
#ifdef KORE_WINDOWS
	VkVertexInputAttributeDescription *vi_attrs =
	    (VkVertexInputAttributeDescription *)alloca(sizeof(VkVertexInputAttributeDescription) * pipeline->inputLayout[0]->size);
#else
	VkVertexInputAttributeDescription vi_attrs[pipeline->inputLayout[0]->size];
#endif

	VkPipelineVertexInputStateCreateInfo vi = {};
	vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vi.pNext = NULL;
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = vi_bindings;
	vi.vertexAttributeDescriptionCount = pipeline->inputLayout[0]->size;
	vi.pVertexAttributeDescriptions = vi_attrs;

	vi_bindings[0].binding = 0;
	vi_bindings[0].stride = stride;
	vi_bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	uint32_t offset = 0;
	for (int i = 0; i < pipeline->inputLayout[0]->size; ++i) {
		kinc_g5_vertex_element_t element = pipeline->inputLayout[0]->elements[i];
		switch (element.data) {
		case KINC_G4_VERTEX_DATA_COLOR:
			vi_attrs[i].binding = 0;
			vi_attrs[i].location = find_number(pipeline->impl.vertexLocations, element.name);
			vi_attrs[i].format = VK_FORMAT_R32_UINT;
			vi_attrs[i].offset = offset;
			offset += 1 * 4;
			break;
		case KINC_G4_VERTEX_DATA_FLOAT1:
			vi_attrs[i].binding = 0;
			vi_attrs[i].location = find_number(pipeline->impl.vertexLocations, element.name);
			vi_attrs[i].format = VK_FORMAT_R32_SFLOAT;
			vi_attrs[i].offset = offset;
			offset += 1 * 4;
			break;
		case KINC_G4_VERTEX_DATA_FLOAT2:
			vi_attrs[i].binding = 0;
			vi_attrs[i].location = find_number(pipeline->impl.vertexLocations, element.name);
			vi_attrs[i].format = VK_FORMAT_R32G32_SFLOAT;
			vi_attrs[i].offset = offset;
			offset += 2 * 4;
			break;
		case KINC_G4_VERTEX_DATA_FLOAT3:
			vi_attrs[i].binding = 0;
			vi_attrs[i].location = find_number(pipeline->impl.vertexLocations, element.name);
			vi_attrs[i].format = VK_FORMAT_R32G32B32_SFLOAT;
			vi_attrs[i].offset = offset;
			offset += 3 * 4;
			break;
		case KINC_G4_VERTEX_DATA_FLOAT4:
			vi_attrs[i].binding = 0;
			vi_attrs[i].location = find_number(pipeline->impl.vertexLocations, element.name);
			vi_attrs[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
			vi_attrs[i].offset = offset;
			offset += 4 * 4;
			break;
		case KINC_G4_VERTEX_DATA_FLOAT4X4:
			vi_attrs[i].binding = 0;
			vi_attrs[i].location = find_number(pipeline->impl.vertexLocations, element.name);
			vi_attrs[i].format = VK_FORMAT_R32G32B32A32_SFLOAT; // TODO
			vi_attrs[i].offset = offset;
			offset += 4 * 4 * 4;
			break;
		case KINC_G4_VERTEX_DATA_SHORT2_NORM:
			vi_attrs[i].binding = 0;
			vi_attrs[i].location = find_number(pipeline->impl.vertexLocations, element.name);
			vi_attrs[i].format = VK_FORMAT_R16G16_SNORM;
			vi_attrs[i].offset = offset;
			offset += 2 * 2;
			break;
		case KINC_G4_VERTEX_DATA_SHORT4_NORM:
			vi_attrs[i].binding = 0;
			vi_attrs[i].location = find_number(pipeline->impl.vertexLocations, element.name);
			vi_attrs[i].format = VK_FORMAT_R16G16B16A16_SNORM;
			vi_attrs[i].offset = offset;
			offset += 4 * 2;
			break;
		}
	}

	memset(&ia, 0, sizeof(ia));
	ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	memset(&rs, 0, sizeof(rs));
	rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = convert_cull_mode(pipeline->cullMode);
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.depthClampEnable = VK_FALSE;
	rs.rasterizerDiscardEnable = VK_FALSE;
	rs.depthBiasEnable = VK_FALSE;
	rs.lineWidth = 1.0f;

	memset(&cb, 0, sizeof(cb));
	cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	VkPipelineColorBlendAttachmentState att_state[8];
	memset(att_state, 0, sizeof(att_state));
	for (int i = 0; i < pipeline->colorAttachmentCount; ++i) {
		att_state[i].colorWriteMask = (pipeline->colorWriteMaskRed[i] ? VK_COLOR_COMPONENT_R_BIT  : 0)
			| (pipeline->colorWriteMaskGreen[i] ? VK_COLOR_COMPONENT_G_BIT  : 0)
			| (pipeline->colorWriteMaskBlue[i] ? VK_COLOR_COMPONENT_B_BIT  : 0)
			| (pipeline->colorWriteMaskAlpha[i] ? VK_COLOR_COMPONENT_A_BIT  : 0);
		att_state[i].blendEnable = VK_FALSE;
	}
	cb.attachmentCount = pipeline->colorAttachmentCount;
	cb.pAttachments = att_state;

	memset(&vp, 0, sizeof(vp));
	vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	vp.viewportCount = 1;
	dynamicStateEnables[dynamicState.dynamicStateCount++] = VK_DYNAMIC_STATE_VIEWPORT;
	vp.scissorCount = 1;
	dynamicStateEnables[dynamicState.dynamicStateCount++] = VK_DYNAMIC_STATE_SCISSOR;

	memset(&ds, 0, sizeof(ds));
	ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	ds.depthTestEnable = pipeline->depthMode != KINC_G5_COMPARE_MODE_ALWAYS;
	ds.depthWriteEnable = pipeline->depthWrite;
	ds.depthCompareOp = convert_compare_mode(pipeline->depthMode);
	ds.depthBoundsTestEnable = VK_FALSE;
	ds.back.failOp = VK_STENCIL_OP_KEEP;
	ds.back.passOp = VK_STENCIL_OP_KEEP;
	ds.back.compareOp = VK_COMPARE_OP_ALWAYS;
	ds.stencilTestEnable = VK_FALSE;
	ds.front = ds.back;

	memset(&ms, 0, sizeof(ms));
	ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	ms.pSampleMask = nullptr;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	pipeline_info.stageCount = 2;
	VkPipelineShaderStageCreateInfo shaderStages[2];
	memset(&shaderStages, 0, 2 * sizeof(VkPipelineShaderStageCreateInfo));

	shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	shaderStages[0].module = demo_prepare_vs(pipeline->impl.vert_shader_module, pipeline->vertexShader);
	shaderStages[0].pName = "main";

	shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	shaderStages[1].module = demo_prepare_fs(pipeline->impl.frag_shader_module, pipeline->fragmentShader);
	shaderStages[1].pName = "main";

	VkAttachmentDescription attachments[9];
	for (int i = 0; i < pipeline->colorAttachmentCount; ++i) {
		attachments[i].format = VK_FORMAT_B8G8R8A8_UNORM;
		attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[i].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachments[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachments[i].flags = 0;
	}
	attachments[pipeline->colorAttachmentCount].format = VK_FORMAT_D16_UNORM;
	attachments[pipeline->colorAttachmentCount].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[pipeline->colorAttachmentCount].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[pipeline->colorAttachmentCount].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[pipeline->colorAttachmentCount].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[pipeline->colorAttachmentCount].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[pipeline->colorAttachmentCount].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	attachments[pipeline->colorAttachmentCount].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	attachments[pipeline->colorAttachmentCount].flags = 0;

	VkAttachmentReference color_references[8];
	for (int i = 0; i < pipeline->colorAttachmentCount; ++i) {
		color_references[i].attachment = i;
		color_references[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	VkAttachmentReference depth_reference = {};
	depth_reference.attachment = pipeline->colorAttachmentCount;
	depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.flags = 0;
	subpass.inputAttachmentCount = 0;
	subpass.pInputAttachments = nullptr;
	subpass.colorAttachmentCount = pipeline->colorAttachmentCount;
	subpass.pColorAttachments = color_references;
	subpass.pResolveAttachments = nullptr;
	subpass.pDepthStencilAttachment = &depth_reference;
	subpass.preserveAttachmentCount = 0;
	subpass.pPreserveAttachments = nullptr;

	VkRenderPassCreateInfo rp_info = {};
	rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rp_info.pNext = nullptr;
	rp_info.attachmentCount = pipeline->colorAttachmentCount + 1;
	rp_info.pAttachments = attachments;
	rp_info.subpassCount = 1;
	rp_info.pSubpasses = &subpass;
	rp_info.dependencyCount = 0;
	rp_info.pDependencies = nullptr;

	VkRenderPass render_pass;
	err = vkCreateRenderPass(device, &rp_info, NULL, &render_pass);
	assert(!err);

	pipeline_info.pVertexInputState = &vi;
	pipeline_info.pInputAssemblyState = &ia;
	pipeline_info.pRasterizationState = &rs;
	pipeline_info.pColorBlendState = &cb;
	pipeline_info.pMultisampleState = &ms;
	pipeline_info.pViewportState = &vp;
	pipeline_info.pDepthStencilState = &ds;
	pipeline_info.pStages = shaderStages;
	pipeline_info.renderPass = render_pass;
	pipeline_info.pDynamicState = &dynamicState;

	memset(&pipelineCache_info, 0, sizeof(pipelineCache_info));
	pipelineCache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

	err = vkCreatePipelineCache(device, &pipelineCache_info, nullptr, &pipeline->impl.pipelineCache);
	assert(!err);
	err = vkCreateGraphicsPipelines(device, pipeline->impl.pipelineCache, 1, &pipeline_info, nullptr, &pipeline->impl.pipeline);
	assert(!err);

	vkDestroyPipelineCache(device, pipeline->impl.pipelineCache, nullptr);

	vkDestroyShaderModule(device, pipeline->impl.frag_shader_module, nullptr);
	vkDestroyShaderModule(device, pipeline->impl.vert_shader_module, nullptr);
}

void createDescriptorLayout() {
	VkDescriptorSetLayoutBinding layoutBindings[8];
	memset(layoutBindings, 0, sizeof(layoutBindings));

	layoutBindings[0].binding = 0;
	layoutBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	layoutBindings[0].descriptorCount = 1;
	layoutBindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	layoutBindings[0].pImmutableSamplers = nullptr;

	layoutBindings[1].binding = 1;
	layoutBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	layoutBindings[1].descriptorCount = 1;
	layoutBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	layoutBindings[1].pImmutableSamplers = nullptr;

	for (int i = 2; i < 8; ++i) {
		layoutBindings[i].binding = i;
		layoutBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		layoutBindings[i].descriptorCount = 1;
		layoutBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		layoutBindings[i].pImmutableSamplers = nullptr;
	}

	VkDescriptorSetLayoutCreateInfo descriptor_layout = {};
	descriptor_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	descriptor_layout.pNext = NULL;
	descriptor_layout.bindingCount = 8;
	descriptor_layout.pBindings = layoutBindings;

	VkResult err = vkCreateDescriptorSetLayout(device, &descriptor_layout, NULL, &desc_layout);
	assert(!err);

	VkDescriptorPoolSize typeCounts[8];
	memset(typeCounts, 0, sizeof(typeCounts));

	typeCounts[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	typeCounts[0].descriptorCount = 1;

	typeCounts[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	typeCounts[1].descriptorCount = 1;

	for (int i = 2; i < 8; ++i) {
		typeCounts[i].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		typeCounts[i].descriptorCount = 1;
	}

	VkDescriptorPoolCreateInfo descriptor_pool = {};
	descriptor_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	descriptor_pool.pNext = NULL;
	descriptor_pool.maxSets = 1024;
	descriptor_pool.poolSizeCount = 8;
	descriptor_pool.pPoolSizes = typeCounts;

	for (int i = 0; i < 3; ++i) {
		err = vkCreateDescriptorPool(device, &descriptor_pool, NULL, &desc_pools[i]);
		assert(!err);
	}
}

void Kore::Vulkan::createDescriptorSet(struct PipelineState5Impl_s *pipeline, VkDescriptorSet &desc_set) {
	VkDescriptorBufferInfo buffer_descs[2];

	VkDescriptorSetAllocateInfo alloc_info = {};
	alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc_info.pNext = NULL;
	alloc_info.descriptorPool = desc_pools[current_buffer];
	alloc_info.descriptorSetCount = 1;
	alloc_info.pSetLayouts = &desc_layout;
	VkResult err = vkAllocateDescriptorSets(device, &alloc_info, &desc_set);
	assert(!err);

	memset(&buffer_descs, 0, sizeof(buffer_descs));

	if (vertexUniformBuffer != nullptr) {
		buffer_descs[0].buffer = *vertexUniformBuffer;
	}
	buffer_descs[0].offset = 0;
	buffer_descs[0].range = 256 * sizeof(float);

	if (fragmentUniformBuffer != nullptr) {
		buffer_descs[1].buffer = *fragmentUniformBuffer;
	}
	buffer_descs[1].offset = 0;
	buffer_descs[1].range = 256 * sizeof(float);

	VkDescriptorImageInfo tex_desc[6];
	memset(&tex_desc, 0, sizeof(tex_desc));

	for (int i = 0; i < 6; ++i) {
		if (vulkanTextures[i] != nullptr) {
			tex_desc[i].sampler = vulkanTextures[i]->impl.texture.sampler;
			tex_desc[i].imageView = vulkanTextures[i]->impl.texture.view;
		}
		else if (vulkanRenderTargets[i] != nullptr) {
			tex_desc[i].sampler = vulkanRenderTargets[i]->impl.sampler;
			tex_desc[i].imageView = vulkanRenderTargets[i]->impl.destView;
		}
		tex_desc[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	VkWriteDescriptorSet writes[8];
	memset(&writes, 0, sizeof(writes));

	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = desc_set;
	writes[0].dstBinding = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	writes[0].pBufferInfo = &buffer_descs[0];

	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].dstSet = desc_set;
	writes[1].dstBinding = 1;
	writes[1].descriptorCount = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	writes[1].pBufferInfo = &buffer_descs[1];

	for (int i = 2; i < 8; ++i) {
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = desc_set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo = &tex_desc[i - 2];
	}

	if (vulkanTextures[0] != nullptr || vulkanRenderTargets[0] != nullptr) {
		if (vertexUniformBuffer != nullptr && fragmentUniformBuffer != nullptr) {
			vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
		}
		else {
			vkUpdateDescriptorSets(device, 1, writes + 2, 0, nullptr);
		}
	}
	else {
		if (vertexUniformBuffer != nullptr && fragmentUniformBuffer != nullptr) {
			vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
		}
	}
}
