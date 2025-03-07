#ifdef DIRECT3D12

// Socket is used in microprofile this header need to be included before d3d12 headers
#include <WinSock2.h>
#include <d3d12.h>
#include <d3dcompiler.h>

// OS
#include "../../OS/Interfaces/ILog.h"
#include "../../ThirdParty/OpenSource/EASTL/hash_set.h"
#include "../../ThirdParty/OpenSource/EASTL/hash_map.h"
#include "../../ThirdParty/OpenSource/EASTL/sort.h"

// Renderer
#include "../IRenderer.h"
#include "../IRay.h"
#include "../ResourceLoader.h"
#include "Direct3D12Hooks.h"
#include "Direct3D12MemoryAllocator.h"
#include "../../OS/Interfaces/IMemory.h"

//check if WindowsSDK is used which supports raytracing
#ifdef ENABLE_RAYTRACING

// TODO: all thesae definitions are also declared in Direct3D12: move to a common H file
typedef struct DescriptorStoreHeap
{
	uint32_t mNumDescriptors;
	/// DescriptorInfo Increment Size
	uint32_t mDescriptorSize;
	/// Bitset for finding SAFE_FREE descriptor slots
	uint32_t* flags;
	/// Lock for multi-threaded descriptor allocations
	Mutex* pAllocationMutex;
	uint64_t mUsedDescriptors;
	/// Type of descriptor heap -> CBV / DSV / ...
	D3D12_DESCRIPTOR_HEAP_TYPE mType;
	/// DX Heap
	ID3D12DescriptorHeap* pCurrentHeap;
	/// Start position in the heap
	D3D12_CPU_DESCRIPTOR_HANDLE mStartCpuHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE mStartGpuHandle;
} DescriptorStoreHeap;

/// Descriptor table structure holding the native descriptor set handle
typedef struct DescriptorTable
{
	/// Handle to the start of the cbv_srv_uav descriptor table in the gpu visible cbv_srv_uav heap
	D3D12_CPU_DESCRIPTOR_HANDLE mBaseCpuHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE mBaseGpuHandle;
	uint32_t                    mDescriptorCount;
	uint32_t                    mNodeIndex;
} DescriptorTable;

#define MAX_FRAMES_IN_FLIGHT 3U
//using HashMap = eastl::unordered_map<uint64_t, uint32_t>;

using DescriptorBinderMap = eastl::hash_map<const RootSignature*, struct DescriptorBinderNode*>;

typedef struct DescriptorBinder
{
	DescriptorStoreHeap* pCbvSrvUavHeap[MAX_GPUS];
	DescriptorStoreHeap* pSamplerHeap[MAX_GPUS];
	DescriptorBinderMap  mRootSignatureNodes;
} DescriptorBinder;

#ifndef ENABLE_RENDERER_RUNTIME_SWITCH
extern void addBuffer(Renderer* pRenderer, const BufferDesc* desc, Buffer** pp_buffer);
extern void removeBuffer(Renderer* pRenderer, Buffer* p_buffer);
#endif

extern void add_descriptor_heap(Renderer* pRenderer, D3D12_DESCRIPTOR_HEAP_TYPE type, D3D12_DESCRIPTOR_HEAP_FLAGS flags, uint32_t numDescriptors, struct DescriptorStoreHeap** ppDescHeap);
extern void reset_descriptor_heap(struct DescriptorStoreHeap* pHeap);
extern void remove_descriptor_heap(struct DescriptorStoreHeap* pHeap);
extern D3D12_CPU_DESCRIPTOR_HANDLE add_cpu_descriptor_handles(struct DescriptorStoreHeap* pHeap, uint32_t numDescriptors, uint32_t* pDescriptorIndex = NULL);
extern void add_gpu_descriptor_handles(struct DescriptorStoreHeap* pHeap, D3D12_CPU_DESCRIPTOR_HANDLE* pStartCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* pStartGpuHandle, uint32_t numDescriptors);
extern void remove_gpu_descriptor_handles(DescriptorStoreHeap* pHeap, D3D12_GPU_DESCRIPTOR_HANDLE* startHandle, uint64_t numDescriptors);

// Enable experimental features and return if they are supported.
// To test them being supported we need to check both their enablement as well as device creation afterwards.
inline bool EnableD3D12ExperimentalFeatures(UUID* experimentalFeatures, uint32_t featureCount)
{
	ID3D12Device* testDevice = NULL;
	bool ret = SUCCEEDED(D3D12EnableExperimentalFeatures(featureCount, experimentalFeatures, NULL, NULL))
		&& SUCCEEDED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&testDevice)));
	if (ret)
		testDevice->Release();

	return ret;
}

// Enable experimental features required for compute-based raytracing fallback.
// This will set active D3D12 devices to DEVICE_REMOVED state.
// Returns bool whether the call succeeded and the device supports the feature.
inline bool EnableComputeRaytracingFallback()
{
	UUID experimentalFeatures[] = { D3D12ExperimentalShaderModels };
	return EnableD3D12ExperimentalFeatures(experimentalFeatures, 1);
}

/************************************************************************/
// Utility Functions Declarations
/************************************************************************/
D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS util_to_dx_acceleration_structure_build_flags(AccelerationStructureBuildFlags flags);
D3D12_RAYTRACING_GEOMETRY_FLAGS util_to_dx_geometry_flags(AccelerationStructureGeometryFlags flags);
D3D12_RAYTRACING_INSTANCE_FLAGS util_to_dx_instance_flags(AccelerationStructureInstanceFlags flags);
/************************************************************************/
// Forge Raytracing Implementation using DXR
/************************************************************************/
struct RaytracingShader
{
	Shader*		pShader;
};

struct AccelerationStructureBottom
{
	Buffer*											 pVertexBuffer;
	Buffer*											 pIndexBuffer;
	Buffer*											 pASBuffer;
	D3D12_RAYTRACING_GEOMETRY_DESC*					 pGeometryDescs;
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS mFlags;
	uint32_t											mDescCount;
};

struct AccelerationStructure
{
	Buffer*		pInstanceDescBuffer;
	Buffer*		pASBuffer;
	Buffer*		pScratchBuffer;
	AccelerationStructureBottom* ppBottomAS;
	uint32_t	mInstanceDescCount;
	uint32_t	mBottomASCount;
	uint32_t	mScratchBufferSize;
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS mFlags;
};

struct RaytracingShaderTable
{
	Pipeline*					pPipeline;
	Buffer*						pBuffer;
	D3D12_GPU_DESCRIPTOR_HANDLE mViewGpuDescriptorHandle[DESCRIPTOR_UPDATE_FREQ_COUNT];
	D3D12_GPU_DESCRIPTOR_HANDLE mSamplerGpuDescriptorHandle[DESCRIPTOR_UPDATE_FREQ_COUNT];
	uint32_t					mViewDescriptorCount[DESCRIPTOR_UPDATE_FREQ_COUNT];
	uint32_t					mSamplerDescriptorCount[DESCRIPTOR_UPDATE_FREQ_COUNT];
	uint64_t					mMaxEntrySize;
	uint64_t					mMissRecordSize;
	uint64_t					mHitGroupRecordSize;
};

#if defined(__cplusplus) && defined(ENABLE_RENDERER_RUNTIME_SWITCH)
namespace d3d12 {
#endif

bool isRaytracingSupported(Renderer* pRenderer)
{
	ASSERT(pRenderer);
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5 = {};
	pRenderer->pDxDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5));
	return (opts5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED);
}

bool initRaytracing(Renderer* pRenderer, Raytracing** ppRaytracing)
{
	ASSERT(pRenderer);
	ASSERT(ppRaytracing);

	if (!isRaytracingSupported(pRenderer)) return false;

	Raytracing* pRaytracing = (Raytracing*)conf_calloc(1, sizeof(*pRaytracing));
	ASSERT(pRaytracing);

	pRaytracing->pRenderer = pRenderer;
	pRenderer->pDxDevice->QueryInterface(&pRaytracing->pDxrDevice);

	*ppRaytracing = pRaytracing;
	return true;
}

void removeRaytracing(Renderer* pRenderer, Raytracing* pRaytracing)
{
	ASSERT(pRenderer);
	ASSERT(pRaytracing);

	if (pRaytracing->pDxrDevice)
		pRaytracing->pDxrDevice->Release();

	conf_free(pRaytracing);
}

Buffer* createGeomVertexBuffer(const AccelerationStructureGeometryDesc* desc)
{
	ASSERT(desc->pVertexArray);
	ASSERT(desc->vertexCount > 0);

	Buffer* result = NULL;
	BufferLoadDesc vbDesc = {};
	vbDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_VERTEX_BUFFER;
	vbDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
	vbDesc.mDesc.mSize = sizeof(float3) * desc->vertexCount;
	vbDesc.mDesc.mVertexStride = sizeof(float3);
	vbDesc.pData = desc->pVertexArray;
	vbDesc.ppBuffer = &result;
	addResource(&vbDesc);

	return result;
}

Buffer* createGeomIndexBuffer(const AccelerationStructureGeometryDesc* desc)
{
	ASSERT(desc->pIndices16 != NULL || desc->pIndices32 != NULL);
	ASSERT(desc->indicesCount > 0);

	Buffer* result = NULL;
	BufferLoadDesc indexBufferDesc = {};
	indexBufferDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_INDEX_BUFFER;
	indexBufferDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
	indexBufferDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_OWN_MEMORY_BIT;
	indexBufferDesc.mDesc.mSize = sizeof(uint) * desc->indicesCount;
	indexBufferDesc.mDesc.mIndexType = desc->indexType;
	indexBufferDesc.pData = desc->indexType == INDEX_TYPE_UINT32 ? (void*) desc->pIndices32 : (void*) desc->pIndices16;
	indexBufferDesc.ppBuffer = &result;
	addResource(&indexBufferDesc);

	return result;
}

AccelerationStructureBottom* createBottomAS(Raytracing* pRaytracing, const AccelerationStructureDescTop* pDesc, uint32_t* pScratchBufferSize)
{
	ASSERT(pRaytracing);
	ASSERT(pDesc);
	ASSERT(pScratchBufferSize);
	ASSERT(pDesc->mBottomASDescsCount > 0);

	uint32_t scratchBufferSize = 0;
	AccelerationStructureBottom* pResult = (AccelerationStructureBottom*)conf_calloc(pDesc->mBottomASDescsCount, sizeof(AccelerationStructureBottom));

	for (uint32_t i = 0; i < pDesc->mBottomASDescsCount; ++i)
	{
		pResult[i].mDescCount = pDesc->mBottomASDescs[i].mDescCount;
		pResult[i].mFlags = util_to_dx_acceleration_structure_build_flags(pDesc->mBottomASDescs[i].mFlags);
		pResult[i].pGeometryDescs = (D3D12_RAYTRACING_GEOMETRY_DESC*)conf_calloc(pResult[i].mDescCount, sizeof(D3D12_RAYTRACING_GEOMETRY_DESC));
		for (uint32_t j = 0; j < pResult[i].mDescCount; ++j)
		{
			AccelerationStructureGeometryDesc* pGeom = &pDesc->mBottomASDescs[i].pGeometryDescs[j];
			D3D12_RAYTRACING_GEOMETRY_DESC* pGeomD3D12 = &pResult[i].pGeometryDescs[j];

			pGeomD3D12->Flags = util_to_dx_geometry_flags(pGeom->mFlags);

			pResult[i].pIndexBuffer = NULL;
			if (pGeom->indicesCount > 0)
			{
				pResult[i].pIndexBuffer = createGeomIndexBuffer(pGeom);
				pGeomD3D12->Triangles.IndexBuffer = pResult[i].pIndexBuffer->mDxGpuAddress;
				pGeomD3D12->Triangles.IndexCount = (UINT)pResult[i].pIndexBuffer->mDesc.mSize /
					(pResult[i].pIndexBuffer->mDesc.mIndexType == INDEX_TYPE_UINT16 ? sizeof(uint16_t) : sizeof(uint32_t));
				pGeomD3D12->Triangles.IndexFormat = pResult[i].pIndexBuffer->mDxIndexFormat;
			}

			pResult[i].pVertexBuffer = createGeomVertexBuffer(pGeom);
			pGeomD3D12->Triangles.VertexBuffer.StartAddress = pResult[i].pVertexBuffer->mDxGpuAddress;
			pGeomD3D12->Triangles.VertexBuffer.StrideInBytes = (UINT)pResult[i].pVertexBuffer->mDesc.mVertexStride;
			pGeomD3D12->Triangles.VertexCount = (UINT)pResult[i].pVertexBuffer->mDesc.mSize / (UINT)pResult[i].pVertexBuffer->mDesc.mVertexStride;
			if (pGeomD3D12->Triangles.VertexBuffer.StrideInBytes == sizeof(float))
				pGeomD3D12->Triangles.VertexFormat = DXGI_FORMAT_R32_FLOAT;
			else if (pGeomD3D12->Triangles.VertexBuffer.StrideInBytes == sizeof(float) * 2)
				pGeomD3D12->Triangles.VertexFormat = DXGI_FORMAT_R32G32_FLOAT;
			else if (pGeomD3D12->Triangles.VertexBuffer.StrideInBytes == sizeof(float) * 3)
				pGeomD3D12->Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
			else if (pGeomD3D12->Triangles.VertexBuffer.StrideInBytes == sizeof(float) * 4)
				pGeomD3D12->Triangles.VertexFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;

		}

		/************************************************************************/
		// Get the size requirement for the Acceleration Structures
		/************************************************************************/
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS prebuildDesc = {};
		prebuildDesc.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		prebuildDesc.Flags = pResult[i].mFlags;
		prebuildDesc.NumDescs = pResult[i].mDescCount;
		prebuildDesc.pGeometryDescs = pResult[i].pGeometryDescs;
		prebuildDesc.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
		pRaytracing->pDxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&prebuildDesc, &info);

		/************************************************************************/
		// Allocate Acceleration Structure Buffer
		/************************************************************************/
		BufferDesc bufferDesc = {};
		bufferDesc.mDescriptors = DESCRIPTOR_TYPE_RW_BUFFER;
		bufferDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
		bufferDesc.mFlags = BUFFER_CREATION_FLAG_OWN_MEMORY_BIT | BUFFER_CREATION_FLAG_NO_DESCRIPTOR_VIEW_CREATION;
		bufferDesc.mStructStride = 0;
		bufferDesc.mFirstElement = 0;
		bufferDesc.mElementCount = info.ResultDataMaxSizeInBytes / sizeof(UINT32);
		bufferDesc.mSize = info.ResultDataMaxSizeInBytes; //Rustam: isn't this should be sizeof(UINT32) ?
		bufferDesc.mStartState = (ResourceState)D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
		addBuffer(pRaytracing->pRenderer, &bufferDesc, &pResult[i].pASBuffer);
		/************************************************************************/
		// Store the scratch buffer size so user can create the scratch buffer accordingly
		/************************************************************************/
		scratchBufferSize = (UINT)info.ScratchDataSizeInBytes > scratchBufferSize ? (UINT)info.ScratchDataSizeInBytes  : scratchBufferSize;
	}

	*pScratchBufferSize = scratchBufferSize;
	return pResult;
}

Buffer* createTopAS(Raytracing* pRaytracing, const AccelerationStructureDescTop* pDesc, const AccelerationStructureBottom* pASBottom, uint32_t* pScratchBufferSize, Buffer** ppInstanceDescBuffer)
{
	ASSERT(pRaytracing);
	ASSERT(pDesc);
	ASSERT(pScratchBufferSize);
	ASSERT(pASBottom);
	ASSERT(ppInstanceDescBuffer);
	/************************************************************************/
	// Get the size requirement for the Acceleration Structures
	/************************************************************************/
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS prebuildDesc = {};
	prebuildDesc.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	prebuildDesc.Flags = util_to_dx_acceleration_structure_build_flags(pDesc->mFlags);
	prebuildDesc.NumDescs = pDesc->mInstancesDescCount;
	prebuildDesc.pGeometryDescs = NULL;
	prebuildDesc.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
	pRaytracing->pDxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&prebuildDesc, &info);

	/************************************************************************/
	/*  Construct buffer with instances descriptions                        */
	/************************************************************************/
	eastl::vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs(pDesc->mInstancesDescCount);
	for (uint32_t i = 0; i < pDesc->mInstancesDescCount; ++i)
	{
		AccelerationStructureInstanceDesc* pInst = &pDesc->pInstanceDescs[i];
		Buffer* pASBuffer = pASBottom[pInst->mAccelerationStructureIndex].pASBuffer;
		instanceDescs[i].AccelerationStructure = pASBuffer->pDxResource->GetGPUVirtualAddress();
		instanceDescs[i].Flags = util_to_dx_instance_flags(pInst->mFlags);
		instanceDescs[i].InstanceContributionToHitGroupIndex = pInst->mInstanceContributionToHitGroupIndex;
		instanceDescs[i].InstanceID = pInst->mInstanceID;
		instanceDescs[i].InstanceMask = pInst->mInstanceMask;

		memcpy(instanceDescs[i].Transform, pInst->mTransform, sizeof(float[12]));
	}

	BufferDesc instanceDesc = {};
	instanceDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
	instanceDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
	instanceDesc.mSize = instanceDescs.size() * sizeof(instanceDescs[0]);
	Buffer* pInstanceDescBuffer = NULL;
	addBuffer(pRaytracing->pRenderer, &instanceDesc, &pInstanceDescBuffer);
	memcpy(pInstanceDescBuffer->pCpuMappedAddress, instanceDescs.data(), instanceDesc.mSize);
	/************************************************************************/
	// Allocate Acceleration Structure Buffer
	/************************************************************************/
	BufferDesc bufferDesc = {};
	bufferDesc.mDescriptors = DESCRIPTOR_TYPE_RW_BUFFER_RAW | DESCRIPTOR_TYPE_BUFFER_RAW;
	bufferDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
	bufferDesc.mFlags = BUFFER_CREATION_FLAG_OWN_MEMORY_BIT;
	bufferDesc.mStructStride = 0;
	bufferDesc.mFirstElement = 0;
	bufferDesc.mElementCount = info.ResultDataMaxSizeInBytes / sizeof(UINT32);
	bufferDesc.mSize = info.ResultDataMaxSizeInBytes;
	bufferDesc.mStartState = (ResourceState)D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
	Buffer* pTopASBuffer = NULL;
	addBuffer(pRaytracing->pRenderer, &bufferDesc, &pTopASBuffer);

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.RaytracingAccelerationStructure.Location = pTopASBuffer->mDxGpuAddress;
	pRaytracing->pRenderer->pDxDevice->CreateShaderResourceView(NULL, &srvDesc, pTopASBuffer->mDxSrvHandle);

	*pScratchBufferSize = (UINT)info.ScratchDataSizeInBytes;
	*ppInstanceDescBuffer = pInstanceDescBuffer;
	return pTopASBuffer;
}

void addAccelerationStructure(Raytracing* pRaytracing, const AccelerationStructureDescTop* pDesc, AccelerationStructure** ppAccelerationStructure)
{
	ASSERT(pRaytracing);
	ASSERT(pDesc);
	ASSERT(ppAccelerationStructure);

	AccelerationStructure* pAccelerationStructure = (AccelerationStructure*)conf_calloc(1, sizeof(*pAccelerationStructure));
	ASSERT(pAccelerationStructure);

	uint32_t scratchBottomBufferSize = 0;
	pAccelerationStructure->mBottomASCount = pDesc->mBottomASDescsCount;
	pAccelerationStructure->ppBottomAS = createBottomAS(pRaytracing, pDesc, &scratchBottomBufferSize);
	
	uint32_t scratchTopBufferSize = 0;
	pAccelerationStructure->mInstanceDescCount = pDesc->mInstancesDescCount;
	pAccelerationStructure->pASBuffer = createTopAS(pRaytracing, pDesc, pAccelerationStructure->ppBottomAS, &scratchTopBufferSize, &pAccelerationStructure->pInstanceDescBuffer);
	pAccelerationStructure->mScratchBufferSize = max(scratchBottomBufferSize, scratchTopBufferSize);
	pAccelerationStructure->mFlags = util_to_dx_acceleration_structure_build_flags(pDesc->mFlags);

	//Create scratch buffer
	BufferLoadDesc scratchBufferDesc = {};
	scratchBufferDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_RW_BUFFER;
	scratchBufferDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
	scratchBufferDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_NO_DESCRIPTOR_VIEW_CREATION;
	scratchBufferDesc.mDesc.mSize = pAccelerationStructure->mScratchBufferSize;
	scratchBufferDesc.ppBuffer = &pAccelerationStructure->pScratchBuffer;
	addResource(&scratchBufferDesc);

	*ppAccelerationStructure = pAccelerationStructure;
}

void removeAccelerationStructure(Raytracing* pRaytracing, AccelerationStructure* pAccelerationStructure)
{
	ASSERT(pRaytracing);
	ASSERT(pAccelerationStructure);

	removeBuffer(pRaytracing->pRenderer, pAccelerationStructure->pASBuffer);
	removeBuffer(pRaytracing->pRenderer, pAccelerationStructure->pInstanceDescBuffer);
	removeBuffer(pRaytracing->pRenderer, pAccelerationStructure->pScratchBuffer);

	for (unsigned i = 0; i < pAccelerationStructure->mBottomASCount; ++i)
	{
		removeBuffer(pRaytracing->pRenderer, pAccelerationStructure->ppBottomAS[i].pASBuffer);
		removeBuffer(pRaytracing->pRenderer, pAccelerationStructure->ppBottomAS[i].pVertexBuffer);
		if (pAccelerationStructure->ppBottomAS[i].pIndexBuffer != NULL)
			removeBuffer(pRaytracing->pRenderer, pAccelerationStructure->ppBottomAS[i].pIndexBuffer);
		conf_free(pAccelerationStructure->ppBottomAS[i].pGeometryDescs);
	}
	conf_free(pAccelerationStructure->ppBottomAS);
	conf_free(pAccelerationStructure);
}

void addRaytracingShader(Raytracing* pRaytracing, const unsigned char* pByteCode, unsigned byteCodeSize, const char* pName, RaytracingShader** ppShader)
{
	ASSERT(pRaytracing);
	ASSERT(pByteCode);
	ASSERT(byteCodeSize);
	ASSERT(pName);
	ASSERT(ppShader);
	
	RaytracingShader* pShader = (RaytracingShader*)conf_calloc(1, sizeof(*pShader));
	ASSERT(pShader);

	{
		ShaderLoadDesc desc = {};
		desc.mStages[0] = { (char*)pByteCode, NULL, 0, FSR_SrcShaders };
		desc.mTarget = shader_target_6_3;

		addShader(pRaytracing->pRenderer, &desc, &pShader->pShader);
	}

	*ppShader = pShader;
}

void removeRaytracingShader(Raytracing* pRaytracing, RaytracingShader* pShader)
{
	ASSERT(pRaytracing);
	ASSERT(pShader);

	removeShader(pRaytracing->pRenderer, pShader->pShader);
	conf_free(pShader);
}

static const uint64_t gShaderIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;

void FillShaderIdentifiers(	const RaytracingShaderTableRecordDesc* pRecords, uint32_t shaderCount, 
							ID3D12StateObjectProperties* pRtsoProps, uint64_t& maxShaderTableSize,
							uint32_t& index, RaytracingShaderTable* pTable, Raytracing* pRaytracing)
{
	for (uint32_t i = 0; i < shaderCount; ++i)
	{
		eastl::hash_set<uint32_t> addedTables;

		const RaytracingShaderTableRecordDesc* pRecord = &pRecords[i];
		void* pIdentifier = NULL;
		WCHAR* pName = (WCHAR*)conf_calloc(strlen(pRecord->pName) + 1, sizeof(WCHAR));
		mbstowcs(pName, pRecord->pName, strlen(pRecord->pName));

		pIdentifier = pRtsoProps->GetShaderIdentifier(pName);

		ASSERT(pIdentifier);
		conf_free(pName);

		uint64_t currentPosition = maxShaderTableSize * index++;
		memcpy((uint8_t*)pTable->pBuffer->pCpuMappedAddress + currentPosition, pIdentifier, gShaderIdentifierSize);

		// #TODO
		//if (!pRecord->pRootSignature)
		//	continue;

		currentPosition += gShaderIdentifierSize;
		/************************************************************************/
		// #NOTE : User can specify root data in any order but we need to fill
		// it into the buffer based on the root index associated with each root data entry
		// So we collect them here and do a lookup when looping through the descriptor array
		// from the root signature
		/************************************************************************/
		// #TODO
		//eastl::string_hash_map<const DescriptorData*> data;
		//for (uint32_t desc = 0; desc < pRecord->mRootDataCount; ++desc)
		//{
		//	data.insert(pRecord->pRootData[desc].pName, &pRecord->pRootData[desc]);
		//}

		//for (uint32_t desc = 0; desc < pRecord->pRootSignature->mDescriptorCount; ++desc)
		//{
		//	uint32_t descIndex = -1;
		//	const DescriptorInfo* pDesc = &pRecord->pRootSignature->pDescriptors[desc];
		//	eastl::string_hash_map<const DescriptorData*>::iterator it = data.find(pDesc->mDesc.name);
		//	const DescriptorData* pData = it->second;

		//	switch (pDesc->mDxType)
		//	{
		//	case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
		//	{
		//		memcpy((uint8_t*)pTable->pBuffer->pCpuMappedAddress + currentPosition, pData->pRootConstant, pDesc->mDesc.size * sizeof(uint32_t));
		//		currentPosition += pDesc->mDesc.size * sizeof(uint32_t);
		//		break;
		//	}
		//	case D3D12_ROOT_PARAMETER_TYPE_CBV:
		//	case D3D12_ROOT_PARAMETER_TYPE_SRV:
		//	case D3D12_ROOT_PARAMETER_TYPE_UAV:
		//	{
		//		// Root Descriptors need to be aligned to 8 byte address
		//		currentPosition = round_up_64(currentPosition, gLocalRootDescriptorSize);
		//		uint64_t offset = pData->pOffsets ? pData->pOffsets[0] : 0;
		//		D3D12_GPU_VIRTUAL_ADDRESS cbvAddress = pData->ppBuffers[0]->pDxResource->GetGPUVirtualAddress() + pData->ppBuffers[0]->mPositionInHeap + offset;
		//		memcpy((uint8_t*)pTable->pBuffer->pCpuMappedAddress + currentPosition, &cbvAddress, sizeof(D3D12_GPU_VIRTUAL_ADDRESS));
		//		currentPosition += gLocalRootDescriptorSize;
		//		break;
		//	}
		//	default:
		//		break;
		//	}
		//}
	}
}

void CalculateMaxShaderRecordSize(const RaytracingShaderTableRecordDesc* pRecords, uint32_t shaderCount, uint64_t& maxShaderTableSize)
{
	// #TODO
	//for (uint32_t i = 0; i < shaderCount; ++i)
	//{
	//	eastl::hash_set<uint32_t> addedTables;
	//	const RaytracingShaderTableRecordDesc* pRecord = &pRecords[i];
	//	uint32_t shaderSize = 0;
	//	for (uint32_t desc = 0; desc < pRecord->mRootDataCount; ++desc)
	//	{
	//		uint32_t descIndex = -1;
	//		const DescriptorInfo* pDesc = get_descriptor(pRecord->pRootSignature, pRecord->pRootData[desc].pName, &descIndex);
	//		ASSERT(pDesc);

	//		switch (pDesc->mDxType)
	//		{
	//		case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
	//			shaderSize += pDesc->mDesc.size * gLocalRootConstantSize;
	//			break;
	//		case D3D12_ROOT_PARAMETER_TYPE_CBV:
	//		case D3D12_ROOT_PARAMETER_TYPE_SRV:
	//		case D3D12_ROOT_PARAMETER_TYPE_UAV:
	//			shaderSize += gLocalRootDescriptorSize;
	//			break;
	//		case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
	//		{
	//			const uint32_t rootIndex = pDesc->mDesc.type == DESCRIPTOR_TYPE_SAMPLER ?
	//				pRecord->pRootSignature->mDxSamplerDescriptorTableRootIndices[pDesc->mUpdateFrquency] :
	//				pRecord->pRootSignature->mDxViewDescriptorTableRootIndices[pDesc->mUpdateFrquency];
	//			if (addedTables.find(rootIndex) == addedTables.end())
	//				shaderSize += gLocalRootDescriptorTableSize;
	//			else
	//				addedTables.insert(rootIndex);
	//			break;
	//		}
	//		default:
	//			break;
	//		}
	//	}

	//	maxShaderTableSize = max(maxShaderTableSize, shaderSize);
	//}
}

void addRaytracingShaderTable(Raytracing* pRaytracing, const RaytracingShaderTableDesc* pDesc, RaytracingShaderTable** ppTable)
{
	ASSERT(pRaytracing);
	ASSERT(pDesc);
	ASSERT(pDesc->pPipeline);
	ASSERT(ppTable);
	ASSERT(pDesc->pRayGenShader);

	RaytracingShaderTable* pTable = (RaytracingShaderTable*)conf_calloc(1, sizeof(*pTable));
	conf_placement_new<RaytracingShaderTable>((void*)pTable);
	ASSERT(pTable);

	pTable->pPipeline = pDesc->pPipeline;

	const uint32_t rayGenShaderCount = 1;
	const uint32_t recordCount = rayGenShaderCount + pDesc->mMissShaderCount + pDesc->mHitGroupCount;
	uint64_t maxShaderTableSize = 0;
	/************************************************************************/
	// Calculate max size for each element in the shader table
	/************************************************************************/
	CalculateMaxShaderRecordSize(pDesc->pRayGenShader, 1, maxShaderTableSize);
	CalculateMaxShaderRecordSize(pDesc->pMissShaders, pDesc->mMissShaderCount, maxShaderTableSize);
	CalculateMaxShaderRecordSize(pDesc->pHitGroups, pDesc->mHitGroupCount, maxShaderTableSize);
	/************************************************************************/
	// Align max size
	/************************************************************************/
	maxShaderTableSize = round_up_64(gShaderIdentifierSize + maxShaderTableSize, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
	pTable->mMaxEntrySize = maxShaderTableSize;
	/************************************************************************/
	// Create shader table buffer
	/************************************************************************/
	BufferDesc bufferDesc = {};
	bufferDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
	bufferDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
	bufferDesc.mSize = maxShaderTableSize * recordCount;
	bufferDesc.pDebugName = L"RTShadersTable";
	addBuffer(pRaytracing->pRenderer, &bufferDesc, &pTable->pBuffer);
	/************************************************************************/
	// Copy shader identifiers into the buffer
	/************************************************************************/
	ID3D12StateObjectProperties* pRtsoProps = NULL;
	pDesc->pPipeline->pDxrPipeline->QueryInterface(IID_PPV_ARGS(&pRtsoProps));
	   
	uint32_t index = 0;
	FillShaderIdentifiers(	pDesc->pRayGenShader, 1, pRtsoProps,
							maxShaderTableSize, index, pTable, pRaytracing);

	pTable->mMissRecordSize = maxShaderTableSize * pDesc->mMissShaderCount;
	FillShaderIdentifiers(	pDesc->pMissShaders, pDesc->mMissShaderCount, pRtsoProps, 
							maxShaderTableSize, index, pTable, pRaytracing);

	pTable->mHitGroupRecordSize = maxShaderTableSize * pDesc->mHitGroupCount;
	FillShaderIdentifiers(	pDesc->pHitGroups, pDesc->mHitGroupCount, pRtsoProps, 
							maxShaderTableSize, index, pTable, pRaytracing);

	if (pRtsoProps)
		pRtsoProps->Release();
	/************************************************************************/
	/************************************************************************/

	*ppTable = pTable;
}

void removeRaytracingShaderTable(Raytracing* pRaytracing, RaytracingShaderTable* pTable)
{
	ASSERT(pRaytracing);
	ASSERT(pTable);

	removeBuffer(pRaytracing->pRenderer, pTable->pBuffer);

	pTable->~RaytracingShaderTable();
	conf_free(pTable);
}

/************************************************************************/
// Raytracing Command Buffer Functions Implementation
/************************************************************************/
void util_build_acceleration_structure(ID3D12GraphicsCommandList4* pDxrCmd, ID3D12Resource* pScratchBuffer, ID3D12Resource* pASBuffer,
									D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE ASType, 
									D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS ASFlags,
									const D3D12_RAYTRACING_GEOMETRY_DESC * pGeometryDescs,
									D3D12_GPU_VIRTUAL_ADDRESS pInstanceDescBuffer,
									uint32_t descCount)
{
	ASSERT(pDxrCmd);

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
	buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	buildDesc.Inputs.Type = ASType;
	buildDesc.DestAccelerationStructureData = pASBuffer->GetGPUVirtualAddress();
	buildDesc.Inputs.Flags = ASFlags;
	buildDesc.Inputs.pGeometryDescs = NULL;

	if (ASType == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL)
		buildDesc.Inputs.pGeometryDescs = pGeometryDescs;
	else if (ASType == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL)
		buildDesc.Inputs.InstanceDescs = pInstanceDescBuffer;

	buildDesc.Inputs.NumDescs = descCount;
	buildDesc.ScratchAccelerationStructureData = pScratchBuffer->GetGPUVirtualAddress();

	pDxrCmd->BuildRaytracingAccelerationStructure(&buildDesc, 0, NULL);

	D3D12_RESOURCE_BARRIER uavBarrier = {};
	uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarrier.UAV.pResource = pASBuffer;
	pDxrCmd->ResourceBarrier(1, &uavBarrier);
}

void cmdBuildAccelerationStructure(Cmd* pCmd, Raytracing* pRaytracing, RaytracingBuildASDesc* pDesc)
{
	ASSERT(pDesc);
	ASSERT(pDesc->pAccelerationStructure);

	AccelerationStructure* pAccelerationStructure = pDesc->pAccelerationStructure;
	ID3D12GraphicsCommandList4* pDxrCmd = NULL;
	pCmd->pDxCmdList->QueryInterface(&pDxrCmd);
	ASSERT(pDxrCmd);

	for (uint32_t i = 0; i < pDesc->mBottomASIndicesCount; ++i)
	{
		uint32_t index = pDesc->pBottomASIndices[i];

		util_build_acceleration_structure(pDxrCmd,
			pAccelerationStructure->pScratchBuffer->pDxResource,
			pAccelerationStructure->ppBottomAS[index].pASBuffer->pDxResource,
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
			pAccelerationStructure->ppBottomAS[index].mFlags,
			pAccelerationStructure->ppBottomAS[index].pGeometryDescs,
			NULL, pAccelerationStructure->ppBottomAS[index].mDescCount);
	}

	util_build_acceleration_structure(pDxrCmd,
		pAccelerationStructure->pScratchBuffer->pDxResource,
		pAccelerationStructure->pASBuffer->pDxResource,
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL,
		pAccelerationStructure->mFlags,
		NULL,
		pAccelerationStructure->pInstanceDescBuffer->pDxResource->GetGPUVirtualAddress() + pAccelerationStructure->pInstanceDescBuffer->mPositionInHeap,
		pAccelerationStructure->mInstanceDescCount);

	pDxrCmd->Release();
}

void cmdDispatchRays(Cmd* pCmd, Raytracing* pRaytracing, const RaytracingDispatchDesc* pDesc)
{
	const RaytracingShaderTable* pShaderTable = pDesc->pShaderTable;
	/************************************************************************/
	// Compute shader table GPU addresses
	// #TODO: Support for different offsets into the shader table
	/************************************************************************/
	D3D12_GPU_VIRTUAL_ADDRESS startAddress = pDesc->pShaderTable->pBuffer->pDxResource->GetGPUVirtualAddress() + pShaderTable->pBuffer->mPositionInHeap;

	D3D12_GPU_VIRTUAL_ADDRESS_RANGE rayGenShaderRecord = {};
	rayGenShaderRecord.SizeInBytes = pShaderTable->mMaxEntrySize;
	rayGenShaderRecord.StartAddress = startAddress + pShaderTable->mMaxEntrySize * 0;

	D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE missShaderTable = {};
	missShaderTable.SizeInBytes = pShaderTable->mMissRecordSize;
	missShaderTable.StartAddress = startAddress + pShaderTable->mMaxEntrySize;
	missShaderTable.StrideInBytes = pShaderTable->mMaxEntrySize;

	D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE hitGroupTable = {};
	hitGroupTable.SizeInBytes = pShaderTable->mHitGroupRecordSize;
	hitGroupTable.StartAddress = startAddress + pShaderTable->mMaxEntrySize + pShaderTable->mMissRecordSize;
	hitGroupTable.StrideInBytes = pShaderTable->mMaxEntrySize;
	/************************************************************************/
	/************************************************************************/
	D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
	dispatchDesc.Height = pDesc->mHeight;
	dispatchDesc.Width = pDesc->mWidth;
	dispatchDesc.Depth = 1;
	dispatchDesc.RayGenerationShaderRecord = rayGenShaderRecord;
	dispatchDesc.MissShaderTable = missShaderTable;
	dispatchDesc.HitGroupTable = hitGroupTable;

	ID3D12GraphicsCommandList4* pDxrCmd = NULL;
	pCmd->pDxCmdList->QueryInterface(&pDxrCmd);
	pDxrCmd->SetPipelineState1(pShaderTable->pPipeline->pDxrPipeline);
	pDxrCmd->DispatchRays(&dispatchDesc);
	pDxrCmd->Release();
}

#if defined(__cplusplus) && defined(ENABLE_RENDERER_RUNTIME_SWITCH)
}
#endif
/************************************************************************/
// Utility Functions Implementation
/************************************************************************/
D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS util_to_dx_acceleration_structure_build_flags(AccelerationStructureBuildFlags flags)
{
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS ret = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
	if (flags & ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION)
		ret |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
	if (flags & ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE)
		ret |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
	if (flags & ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY)
		ret |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY;
	if (flags & ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE)
		ret |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
	if (flags & ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD)
		ret |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
	if (flags & ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE)
		ret |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

	return ret;
}

D3D12_RAYTRACING_GEOMETRY_FLAGS util_to_dx_geometry_flags(AccelerationStructureGeometryFlags flags)
{
	D3D12_RAYTRACING_GEOMETRY_FLAGS ret = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
	if (flags & ACCELERATION_STRUCTURE_GEOMETRY_FLAG_OPAQUE)
		ret |= D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
	if (flags & ACCELERATION_STRUCTURE_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION)
		ret |= D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION;

	return ret;
}

D3D12_RAYTRACING_INSTANCE_FLAGS util_to_dx_instance_flags(AccelerationStructureInstanceFlags flags)
{
	D3D12_RAYTRACING_INSTANCE_FLAGS ret = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
	if (flags & ACCELERATION_STRUCTURE_INSTANCE_FLAG_FORCE_OPAQUE)
		ret |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;
	if (flags & ACCELERATION_STRUCTURE_INSTANCE_FLAG_FORCE_OPAQUE)
		ret |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
	if (flags & ACCELERATION_STRUCTURE_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE)
		ret |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
	if (flags & ACCELERATION_STRUCTURE_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE)
		ret |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;

	return ret;
}

void d3d12_addRaytracingPipeline(const RaytracingPipelineDesc* pDesc, Pipeline** ppPipeline)
{
	Raytracing* pRaytracing = pDesc->pRaytracing;

	ASSERT(pRaytracing);
	ASSERT(pDesc);
	ASSERT(ppPipeline);

	Pipeline* pPipeline = (Pipeline*)conf_calloc(1, sizeof(*pPipeline));
	ASSERT(pPipeline);

	pPipeline->mType = PIPELINE_TYPE_RAYTRACING;
	/************************************************************************/
	// Pipeline Creation
	/************************************************************************/
	eastl::vector<D3D12_STATE_SUBOBJECT>                                           subobjects;
	eastl::vector<eastl::pair<uint32_t, D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*> > exportAssociationsDelayed;
	// Reserve average number of subobject space in the beginning
	subobjects.reserve(10);
	/************************************************************************/
	// Step 1 - Create DXIL Libraries
	/************************************************************************/
	eastl::vector<D3D12_DXIL_LIBRARY_DESC> dxilLibDescs;
	eastl::vector<D3D12_STATE_SUBOBJECT>   stateSubobject = {};
	eastl::vector<D3D12_EXPORT_DESC*>      exportDesc = {};

	D3D12_DXIL_LIBRARY_DESC rayGenDesc = {};
	D3D12_EXPORT_DESC       rayGenExportDesc = {};
	rayGenExportDesc.ExportToRename = NULL;
	rayGenExportDesc.Flags = D3D12_EXPORT_FLAG_NONE;
	rayGenExportDesc.Name = pDesc->pRayGenShader->pEntryNames[0];

	rayGenDesc.DXILLibrary.BytecodeLength = pDesc->pRayGenShader->pShaderBlobs[0]->GetBufferSize();
	rayGenDesc.DXILLibrary.pShaderBytecode = pDesc->pRayGenShader->pShaderBlobs[0]->GetBufferPointer();
	rayGenDesc.NumExports = 1;
	rayGenDesc.pExports = &rayGenExportDesc;

	dxilLibDescs.emplace_back(rayGenDesc);

	eastl::vector<LPCWSTR> missShadersEntries(pDesc->mMissShaderCount);
	for (uint32_t i = 0; i < pDesc->mMissShaderCount; ++i)
	{
		D3D12_EXPORT_DESC* pMissExportDesc = (D3D12_EXPORT_DESC*)conf_calloc(1, sizeof(*pMissExportDesc));
		pMissExportDesc->ExportToRename = NULL;
		pMissExportDesc->Flags = D3D12_EXPORT_FLAG_NONE;

		pMissExportDesc->Name = pDesc->ppMissShaders[i]->pEntryNames[0];
		missShadersEntries[i] = pMissExportDesc->Name;

		D3D12_DXIL_LIBRARY_DESC missDesc = {};
		missDesc.DXILLibrary.BytecodeLength = pDesc->ppMissShaders[i]->pShaderBlobs[0]->GetBufferSize();
		missDesc.DXILLibrary.pShaderBytecode = pDesc->ppMissShaders[i]->pShaderBlobs[0]->GetBufferPointer();
		missDesc.NumExports = 1;
		missDesc.pExports = pMissExportDesc;

		exportDesc.emplace_back(pMissExportDesc);
		dxilLibDescs.emplace_back(missDesc);
	}

	eastl::vector<LPCWSTR> hitGroupsIntersectionsEntries(pDesc->mHitGroupCount);
	eastl::vector<LPCWSTR> hitGroupsAnyHitEntries(pDesc->mHitGroupCount);
	eastl::vector<LPCWSTR> hitGroupsClosestHitEntries(pDesc->mHitGroupCount);
	for (uint32_t i = 0; i < pDesc->mHitGroupCount; ++i)
	{
		if (pDesc->pHitGroups[i].pIntersectionShader)
		{
			D3D12_EXPORT_DESC* pIntersectionExportDesc = (D3D12_EXPORT_DESC*)conf_calloc(1, sizeof(*pIntersectionExportDesc));
			pIntersectionExportDesc->ExportToRename = NULL;
			pIntersectionExportDesc->Flags = D3D12_EXPORT_FLAG_NONE;
			pIntersectionExportDesc->Name = pDesc->pHitGroups[i].pIntersectionShader->pEntryNames[0];
			hitGroupsIntersectionsEntries[i] = pIntersectionExportDesc->Name;

			D3D12_DXIL_LIBRARY_DESC intersectionDesc = {};
			intersectionDesc.DXILLibrary.BytecodeLength = pDesc->pHitGroups[i].pIntersectionShader->pShaderBlobs[0]->GetBufferSize();
			intersectionDesc.DXILLibrary.pShaderBytecode = pDesc->pHitGroups[i].pIntersectionShader->pShaderBlobs[0]->GetBufferPointer();
			intersectionDesc.NumExports = 1;
			intersectionDesc.pExports = pIntersectionExportDesc;

			exportDesc.emplace_back(pIntersectionExportDesc);
			dxilLibDescs.emplace_back(intersectionDesc);
		}
		if (pDesc->pHitGroups[i].pAnyHitShader)
		{
			D3D12_EXPORT_DESC* pAnyHitExportDesc = (D3D12_EXPORT_DESC*)conf_calloc(1, sizeof(*pAnyHitExportDesc));
			pAnyHitExportDesc->ExportToRename = NULL;
			pAnyHitExportDesc->Flags = D3D12_EXPORT_FLAG_NONE;
			pAnyHitExportDesc->Name = pDesc->pHitGroups[i].pAnyHitShader->pEntryNames[0];
			hitGroupsAnyHitEntries[i] = pAnyHitExportDesc->Name;

			D3D12_DXIL_LIBRARY_DESC anyHitDesc = {};
			anyHitDesc.DXILLibrary.BytecodeLength = pDesc->pHitGroups[i].pAnyHitShader->pShaderBlobs[0]->GetBufferSize();
			anyHitDesc.DXILLibrary.pShaderBytecode = pDesc->pHitGroups[i].pAnyHitShader->pShaderBlobs[0]->GetBufferPointer();
			anyHitDesc.NumExports = 1;
			anyHitDesc.pExports = pAnyHitExportDesc;

			exportDesc.emplace_back(pAnyHitExportDesc);
			dxilLibDescs.emplace_back(anyHitDesc);
		}
		if (pDesc->pHitGroups[i].pClosestHitShader)
		{
			D3D12_EXPORT_DESC* pClosestHitExportDesc = (D3D12_EXPORT_DESC*)conf_calloc(1, sizeof(*pClosestHitExportDesc));
			pClosestHitExportDesc->ExportToRename = NULL;
			pClosestHitExportDesc->Flags = D3D12_EXPORT_FLAG_NONE;
			pClosestHitExportDesc->Name = pDesc->pHitGroups[i].pClosestHitShader->pEntryNames[0];
			hitGroupsClosestHitEntries[i] = pClosestHitExportDesc->Name;

			D3D12_DXIL_LIBRARY_DESC closestHitDesc = {};
			closestHitDesc.DXILLibrary.BytecodeLength = pDesc->pHitGroups[i].pClosestHitShader->pShaderBlobs[0]->GetBufferSize();
			closestHitDesc.DXILLibrary.pShaderBytecode = pDesc->pHitGroups[i].pClosestHitShader->pShaderBlobs[0]->GetBufferPointer();
			closestHitDesc.NumExports = 1;
			closestHitDesc.pExports = pClosestHitExportDesc;

			exportDesc.emplace_back(pClosestHitExportDesc);
			dxilLibDescs.emplace_back(closestHitDesc);
		}
	}

	for (uint32_t i = 0; i < (uint32_t)dxilLibDescs.size(); ++i)
	{
		subobjects.emplace_back(D3D12_STATE_SUBOBJECT{ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &dxilLibDescs[i] });
	}
	/************************************************************************/
	// Step 2 - Create Hit Groups
	/************************************************************************/
	eastl::vector<D3D12_HIT_GROUP_DESC> hitGroupDescs(pDesc->mHitGroupCount);
	eastl::vector<WCHAR*>               hitGroupNames(pDesc->mHitGroupCount);

	for (uint32_t i = 0; i < pDesc->mHitGroupCount; ++i)
	{
		const RaytracingHitGroup* pHitGroup = &pDesc->pHitGroups[i];
		ASSERT(pDesc->pHitGroups[i].pHitGroupName);
		hitGroupNames[i] = (WCHAR*)conf_calloc(strlen(pDesc->pHitGroups[i].pHitGroupName) + 1, sizeof(WCHAR));
		mbstowcs(hitGroupNames[i], pDesc->pHitGroups[i].pHitGroupName, strlen(pDesc->pHitGroups[i].pHitGroupName));

		if (pHitGroup->pAnyHitShader)
		{
			hitGroupDescs[i].AnyHitShaderImport = hitGroupsAnyHitEntries[i];
		}
		else
			hitGroupDescs[i].AnyHitShaderImport = NULL;

		if (pHitGroup->pClosestHitShader)
		{
			hitGroupDescs[i].ClosestHitShaderImport = hitGroupsClosestHitEntries[i];
		}
		else
			hitGroupDescs[i].ClosestHitShaderImport = NULL;

		if (pHitGroup->pIntersectionShader)
		{
			hitGroupDescs[i].IntersectionShaderImport = hitGroupsIntersectionsEntries[i];
		}
		else
			hitGroupDescs[i].IntersectionShaderImport = NULL;

		hitGroupDescs[i].HitGroupExport = hitGroupNames[i];

		subobjects.emplace_back(D3D12_STATE_SUBOBJECT{ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hitGroupDescs[i] });
	}
	/************************************************************************/
	// Step 4 = Pipeline Config
	/************************************************************************/
	D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
	pipelineConfig.MaxTraceRecursionDepth = pDesc->mMaxTraceRecursionDepth;
	subobjects.emplace_back(D3D12_STATE_SUBOBJECT{ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig });
	/************************************************************************/
	// Step 5 - Global Root Signature
	/************************************************************************/
	subobjects.emplace_back(D3D12_STATE_SUBOBJECT{ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,
												   pDesc->pGlobalRootSignature ? &pDesc->pGlobalRootSignature->pDxRootSignature : NULL });
	/************************************************************************/
	// Step 6 - Local Root Signatures
	/************************************************************************/
	// Local Root Signature for Ray Generation Shader
	D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION rayGenRootSignatureAssociation = {};
	D3D12_LOCAL_ROOT_SIGNATURE             rayGenRSdesc = {};
	if (pDesc->pRayGenRootSignature)
	{
		rayGenRSdesc.pLocalRootSignature =
			pDesc->pRayGenRootSignature ? pDesc->pRayGenRootSignature->pDxRootSignature : pDesc->pEmptyRootSignature->pDxRootSignature;
		subobjects.emplace_back(D3D12_STATE_SUBOBJECT{ D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &rayGenRSdesc });

		rayGenRootSignatureAssociation.NumExports = 1;
		rayGenRootSignatureAssociation.pExports = &rayGenExportDesc.Name;

		exportAssociationsDelayed.push_back({ (uint32_t)subobjects.size() - 1, &rayGenRootSignatureAssociation });
	}

	// Local Root Signatures for Miss Shaders
	eastl::vector<D3D12_STATE_SUBOBJECT>                  missRootSignatures(pDesc->mMissShaderCount);
	eastl::vector<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION> missRootSignaturesAssociation(pDesc->mMissShaderCount);
	eastl::vector<D3D12_LOCAL_ROOT_SIGNATURE>             mMissShaderRSDescs(pDesc->mMissShaderCount);
	;
	for (uint32_t i = 0; i < pDesc->mMissShaderCount; ++i)
	{
		if (pDesc->ppMissRootSignatures && pDesc->ppMissRootSignatures[i])
		{
			missRootSignatures[i].Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
			if (pDesc->ppMissRootSignatures && pDesc->ppMissRootSignatures[i])
				mMissShaderRSDescs[i].pLocalRootSignature = pDesc->ppMissRootSignatures[i]->pDxRootSignature;
			else
				mMissShaderRSDescs[i].pLocalRootSignature = pDesc->pEmptyRootSignature->pDxRootSignature;
			missRootSignatures[i].pDesc = &mMissShaderRSDescs[i];
			subobjects.emplace_back(missRootSignatures[i]);

			missRootSignaturesAssociation[i].NumExports = 1;
			missRootSignaturesAssociation[i].pExports = &missShadersEntries[i];

			exportAssociationsDelayed.push_back({ (uint32_t)subobjects.size() - 1, &missRootSignaturesAssociation[i] });
		}
	}

	// Local Root Signatures for Hit Groups
	eastl::vector<D3D12_STATE_SUBOBJECT>                  hitGroupRootSignatures(pDesc->mHitGroupCount);
	eastl::vector<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION> hitGroupRootSignatureAssociation(pDesc->mHitGroupCount);
	eastl::vector<D3D12_LOCAL_ROOT_SIGNATURE>             hitGroupRSDescs(pDesc->mHitGroupCount);
	for (uint32_t i = 0; i < pDesc->mHitGroupCount; ++i)
	{
		if (pDesc->pHitGroups[i].pRootSignature)
		{
			hitGroupRootSignatures[i].Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
			if (pDesc->pHitGroups[i].pRootSignature)
				hitGroupRSDescs[i].pLocalRootSignature = pDesc->pHitGroups[i].pRootSignature->pDxRootSignature;
			else
				hitGroupRSDescs[i].pLocalRootSignature = pDesc->pEmptyRootSignature->pDxRootSignature;
			hitGroupRootSignatures[i].pDesc = &hitGroupRSDescs[i];
			subobjects.emplace_back(hitGroupRootSignatures[i]);

			hitGroupRootSignatureAssociation[i].NumExports = 1;
			hitGroupRootSignatureAssociation[i].pExports = &hitGroupDescs[i].HitGroupExport;

			exportAssociationsDelayed.push_back({ (uint32_t)subobjects.size() - 1, &hitGroupRootSignatureAssociation[i] });
		}
	}
	/************************************************************************/
	// Shader Config
	/************************************************************************/
	D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
	shaderConfig.MaxAttributeSizeInBytes = pDesc->mAttributeSize;
	shaderConfig.MaxPayloadSizeInBytes = pDesc->mPayloadSize;
	subobjects.push_back(D3D12_STATE_SUBOBJECT{ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig });
	/************************************************************************/
	// Export Associations
	/************************************************************************/
	size_t base = subobjects.size(), numExportAssocs = exportAssociationsDelayed.size();
	subobjects.resize(base + numExportAssocs);
	for (size_t i = 0; i < numExportAssocs; ++i)
	{
		exportAssociationsDelayed[i].second->pSubobjectToAssociate = &subobjects[exportAssociationsDelayed[i].first];
		//D3D12_STATE_SUBOBJECT exportAssociationLocalRootSignature;
		//exportAssociationLocalRootSignature.Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
		//D3D12_LOCAL_ROOT_SIGNATURE desc;
		//desc.pLocalRootSignature = (ID3D12RootSignature *)subobjects[exportAssociationsDelayed[i].first].pDesc;
		//exportAssociationLocalRootSignature.pDesc = &desc;
		//subobjects.push_back(exportAssociationLocalRootSignature);

		subobjects[base + i].Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
		subobjects[base + i].pDesc = exportAssociationsDelayed[i].second;
	}
	/************************************************************************/
	// Step 7 - Create State Object
	/************************************************************************/
	D3D12_STATE_OBJECT_DESC pipelineDesc = {};
	pipelineDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
	pipelineDesc.NumSubobjects = (UINT)subobjects.size();
	pipelineDesc.pSubobjects = subobjects.data();

	// Create the state object.
	HRESULT hr = pRaytracing->pDxrDevice->CreateStateObject(&pipelineDesc, IID_PPV_ARGS(&pPipeline->pDxrPipeline));
	ASSERT(SUCCEEDED(hr));
	/************************************************************************/
	// Clean up
	/************************************************************************/
	for (uint32_t i = 0; i < (uint32_t)exportDesc.size(); ++i)
		conf_free(exportDesc[i]);

	for (uint32_t i = 0; i < (uint32_t)hitGroupNames.size(); ++i)
		conf_free(hitGroupNames[i]);
	/************************************************************************/
	/************************************************************************/
	*ppPipeline = pPipeline;
}

void d3d12_fillRaytracingRootDescriptorData(AccelerationStructure* pAccelerationStructure, D3D12_GPU_VIRTUAL_ADDRESS* pAddress)
{
	*pAddress = pAccelerationStructure->pASBuffer->pDxResource->GetGPUVirtualAddress();
}

void d3d12_fillRaytracingDescriptorHandle(AccelerationStructure* pAccelerationStructure, uint64_t* pHandle, uint64_t* pHash)
{
	*pHandle = pAccelerationStructure->pASBuffer->mDxSrvHandle.ptr;
	*pHash = eastl::mem_hash<uint64_t>()(&pAccelerationStructure->pASBuffer->mBufferId, 1, *pHash);
}

void d3d12_cmdBindRaytracingPipeline(Cmd* pCmd, Pipeline* pPipeline)
{
	ASSERT(pPipeline->pDxrPipeline);
	ID3D12GraphicsCommandList4* pDxrCmd = NULL;
	pCmd->pDxCmdList->QueryInterface(&pDxrCmd);
	pDxrCmd->SetPipelineState1(pPipeline->pDxrPipeline);
	pDxrCmd->Release();
}
#else
#if defined(__cplusplus) && defined(ENABLE_RENDERER_RUNTIME_SWITCH)
namespace d3d12 {
#endif

bool isRaytracingSupported(Renderer* pRenderer)
{
	return false;
}

bool initRaytracing(Renderer* pRenderer, Raytracing** ppRaytracing)
{
	return false;
}

void removeRaytracing(Renderer* pRenderer, Raytracing* pRaytracing)
{
}

void addAccelerationStructure(Raytracing* pRaytracing, const AccelerationStructureDescTop* pDesc, AccelerationStructure** ppAccelerationStructure)
{
}

void cmdBuildTopAS(Cmd* pCmd, Raytracing* pRaytracing, AccelerationStructure* pAccelerationStructure)
{
}

void cmdBuildBottomAS(Cmd* pCmd, Raytracing* pRaytracing, AccelerationStructure* pAccelerationStructure, unsigned bottomASIndex)
{
}

void cmdBuildAccelerationStructure(Cmd* pCmd, Raytracing* pRaytracing, RaytracingBuildASDesc* pDesc)
{
}

void addRaytracingShaderTable(Raytracing* pRaytracing, const RaytracingShaderTableDesc* pDesc, RaytracingShaderTable** ppTable)
{
}

void cmdDispatchRays(Cmd* pCmd, Raytracing* pRaytracing, const RaytracingDispatchDesc* pDesc)
{
}

void removeAccelerationStructure(Raytracing* pRaytracing, AccelerationStructure* pAccelerationStructure)
{
}

void removeRaytracingShaderTable(Raytracing* pRaytracing, RaytracingShaderTable* pTable)
{
}

#if defined(__cplusplus) && defined(ENABLE_RENDERER_RUNTIME_SWITCH)
}
#endif
#endif
#endif