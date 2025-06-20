//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************
#define _CRT_SECURE_NO_WARNINGS
#include "stdafx.h"
#include "D3D12HelloTriangle.h"
#include "DXRHelper.h"
#include "nv_helpers_dx12/BottomLevelASGenerator.h"
#include "nv_helpers_dx12/RaytracingPipelineGenerator.h"
#include "nv_helpers_dx12/RootSignatureGenerator.h"
#include "glm/gtc/type_ptr.hpp"
#include "manipulator.h"
#include "windowsx.h"

D3D12HelloTriangle::D3D12HelloTriangle(UINT width, UINT height, std::wstring name) :
    DXSample(width, height, name),
    m_frameIndex(0),
    m_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
    m_scissorRect(0, 0, static_cast<LONG>(width), static_cast<LONG>(height)),
    m_rtvDescriptorSize(0),
    uiConstructor(UIConstructor()),
    renderUI(false),
    materials({ Material() })
{
    uiConstructor.SetModelUpdateFunction(
        [this](std::vector<XMFLOAT3>& vertices, std::vector<UINT>& indices)
        {
            this->QueueModelVertexAndIndexBufferUpdates(vertices, indices);
        }
    );
}

void D3D12HelloTriangle::OnInit()
{
    uiConstructor.SetRenderingMode(!m_raster);
    //Setup for camera movement and rotation
    nv_helpers_dx12::CameraManip.setWindowSize(GetWidth(), GetHeight());
    nv_helpers_dx12::CameraManip.setLookat(glm::vec3(1.5f, 1.5f, 1.5f), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    LoadPipeline(); //Most of the code here was already here from Microsoft's D3D12HelloTriangle sample project.
    LoadAssets(); //Loads vertex and index data as well as camera data.
    CheckRaytracingSupport();
    // Setup the acceleration structures (AS) for raytracing. When setting up
    // geometry, each bottom-level AS has its own transform matrix.
    CreateAccelerationStructures();
    // Create the raytracing pipeline, associating the shader code to symbol names
    // and to their root signatures, and defining the amount of memory carried by
    // rays (ray payload)
    CreateRaytracingPipeline();
    // #DXR Extra: Per-Instance Data
    CreatePerInstanceConstantBuffers(); //This was used at some point but idk if it is still being used and I don't have time to figure it out and refactor.
    // #DXR Extra: Per-Instance Data
    // Create a constant buffer, with a color for each vertex of the triangle, for each triangle instance
    CreateGlobalConstantBuffer(); //Same thing as per instance constant buffers.
    // Allocate the buffer storing the raytracing output
    CreateRaytracingOutputBuffer();
    // #DXR Extra - Refitting
    CreateInstancePropertiesBuffer();
    // #DXR Extra: Perspective Camera
    // Create a buffer to store the modelview and perspective camera matrices
    CreateCameraBuffer();
    //Create materials buffer
    CreateMaterialsBuffer();
    // Create the buffer containing the raytracing result (always output in a
    // UAV), and create the heap referencing the resources used by the raytracing,
    // such as the acceleration structure
    CreateShaderResourceHeap();
    // Create the shader binding table and indicating which shaders
    // are invoked for each instance in the  AS
    CreateShaderBindingTable();
    //Initialize ImGui.
    InitializeImGuiContext();
    // Command lists are created in the recording state, but there is nothing
    // to record yet. The main loop expects it to be closed, so close it now.
    ThrowIfFailed(m_commandList->Close());
}

// Load the rendering pipeline dependencies.
void D3D12HelloTriangle::LoadPipeline()
{
    UINT dxgiFactoryFlags = 0;

#if defined(_DEBUG)
    // Enable the debug layer (requires the Graphics Tools "optional feature").
    // NOTE: Enabling the debug layer after device creation will invalidate the active device.
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            ComPtr<ID3D12Debug1> debugController1;
            if (SUCCEEDED(debugController.As(&debugController1)))
            {
                debugController1->SetEnableGPUBasedValidation(TRUE);
            }
            // Enable additional debug layers.
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

    if (m_useWarpDevice)
    {
        ComPtr<IDXGIAdapter> warpAdapter;
        ThrowIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));

        ThrowIfFailed(D3D12CreateDevice(
            warpAdapter.Get(),
            D3D_FEATURE_LEVEL_12_1,
            IID_PPV_ARGS(&m_device)
        ));
    }
    else
    {
        ComPtr<IDXGIAdapter1> hardwareAdapter;
        GetHardwareAdapter(factory.Get(), &hardwareAdapter);

        ThrowIfFailed(D3D12CreateDevice(
            hardwareAdapter.Get(),
            D3D_FEATURE_LEVEL_12_1,
            IID_PPV_ARGS(&m_device)
        ));
    }
    
    // Describe and create the command queue.
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));

    // Describe and create the swap chain.
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.Width = m_width;
    swapChainDesc.Height = m_height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;
    
    ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(
        m_commandQueue.Get(),		// Swap chain needs the queue so that it can force a flush on it.
        Win32Application::GetHwnd(),
        &swapChainDesc,
        nullptr,
        nullptr,
        &swapChain
    ));

    // This sample does not support fullscreen transitions.
    ThrowIfFailed(factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));

    ThrowIfFailed(swapChain.As(&m_swapChain));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // Create descriptor heaps.
    {
        // Describe and create a render target view (RTV) descriptor heap.
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = FrameCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));

        m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    // Create frame resources.
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

        // Create a RTV for each frame.
        for (UINT n = 0; n < FrameCount; n++)
        {
            ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n])));
            m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, rtvHandle);
            rtvHandle.Offset(1, m_rtvDescriptorSize);
        }
    }

    ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator)));
    
    // #DXR Extra: Depth Buffering
    // The original sample does not support depth buffering, so we need to allocate a depth buffer,
    // and later bind it before rasterization
    CreateDepthBuffer();
}

// Load the sample assets.
void D3D12HelloTriangle::LoadAssets()
{
    // #DXR Extra: Perspective Camera
    // The root signature describes which data is accessed by the shader. The camera matrices are held
    // in a constant buffer, itself referenced the heap. To do this we reference a range in the heap,
    // and use that range as the sole parameter of the shader. The camera buffer is associated in the
    // index 0, making it accessible in the shader in the b0 register.
    {
        CD3DX12_ROOT_PARAMETER constantParameter;
        CD3DX12_DESCRIPTOR_RANGE range;
        range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
        constantParameter.InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_ALL);

        // #DXR Extra - Refitting
        // Per-instance properties buffer
        CD3DX12_ROOT_PARAMETER matricesParameter;
        CD3DX12_DESCRIPTOR_RANGE matricesRange;
        matricesRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1/*descriptor count*/, 0/*register*/, 0/*space (space0)*/, 1/*heap slot*/);
        matricesParameter.InitAsDescriptorTable(1, &matricesRange, D3D12_SHADER_VISIBILITY_ALL);

        // #DXR Extra - Refitting
        // Per-instance properties index for the current geometry
        CD3DX12_ROOT_PARAMETER indexParameter;
        indexParameter.InitAsConstants(1 /*value count*/, 1/*register*/);

        std::vector<CD3DX12_ROOT_PARAMETER> params = { constantParameter, matricesParameter, indexParameter };

        CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init((UINT)params.size(), params.data(), 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> error;
        ThrowIfFailed(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
        ThrowIfFailed(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));
    }

    // Create the pipeline state, which includes compiling and loading shaders.
    {
        ComPtr<ID3DBlob> vertexShader;
        ComPtr<ID3DBlob> pixelShader;

#if defined(_DEBUG)
        // Enable better shader debugging with the graphics debugging tools.
        UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        UINT compileFlags = 0;
#endif

        ThrowIfFailed(D3DCompileFromFile(L"shaders\\shaders.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, nullptr));
        ThrowIfFailed(D3DCompileFromFile(L"shaders\\shaders.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, nullptr));

        // Define the vertex input layout.
        D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =        
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };

        // Describe and create the graphics pipeline state object (PSO).
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoDesc.pRootSignature = m_rootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc.Count = 1;
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState)));
    }

    // Create the command list.
    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator.Get(), m_pipelineState.Get(), IID_PPV_ARGS(&m_commandList)));

    // Create the vertex buffer.
    {
        std::vector<Vertex> vertices;
        std::vector<UINT> indices;

        {
            bool createCube = false; //Set this to true to make a cube for debugging purposes.
            if (createCube)
            {
                //Box vertices for debugging.
                indices = {
                    // Top face (+Y)
                    2, 7, 6,
                    2, 7, 3,

                    // Bottom face (-Y)
                    0, 4, 5,
                    0, 5, 1,

                    // Left face (-X)
                    0, 2, 6,
                    0, 6, 4,

                    // Right face (+X)
                    1, 5, 7,
                    1, 7, 3,

                    // Front face (+Z)
                    0, 1, 3,
                    0, 3, 2,

                    // Back face (-Z)
                    4, 6, 7,
                    4, 7, 5
                };

                vertices =
                {
                    Vertex({-1, -1,  1}), //0
                    Vertex({ 1, -1,  1}), //1
                    Vertex({-1,  1,  1}), //2
                    Vertex({ 1,  1,  1}), //3
                    Vertex({-1, -1, -1}), //4
                    Vertex({ 1, -1, -1}), //5
                    Vertex({-1,  1, -1}), //6
                    Vertex({ 1,  1, -1}), //7
                };
            }
            else
            {
                OBJFileManager ofm = OBJFileManager();
                std::vector<objl::Vertex> modelFileVertices;

                std::string path = "models\\teapot.obj";
                bool modelFileLoaded = ofm.LoadObjFile(path, modelFileVertices, indices);
                assert(modelFileLoaded == true);
                //Convert from objl::Vertex to Vertex struct to complete the load.
                for (auto& vertex : modelFileVertices)
                {
                    Vertex v;
                    v.position = XMFLOAT3(vertex.Position.X, vertex.Position.Y, vertex.Position.Z);
                    vertices.push_back(v);
                }

                ComputeVertexNormals(vertices, indices);
            }
        }

        m_modelVertexCount = vertices.size();
        m_modelIndexCount = indices.size();

        const UINT vertexBufferSize = vertices.size() * sizeof(Vertex);

        // Note: using upload heaps to transfer static data like vert buffers is not 
        // recommended. Every time the GPU needs it, the upload heap will be marshalled 
        // over. Please read up on Default Heap usage. An upload heap is used here for 
        // code simplicity and because there are very few verts to actually transfer.
        ThrowIfFailed(m_device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_modelVertexBuffer)));

        // Copy the triangle data to the vertex buffer.
        UINT8* pVertexDataBegin;
        CD3DX12_RANGE readRange(0, 0); // We do not intend to read from this resource on the CPU.
        ThrowIfFailed(m_modelVertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin)));
        memcpy(pVertexDataBegin, vertices.data(), vertexBufferSize);
        m_modelVertexBuffer->Unmap(0, nullptr);

        // Initialize the vertex buffer view.
        m_modelVertexBufferView.BufferLocation = m_modelVertexBuffer->GetGPUVirtualAddress();
        m_modelVertexBufferView.StrideInBytes = sizeof(Vertex);
        m_modelVertexBufferView.SizeInBytes = vertexBufferSize;

        const UINT indexBufferSizeInBytes = (UINT)indices.size() * sizeof(UINT);

        CD3DX12_HEAP_PROPERTIES heapProperty = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC bufferResource = CD3DX12_RESOURCE_DESC::Buffer(indexBufferSizeInBytes);
        ThrowIfFailed(m_device->CreateCommittedResource(&heapProperty, D3D12_HEAP_FLAG_NONE, &bufferResource, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_modelIndexBuffer)));

        // Copy the triangle data to the index buffer.
        UINT8* pIndexDataBegin;
        ThrowIfFailed(m_modelIndexBuffer->Map(0, &readRange, (void**)&pIndexDataBegin));
        memcpy(pIndexDataBegin, indices.data(), indexBufferSizeInBytes);
        m_modelIndexBuffer->Unmap(0, nullptr);

        // Initialize the index buffer view.
        m_modelIndexBufferView.BufferLocation = m_modelIndexBuffer->GetGPUVirtualAddress();
        m_modelIndexBufferView.Format = DXGI_FORMAT_R32_UINT;
        m_modelIndexBufferView.SizeInBytes = indexBufferSizeInBytes;

        // #DXR - Per Instance
        // Create a vertex buffer for a ground plane, similarly to the triangle definition above
        CreatePlaneVB();
    }

    // Create synchronization objects and wait until assets have been uploaded to the GPU.
    {
        ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
        m_fenceValue = 1;

        // Create an event handle to use for frame synchronization.
        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (m_fenceEvent == nullptr)
        {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        }

        // Wait for the command list to execute; we are reusing the same command 
        // list in our main loop but for now, we just want to wait for setup to 
        // complete before continuing.
        WaitForPreviousFrame();
    }
}

// Update frame-based values. This method is called before each render.
void D3D12HelloTriangle::OnUpdate()
{
    frameStart = high_resolution_clock::now();
    materials[0].albedo = uiConstructor.GetAlbedo();
    materials[0].roughness = uiConstructor.GetRoughness();
    materials[0].metallic = uiConstructor.GetMetallic();
    materials[0].reflectivity = uiConstructor.GetReflectivity();
    UpdateMaterialsBuffer();
    // #DXR Extra: Perspective Camera
    UpdateCameraBuffer();
    // #DXR Extra - Refitting
    UpdateInstancePropertiesBuffer();
}

// Render the scene.
void D3D12HelloTriangle::OnRender()
{
    // Record all the commands we need to render the scene into the command list.
    PopulateCommandList();

    // Execute the command list.
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    // Present the frame (first argument 1 for vsync enabled, 0 for vsync disabled).
    HRESULT result = m_swapChain->Present(1, 0);

    //Whenever there is a bug in the code, the Present() function crashes.
    //The code below helps with debugging, which is why ThrowIfFailed() is separated here.
    if (result != S_OK)
    {
        HRESULT reason = m_device->GetDeviceRemovedReason();
        ThrowIfFailed(result);
    }

    WaitForPreviousFrame();

    //Update the model if there is a pending update
    if (pendingModelUpdate)
    {
        WaitForPreviousFrame();
        UpdateModelWithPendings();
        pendingModelUpdate = false;
    }

    //Calculate how long the frame took
    frameEnd = high_resolution_clock::now();
    milliseconds duration = duration_cast<milliseconds>(frameEnd - frameStart);
    frameTime = (float)duration.count();
    uiConstructor.SetFrameTime(frameTime);
}

void D3D12HelloTriangle::OnDestroy()
{
    // Ensure that the GPU is no longer referencing resources that are about to be
    // cleaned up by the destructor.
    WaitForPreviousFrame();
    //Cleanup ImGui
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CloseHandle(m_fenceEvent);
}

void D3D12HelloTriangle::PopulateCommandList()
{
    // Command list allocators can only be reset when the associated 
    // command lists have finished execution on the GPU; apps should use 
    // fences to determine GPU execution progress.
    ThrowIfFailed(m_commandAllocator->Reset());

    // However, when ExecuteCommandList() is called on a particular command 
    // list, that command list can then be reset at any time and must be before 
    // re-recording.
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get()));

    // Set necessary state.
    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_commandList->RSSetViewports(1, &m_viewport);
    m_commandList->RSSetScissorRects(1, &m_scissorRect);

    // Indicate that the back buffer will be used as a render target.
    m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_frameIndex, m_rtvDescriptorSize);
    // #DXR Extra: Depth Buffering
    // Bind the depth buffer as a render target
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    // Record commands.
    // #DXR
    if (m_raster)
    {
        // #DXR Extra: Depth Buffering
        //Clear the depth buffer before rendering.
        m_commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        // #DXR Extra: Perspective Camera
        std::vector<ID3D12DescriptorHeap*> heaps = { m_constHeap.Get() };
        m_commandList->SetDescriptorHeaps((UINT)heaps.size(), heaps.data());
        // #DXR Extra - Refitting
        D3D12_GPU_DESCRIPTOR_HANDLE handle = m_constHeap->GetGPUDescriptorHandleForHeapStart();
        // Access to the camera buffer, 1st parameter of the root signature
        m_commandList->SetGraphicsRootDescriptorTable(0, handle);
        // Access to the per-instance properties buffer, 2nd parameter of the root signature
        m_commandList->SetGraphicsRootDescriptorTable(1, handle);
        // Instance index in the per-instance properties buffer, 3rd parameter of the root signature
        // Here we set the value to 0, and since we have only 1 constant, the offset is 0 as well
        m_commandList->SetGraphicsRoot32BitConstant(2, 0, 0);
        const float clearColor[] = { 0.03f, 0.35f, 0.43f, 1.0f };
        m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        //Render tetrahedron
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_commandList->IASetVertexBuffers(0, 1, &m_modelVertexBufferView);
        m_commandList->IASetIndexBuffer(&m_modelIndexBufferView);
        m_commandList->DrawIndexedInstanced(m_modelIndexCount, 1, 0, 0, 0);
        //Render plane
        m_commandList->IASetVertexBuffers(0, 1, &m_planeBufferView);
        m_commandList->DrawInstanced(6, 1, 0, 0);
    }
    else
    {
        const float clearColor[] = { 0.6f, 0.8f, 0.4f, 1.0f };
        m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

        // #DXR
        // Bind the descriptor heap giving access to the top-level acceleration
        // structure, as well as the raytracing output
        std::vector<ID3D12DescriptorHeap*> heaps = { m_srvUavHeap.Get() };
        m_commandList->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());

        // On the last frame, the raytracing output was used as a copy source, to
        // copy its contents into the render target. Now we need to transition it to
        // a UAV so that the shaders can write in it.
        CD3DX12_RESOURCE_BARRIER transition = CD3DX12_RESOURCE_BARRIER::Transition(m_outputResource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_commandList->ResourceBarrier(1, &transition);
        // Setup the raytracing task
        D3D12_DISPATCH_RAYS_DESC desc = {};
        // The layout of the SBT is as follows: ray generation shader, miss
        // shaders, hit groups. As described in the CreateShaderBindingTable method,
        // all SBT entries of a given type have the same size to allow a fixed stride.

        // The ray generation shaders are always at the beginning of the SBT.
        uint32_t rayGenSectionSizeInBytes = m_sbtHelper.GetRayGenSectionSize();
        desc.RayGenerationShaderRecord.StartAddress = m_sbtStorage->GetGPUVirtualAddress();
        desc.RayGenerationShaderRecord.SizeInBytes = rayGenSectionSizeInBytes;

        // The miss shaders are in the second SBT section, right after the ray
        // generation shader. We have one miss shader for the camera rays and one
        // for the shadow rays, so this section has a size of 2*m_sbtEntrySize. We
        // also indicate the stride between the two miss shaders, which is the size
        // of a SBT entry
        uint32_t missSectionSizeInBytes = m_sbtHelper.GetMissSectionSize();
        desc.MissShaderTable.StartAddress = m_sbtStorage->GetGPUVirtualAddress() + rayGenSectionSizeInBytes;
        desc.MissShaderTable.SizeInBytes = missSectionSizeInBytes;
        desc.MissShaderTable.StrideInBytes = m_sbtHelper.GetMissEntrySize();

        // The hit groups section start after the miss shaders. In this sample we
        // have one 1 hit group for the triangle
        uint32_t hitGroupSectionSize = m_sbtHelper.GetHitGroupSectionSize();
        desc.HitGroupTable.StartAddress = m_sbtStorage->GetGPUVirtualAddress() + rayGenSectionSizeInBytes + missSectionSizeInBytes;
        desc.HitGroupTable.SizeInBytes = hitGroupSectionSize;
        desc.HitGroupTable.StrideInBytes = m_sbtHelper.GetHitGroupEntrySize();

        // Dimensions of the image to render, identical to a kernel launch dimension
        desc.Width = GetWidth();
        desc.Height = GetHeight();
        desc.Depth = 1;

        // Bind the raytracing pipeline
        m_commandList->SetPipelineState1(m_rtStateObject.Get());
        m_commandList->DispatchRays(&desc);

        // The raytracing output needs to be copied to the actual render target used
        // for display. For this, we need to transition the raytracing output from a
        // UAV to a copy source, and the render target buffer to a copy destination.
        // We can then do the actual copy, before transitioning the render target
        // buffer into a render target, that will be then used to display the image
        transition = CD3DX12_RESOURCE_BARRIER::Transition(m_outputResource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_commandList->ResourceBarrier(1, &transition);
        transition = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        m_commandList->ResourceBarrier(1, &transition);

        m_commandList->CopyResource(m_renderTargets[m_frameIndex].Get(), m_outputResource.Get());

        transition = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_commandList->ResourceBarrier(1, &transition);
    }

    //ImGui rendering code
    if (renderUI)
    {
        ImGui_ImplWin32_NewFrame();
        ImGui_ImplDX12_NewFrame();
        ImGui::NewFrame();
        uiConstructor.Construct();
        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_commandList.Get());
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault(nullptr, (void*)m_commandList.Get());
    }
    // Indicate that the back buffer will now be used to present.
    m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
    ThrowIfFailed(m_commandList->Close());
}

void D3D12HelloTriangle::WaitForPreviousFrame()
{
    // WAITING FOR THE FRAME TO COMPLETE BEFORE CONTINUING IS NOT BEST PRACTICE.
    // This is code implemented as such for simplicity. The D3D12HelloFrameBuffering
    // sample illustrates how to use fences for efficient resource usage and to
    // maximize GPU utilization.

    // Signal and increment the fence value.
    const UINT64 fence = m_fenceValue;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fence));
    m_fenceValue++;

    // Wait until the previous frame is finished.
    if (m_fence->GetCompletedValue() < fence)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(fence, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void D3D12HelloTriangle::CheckRaytracingSupport()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    ThrowIfFailed(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)));
    if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
    {
        throw std::runtime_error("Raytracing is not supported on device.");
    }
}

void D3D12HelloTriangle::OnKeyUp(UINT8 key)
{
    // Alternate between rasterization and raytracing using the spacebar
    if (key == VK_SPACE && !ImGui::GetIO().WantCaptureKeyboard)
    {
        m_raster = !m_raster;
        uiConstructor.SetRenderingMode(!m_raster);
    }
    if (key == VK_ADD)
    {
        uiConstructor.SetDemoUIEnable(!uiConstructor.IsDemoUIShown());
    }
    if (key == VK_SUBTRACT)
    {
        renderUI = !renderUI;
    }
}

void D3D12HelloTriangle::OnKeyDown(UINT8 key)
{
    //Important note: This function is called multiple times if the key is held.
}

D3D12HelloTriangle::AccelerationStructureBuffers D3D12HelloTriangle::CreateBottomLevelAS(std::vector<std::pair<ComPtr<ID3D12Resource>, uint32_t>> vVertexBuffers, std::vector<std::pair<ComPtr<ID3D12Resource>, uint32_t>> vIndexBuffers)
{
    // Create a bottom-level acceleration structure based on a list of vertex
    // buffers in GPU memory along with their vertex count. The build is done
    // in 3 steps: gathering the geometry, computing the sizes of the required
    // buffers, and building the actual AS

    //Step one: Gathering the geometry
    nv_helpers_dx12::BottomLevelASGenerator bottomLevelAS;

    // #DXR Extra: Indexed Geometry
    for (size_t i = 0; i < vVertexBuffers.size(); i++)
    {
        if (i < vIndexBuffers.size() && vIndexBuffers[i].second > 0)
        {
            bottomLevelAS.AddVertexBuffer(vVertexBuffers[i].first.Get(), 0,
                vVertexBuffers[i].second, sizeof(Vertex),
                vIndexBuffers[i].first.Get(), 0,
                vIndexBuffers[i].second, nullptr, 0, true);
        }
        else
        {
            bottomLevelAS.AddVertexBuffer(vVertexBuffers[i].first.Get(), 0,
                vVertexBuffers[i].second, sizeof(Vertex), 0,
                0);
        }
    }

    //Step two: Computing the sizes for the buffers
    // The AS build requires some scratch space to store temporary information.
    // The amount of scratch memory is dependent on the scene complexity.
    UINT64 scratchSizeInBytes = 0;
    // The final AS also needs to be stored in addition to the existing vertex
    // buffers. It size is also dependent on the scene complexity.
    UINT64 resultSizeInBytes = 0;
    bottomLevelAS.ComputeASBufferSizes(m_device.Get(), false, &scratchSizeInBytes, &resultSizeInBytes);

    //Step three: building the actual AS
    // Once the sizes are obtained, the application is responsible for allocating
    // the necessary buffers. Since the entire generation will be done on the GPU,
    // we can directly allocate those on the default heap
    AccelerationStructureBuffers buffers;
    buffers.pScratch = nv_helpers_dx12::CreateBuffer(m_device.Get(), scratchSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, nv_helpers_dx12::kDefaultHeapProps);
    buffers.pResult = nv_helpers_dx12::CreateBuffer(m_device.Get(), resultSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nv_helpers_dx12::kDefaultHeapProps);

    // Build the acceleration structure. Note that this call integrates a barrier
    // on the generated AS, so that it can be used to compute a top-level AS right
    // after this method.
    bottomLevelAS.Generate(m_commandList.Get(), buffers.pScratch.Get(), buffers.pResult.Get(), false, nullptr);
    return buffers;
}

void D3D12HelloTriangle::CreateTopLevelAS(const std::vector<TLASParams>& instances, bool updateOnly)
{
    nv_helpers_dx12::TopLevelASGenerator m_topLevelASGenerator;

    if (!updateOnly)
    {
        // Create the main acceleration structure that holds all instances of the scene.
        // Similarly to the bottom-level AS generation, it is done in 3 steps: gathering
        // the instances, computing the memory requirements for the AS, and building the
        // AS itself

        //Step one: Gather the instances
        for (size_t i = 0; i < instances.size(); i++)
        {
            const TLASParams& instance = instances[i];
            m_topLevelASGenerator.AddInstance(instance.blas.Get(), instance.transformMatrix, (UINT)i, instance.hitGroupIndex);
        }

        //Step two: Compute the memory requirements

        // As for the bottom-level AS, the building the AS requires some scratch space
        // to store temporary data in addition to the actual AS. In the case of the
        // top-level AS, the instance descriptors also need to be stored in GPU
        // memory. This call outputs the memory requirements for each (scratch,
        // results, instance descriptors) so that the application can allocate the
        // corresponding memory
        UINT64 scratchSize, resultSize, instanceDescSize;
        m_topLevelASGenerator.ComputeASBufferSizes(m_device.Get(), true, &scratchSize, &resultSize, &instanceDescSize);

        //Step three: Create the buffers and build the TLAS

        // Create the scratch and result buffers. Since the build is all done on GPU,
        // those can be allocated on the default heap
        m_topLevelASBuffers.pScratch = nv_helpers_dx12::CreateBuffer(m_device.Get(), scratchSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nv_helpers_dx12::kDefaultHeapProps);
        m_topLevelASBuffers.pResult = nv_helpers_dx12::CreateBuffer(m_device.Get(), resultSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nv_helpers_dx12::kDefaultHeapProps);
        // The buffer describing the instances: ID, shader binding information,
        // matrices ... Those will be copied into the buffer by the helper through
        // mapping, so the buffer has to be allocated on the upload heap.
        m_topLevelASBuffers.pInstanceDesc = nv_helpers_dx12::CreateBuffer(m_device.Get(), instanceDescSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);
    }

    m_topLevelASGenerator.Generate(m_commandList.Get(), m_topLevelASBuffers.pScratch.Get(), m_topLevelASBuffers.pResult.Get(), m_topLevelASBuffers.pInstanceDesc.Get(), updateOnly, m_topLevelASBuffers.pResult.Get());
}

void D3D12HelloTriangle::CreateAccelerationStructures()
{
    // Build the BLAS from triangle vertex buffer
    AccelerationStructureBuffers modelBottomLevelBuffers = CreateBottomLevelAS({ { m_modelVertexBuffer.Get(), m_modelVertexCount } }, { { m_modelIndexBuffer.Get(), m_modelIndexCount } });
    AccelerationStructureBuffers planeBottomLevelBuffers = CreateBottomLevelAS({ { m_planeBuffer.Get(), 6 } });

    m_instances = { TLASParams(modelBottomLevelBuffers.pResult, XMMatrixIdentity(), 0, 0),
                    TLASParams(modelBottomLevelBuffers.pResult, XMMatrixTranslation(-5.0f, 0.0f, 5.0f), 0, 0),
                    TLASParams(modelBottomLevelBuffers.pResult, XMMatrixTranslation(-5.0f, 0.0f, 5.0f), 0, 0),
                    TLASParams(modelBottomLevelBuffers.pResult, XMMatrixTranslation(-5.0f, 0.0f, -5.0f), 0, 0),
                    TLASParams(modelBottomLevelBuffers.pResult, XMMatrixTranslation(5.0f, 0.0f, -5.0f), 0, 0),
                    TLASParams(modelBottomLevelBuffers.pResult, XMMatrixTranslation(5.0f, 0.0f, 5.0f), 0, 0),
                    TLASParams(planeBottomLevelBuffers.pResult, XMMatrixIdentity(), 2, 0),
    };

    CreateTopLevelAS(m_instances);

    //Flush the command list and wait for it to finish
    m_commandList->Close();
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, ppCommandLists);
    m_fenceValue++;
    m_commandQueue->Signal(m_fence.Get(), m_fenceValue);

    m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
    WaitForSingleObject(m_fenceEvent, INFINITE);

    // Once the command list is finished executing, reset it to be reused for rendering
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get()));

    // Store the AS buffers. The rest of the buffers will be released once we exit the function
    m_bottomLevelAS = modelBottomLevelBuffers.pResult;
}

ComPtr<ID3D12RootSignature> D3D12HelloTriangle::CreateRayGenSignature()
{
    //RayGen shader needs to access 3 resources: the raytracing output, the TLAS and the camera matrices (view, proj and their inverses are accessed here)
    nv_helpers_dx12::RootSignatureGenerator rsg;
    //Add the external data needed for the shader program
    rsg.AddHeapRangesParameter({ {0 /*u0*/, 1 /*1 descriptor*/, 0 /*use the implicit register space 0*/, D3D12_DESCRIPTOR_RANGE_TYPE_UAV /*UAV representing the output buffer*/, 0 /*heap slot where the UAV is defined*/},
                                 {0 /*t0*/, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV /*TLAS*/, 1},
                                 {0 /*b0*/, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_CBV /*Camera parameters*/, 2} });

    return rsg.Generate(m_device.Get(), true);
}

ComPtr<ID3D12RootSignature> D3D12HelloTriangle::CreateHitSignature()
{
    nv_helpers_dx12::RootSignatureGenerator rsg;
    rsg.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 0 /*t0*/); // vertices and colors
    rsg.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 1 /*t1*/); // indices
    /*
    * AddHeapRangeParameter() function parameters are defined as follows:
        UINT,                        //BaseShaderRegister,
        UINT,                        //NumDescriptors
        UINT,                        //RegisterSpace
        D3D12_DESCRIPTOR_RANGE_TYPE, //RangeType
        UINT                         //OffsetInDescriptorsFromTableStart
    */
    rsg.AddHeapRangesParameter(
        {
            // #DXR Extra - Another ray type
            // Add a single range pointing to the TLAS in the heap
            { 2 /*t2*/, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1 /*2nd slot of the heap*/ },
            // #DXR Extra: Per-Instance Data
            // The vertex colors may differ for each instance, so it is not possible to
            // point to a single buffer in the heap. Instead we use the concept of root
            // parameters, which are defined directly by a pointer in memory. In the
            // shader binding table we will associate each hit shader instance with its
            // constant buffer. Here we bind the buffer to the first slot, accessible in
            // HLSL as register(b0)
            { 0 /*b0*/, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_CBV /*Scene data*/, 2 },
            // # DXR Extra - Simple Lighting
            { 3 /*t3*/, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV /*Per-instance data*/, 3 },
            { 4 /*t4*/, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV /*Material array*/, 4 }
        });
    return rsg.Generate(m_device.Get(), true);
}

ComPtr<ID3D12RootSignature> D3D12HelloTriangle::CreateMissSignature()
{
    //Miss shader only needs the hit info, which comes from the shaders themselves
    //therefore it doesn't need any external data.
    nv_helpers_dx12::RootSignatureGenerator rsg;
    return rsg.Generate(m_device.Get(), true);
}

void D3D12HelloTriangle::CreateRaytracingPipeline()
{
    // The raytracing pipeline binds the shader code, root signatures and pipeline
    // characteristics in a single structure used by DXR to invoke the shaders and
    // manage temporary memory during raytracing
    nv_helpers_dx12::RayTracingPipelineGenerator pipeline(m_device.Get());

    //First we compile the HLSL shaders to DXIL so that they can be used in GPUs.
    //The raytracing pipeline contains all the shaders that may be executed during the raytracing process.
    //The codes are separated semantically to raygen, miss and hit for clarity. Any code layout can be used.
    m_rayGenLibrary = nv_helpers_dx12::CompileShaderLibrary(L"shaders\\RayGen.hlsl");
    m_missLibrary = nv_helpers_dx12::CompileShaderLibrary(L"shaders\\Miss.hlsl");
    m_hitLibrary = nv_helpers_dx12::CompileShaderLibrary(L"shaders\\Hit.hlsl");
    // #DXR Extra - Another ray type
    m_shadowLibrary = nv_helpers_dx12::CompileShaderLibrary(L"shaders\\ShadowRay.hlsl");

    //Secondly, we add the libraries to the pipeline
    // In a way similar to DLLs, each library is associated with a number of
    // exported symbols. This has to be done explicitly in the lines below. Note that a single library
    // can contain an arbitrary number of symbols, whose semantic is given in HLSL using the [shader("xxx")] syntax
    //It is important to note that the symbol names MUST be unique!
    pipeline.AddLibrary(m_rayGenLibrary.Get(), { L"RayGen" });
    pipeline.AddLibrary(m_missLibrary.Get(), { L"Miss" });
    //L"PlaneClosestHit" is from #DXR Extra: Per-Instance Data
    pipeline.AddLibrary(m_hitLibrary.Get(), { L"ClosestHit", L"PlaneClosestHit" });
    // #DXR Extra - Another ray type
    pipeline.AddLibrary(m_shadowLibrary.Get(), { L"ShadowClosestHit", L"ShadowMiss" });

    //Third, we generate the root signatures of the shaders so that we can define which parameters and buffers will be accessed.
    m_rayGenSignature = CreateRayGenSignature();
    m_hitSignature = CreateHitSignature();
    m_missSignature = CreateMissSignature();
    // #DXR Extra - Another ray type
    m_shadowSignature = CreateHitSignature();

    //Fourth, we need to define what happens when a ray hits our geometry. There are three types of hits: Intersection shader, any-hit shader and a closest-hit shader.
    //All these shaders are stored in what's called a HitGroup. An intersection shader is used when a ray hits a non-triangular geometry. Non-triangular geometries aren't used
    //so this doesn't apply to this project. We can simply use the default intersection shader which is defined in DX12. Any-hit shader is used for any geometry that the ray hits.
    //An empty any-hit shader is also defined in DX12 and that will be used for now as well. The closest-hit shader is the one that's called on the geometry that the ray first hits,
    //therefore the one that is going to be actually visible to the camera. This isn't defined in DX12 and is defined by the developer. Right now, the Hit.hlsl shader defines the ClosestHit
    //shader and that will be fed into the pipeline as the closest-hit shader.
    pipeline.AddHitGroup(L"HitGroup", L"ClosestHit");
    pipeline.AddHitGroup(L"PlaneHitGroup", L"PlaneClosestHit");
    // #DXR Extra - Another ray type
    // Hit group for all geometry when hit by a shadow ray
    pipeline.AddHitGroup(L"ShadowHitGroup", L"ShadowClosestHit");

    //Fifth, we need to associate the shaders imported from DXIL libraries with exactly one root signature.
    // The following section associates the root signature to each shader. Note
    // that we can explicitly show that some shaders share the same root signature
    // (eg. Miss and ShadowMiss). Note that the hit shaders are now only referred
    // to as hit groups, meaning that the underlying intersection, any-hit and
    // closest-hit shaders share the same root signature.
    pipeline.AddRootSignatureAssociation(m_rayGenSignature.Get(), { L"RayGen" });
    pipeline.AddRootSignatureAssociation(m_missSignature.Get(), { L"Miss", L"ShadowMiss" }); //ShadowMiss comes from #DXR Extra - Another ray type
    pipeline.AddRootSignatureAssociation(m_hitSignature.Get(), { L"HitGroup", L"PlaneHitGroup" });
    // #DXR Extra - Another ray type
    pipeline.AddRootSignatureAssociation(m_shadowSignature.Get(), { L"ShadowHitGroup" });

    //Sixth, we need to define the memory sizes and recursions allowed to the shaders.
    // The payload size defines the maximum size of the data carried by the rays,
    // ie. the data exchanged between shaders, such as the HitInfo structure in the HLSL code.
    // It is important to keep this value as low as possible as a too high value
    // would result in unnecessary memory consumption and cache trashing.
    //Sizes of some primitive types in HLSL are defined for easier modification of max payload and attribute sizes if the payload is changed on GPU side.
    const UINT HLSL_UINT_SIZE_IN_BYTES = 4;
    const UINT HLSL_BOOL_SIZE_IN_BYTES = 4;
    const UINT HLSL_FLOAT_SIZE_IN_BYTES = 4;
    const UINT HLSL_FLOAT2_SIZE_IN_BYTES = 2 * HLSL_FLOAT_SIZE_IN_BYTES;
    const UINT HLSL_FLOAT3_SIZE_IN_BYTES = 3 * HLSL_FLOAT_SIZE_IN_BYTES;
    const UINT HLSL_FLOAT4_SIZE_IN_BYTES = 4 * HLSL_FLOAT_SIZE_IN_BYTES;
    pipeline.SetMaxPayloadSize(HLSL_FLOAT3_SIZE_IN_BYTES);

    // Upon hitting a surface, DXR can provide several attributes to the hit.
    // We just use the barycentric coordinates defined by the weights u,v
    // of the last two vertices of the triangle. The actual barycentrics can
    // be obtained using float3 barycentrics = float3(1.f-u-v, u, v);
    //Note: float3(1.f-u-v, u, v) is what it says in the NVIDIA tutorial. In the hit shader, it is used as float3(u, v, 1.f-u-v). idky but it works
    pipeline.SetMaxAttributeSize(HLSL_FLOAT2_SIZE_IN_BYTES); // barycentric coordinates

    // The raytracing process can shoot rays from existing hit points, resulting
    // in nested TraceRay calls. Our code includes shadow rays, which means
    // we need a depth of at least 2 (shadows make it possible to shoot rays from a hit point).
    // Note that this recursion depth should be kept to a minimum for best performance.
    // Path tracing algorithms can be easily flattened into a simple loop in the ray generation.
    //The recursion depth here is 20 although it might look like 3 should suffice since there are up to 3 reflections.
    //The reason is that depending on the angle of the ray, it might bounce multiple times between the models, instead of just once for the reflection.
    //That increases the amount of recursions, which causes 3 to be not enough. I couldn't find a fix number that can handle all cases in my testings,
    //so I just chose a number that seems to be big enough. A value of 10 sometimes works but sometimes crashes and I am not sure why.
    pipeline.SetMaxRecursionDepth(20);

    //Seventh, finally we generate the pipeline to be executed on the GPU and then cast the state object to a properties object
    //so that later we can access the shader pointers by name.
    m_rtStateObject = pipeline.Generate();
    ThrowIfFailed(m_rtStateObject->QueryInterface(IID_PPV_ARGS(&m_rtStateObjectProperties)));
}

void D3D12HelloTriangle::CreateRaytracingOutputBuffer()
{
    //Allocate the buffer for the raytracing output, which is the same size as the output image
    D3D12_RESOURCE_DESC resDesc = {};
    resDesc.DepthOrArraySize = 1;
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    // The backbuffer is actually DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, but sRGB
    // formats cannot be used with UAVs. For accuracy we should convert to sRGB
    // ourselves in the shader
    resDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    resDesc.Width = GetWidth();
    resDesc.Height = GetHeight();
    resDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resDesc.MipLevels = 1;
    resDesc.SampleDesc.Count = 1;
    ThrowIfFailed(m_device->CreateCommittedResource(&nv_helpers_dx12::kDefaultHeapProps, D3D12_HEAP_FLAG_NONE, &resDesc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&m_outputResource)));
}

//Create the main heap used by shaders, which allows access to the raytracing output and the TLAS
void D3D12HelloTriangle::CreateShaderResourceHeap()
{
    if (m_srvUavHeap == nullptr)
    {
        //5 entries needed: 1 UAV for the raytracing output, 1 SRV for TLAS, 1 CBV for camera matrices and 1 for the per-instance data for the lighting, 1 for the materials
        m_srvUavHeap = nv_helpers_dx12::CreateDescriptorHeap(m_device.Get(), 5, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true);
    }

    //Get a handle to te heap memory on the CPU side so that descriptors can be directly written to
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle_cpu = m_srvUavHeap->GetCPUDescriptorHandleForHeapStart();

    //UAV is the first entry based on what we defined in the root signature.
    //Create*View() methods write the view information directly into srvHandle.
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(m_outputResource.Get(), nullptr, &uavDesc, srvHandle_cpu);

    //Add the TLAS SRV right after the raytracing output buffer
    srvHandle_cpu.ptr += m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.RaytracingAccelerationStructure.Location = m_topLevelASBuffers.pResult->GetGPUVirtualAddress();
    //Write the AS view in heap
    m_device->CreateShaderResourceView(nullptr, &srvDesc, srvHandle_cpu);

    // #DXR Extra: Perspective Camera
    // Add the constant buffer for the camera after the TLAS
    srvHandle_cpu.ptr += m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // Describe and create a constant buffer view for the camera
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = m_cameraBuffer->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = m_cameraBufferSize;
    m_device->CreateConstantBufferView(&cbvDesc, srvHandle_cpu);

    //#DXR Extra - Simple Lighting
    srvHandle_cpu.ptr += m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = (UINT)m_instances.size();
    srvDesc.Buffer.StructureByteStride = sizeof(InstanceProperties);
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    // Write the per-instance properties buffer view in the heap
    m_device->CreateShaderResourceView(m_instancePropertiesBuffer.Get(), &srvDesc, srvHandle_cpu);

    //Materials heap slot
    srvHandle_cpu.ptr += m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = (UINT)materials.size();
    srvDesc.Buffer.StructureByteStride = sizeof(Material);
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    m_device->CreateShaderResourceView(materialsBuffer.Get(), &srvDesc, srvHandle_cpu);
}

void D3D12HelloTriangle::CreateShaderBindingTable()
{
    //The resources are bound to shaders in this function.

    // The SBT helper class collects calls to Add*Program. If called several times, the helper must be emptied before re-adding shaders.
    m_sbtHelper.Reset();
    // The pointer to the beginning of the heap is the only parameter required by shaders without root parameters
    D3D12_GPU_DESCRIPTOR_HANDLE srvUavHeapHandle = m_srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    // The helper treats both root parameter pointers and heap pointers as void*, while DX12 uses the
    // D3D12_GPU_DESCRIPTOR_HANDLE to define heap pointers. The pointer in this struct is a UINT64,
    // which then has to be reinterpreted as a pointer.
    auto heapPointer = reinterpret_cast<UINT64*>(srvUavHeapHandle.ptr);

    // The ray generation only uses heap data
    m_sbtHelper.AddRayGenerationProgram(L"RayGen", { heapPointer });
    // The miss and hit shaders do not access any external resources: instead they
    // communicate their results through the ray payload
    m_sbtHelper.AddMissProgram(L"Miss", {});
    // #DXR Extra - Another ray type
    m_sbtHelper.AddMissProgram(L"ShadowMiss", {});

    // Hit shader setup
    m_sbtHelper.AddHitGroup(L"HitGroup", { (void*)m_modelVertexBuffer->GetGPUVirtualAddress(),
                                           (void*)m_modelIndexBuffer->GetGPUVirtualAddress(),
                                           (void*)(heapPointer),
                                           (void*)m_perInstanceConstantBuffers[0]->GetGPUVirtualAddress(),
                                           (void*)m_instancePropertiesBuffer->GetGPUVirtualAddress(),
                                           (void*)materialsBuffer->GetGPUVirtualAddress(),
        });
    // #DXR Extra - Another ray type
    m_sbtHelper.AddHitGroup(L"ShadowHitGroup", {});

    // #DXR Extra: Per-Instance Data
    m_sbtHelper.AddHitGroup(L"PlaneHitGroup",
        {
            (void*)m_planeBuffer->GetGPUVirtualAddress(),
            (void*)m_globalConstantBuffer->GetGPUVirtualAddress(),
            heapPointer,
        });

    // Compute the size of the SBT given the number of shaders and their parameters
    uint32_t sbtSize = m_sbtHelper.ComputeSBTSize();

    // Create the SBT on the upload heap. This is required as the helper will use
    // mapping to write the SBT contents. After the SBT compilation it could be
    // copied to the default heap for performance.
    if (m_sbtStorage != nullptr)
    {
        m_sbtStorage.Reset();
    }
    m_sbtStorage = nv_helpers_dx12::CreateBuffer(m_device.Get(), sbtSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);

    if (!m_sbtStorage)
    {
        throw std::logic_error("Could not allocate the shader binding table.");
    }
    // Compile the SBT from the shader and parameters info
    m_sbtHelper.Generate(m_sbtStorage.Get(), m_rtStateObjectProperties.Get());
}

void D3D12HelloTriangle::CreateCameraBuffer()
{
    // The 4 matrices are the classical ones used in the rasterization process, projecting the world - space positions of the vertices into a unit cube.
    // However, to obtain a raytracing result consistent with the rasterization, we need to do the opposite:
    // the rays are initialized as if we had an orthographic camera located at the origin.
    // We then need to transform the ray origin and direction into world space, using the inverse view and projection matrices.
    // The camera buffer stores all 4 matrices, where the raster and raytracing paths will access only the ones needed.
    uint32_t nbMatrix = 4; //view, perspective, viewInv, perspectiveInv
    m_cameraBufferSize = nbMatrix * sizeof(XMMATRIX);

    // Create the constant buffer for all matrices
    m_cameraBuffer = nv_helpers_dx12::CreateBuffer(m_device.Get(), m_cameraBufferSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);
    //Descriptor heap that will be used by the rasterization shaders
    // #DXR Extra - Refitting
    // Create a descriptor heap that will be used by the rasterization shaders:
    // Camera matrices and per-instance matrices
    m_constHeap = nv_helpers_dx12::CreateDescriptorHeap(m_device.Get(), 2, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true);

    // Describe and create the constant buffer view.
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = m_cameraBuffer->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = m_cameraBufferSize;

    // Get a handle to the heap memory on the CPU side, to be able to write the descriptors directly
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_constHeap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateConstantBufferView(&cbvDesc, srvHandle);

    // #DXR Extra - Refitting
    // Add the per-instance buffer
    srvHandle.ptr += m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = (UINT)m_instances.size();
    srvDesc.Buffer.StructureByteStride = sizeof(InstanceProperties);
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    // Write the per-instance buffer view in the heap
    m_device->CreateShaderResourceView(m_instancePropertiesBuffer.Get(), &srvDesc, srvHandle);
}

void D3D12HelloTriangle::UpdateCameraBuffer()
{
    std::vector<XMMATRIX> matrices(4);
    // Initialize the view matrix, ideally this should be based on user
    // interactions The lookat and perspective matrices used for rasterization are
    // defined to transform world-space vertices into a [0,1]x[0,1]x[0,1] camera
    // space
    const glm::mat4& mat = nv_helpers_dx12::CameraManip.getMatrix();
    memcpy(&matrices[0].r->m128_f32[0], glm::value_ptr(mat), 16 * sizeof(float));

    float fovAngleY_degrees = 45.0f;
    float fovAngleY = fovAngleY_degrees * XM_PI / 180.0f; //Convert fov angle degrees to radians
    matrices[1] = XMMatrixPerspectiveFovRH(fovAngleY, m_aspectRatio, 0.1f, 1000.0f);

    // Raytracing has to do the contrary of rasterization: rays are defined in
    // camera space, and are transformed into world space. To do this, we need to
    // store the inverse matrices as well.
    XMVECTOR det;
    matrices[2] = XMMatrixInverse(&det, matrices[0]);
    matrices[3] = XMMatrixInverse(&det, matrices[1]);

    // Copy matrix contents
    uint8_t* pData;
    ThrowIfFailed(m_cameraBuffer->Map(0, nullptr, (void**)&pData));
    memcpy(pData, matrices.data(), m_cameraBufferSize);
    m_cameraBuffer->Unmap(0, nullptr);
}

void D3D12HelloTriangle::CreateInstancePropertiesBuffer()
{
    // Allocate memory to hold per-instance information
    uint32_t bufferSize = ROUND_UP((uint32_t)m_instances.size() * sizeof(InstanceProperties), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    // Create the constant buffer for all matrices
    //This buffer is allocated on heap because it will be mapped afterwards (when UpdateInstancePropertiesBuffer() is called)
    m_instancePropertiesBuffer = nv_helpers_dx12::CreateBuffer(m_device.Get(), bufferSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);
}

void D3D12HelloTriangle::UpdateInstancePropertiesBuffer()
{
    InstanceProperties* current = nullptr;
    CD3DX12_RANGE readRange(0, 0); // We do not intend to read from this resource on the CPU.
    ThrowIfFailed(m_instancePropertiesBuffer->Map(0, &readRange, (void**)&current));
    for (const auto& instance : m_instances)
    {
        current->objectToWorld = instance.transformMatrix; //Set the matrix to the matrix set for instance.
        // #DXR Extra - Simple Lighting
        XMMATRIX upper3x3 = instance.transformMatrix;
        // Remove the translation and lower vector of the matrix
        upper3x3.r[0].m128_f32[3] = 0.f;
        upper3x3.r[1].m128_f32[3] = 0.f;
        upper3x3.r[2].m128_f32[3] = 0.f;
        upper3x3.r[3].m128_f32[0] = 0.f;
        upper3x3.r[3].m128_f32[1] = 0.f;
        upper3x3.r[3].m128_f32[2] = 0.f;
        upper3x3.r[3].m128_f32[3] = 1.f;
        XMVECTOR det;
        current->objectToWorldNormal = XMMatrixTranspose(XMMatrixInverse(&det, upper3x3));
        current++; //Go to the next instance's address
    }
    m_instancePropertiesBuffer->Unmap(0, nullptr);
}

void D3D12HelloTriangle::OnButtonDown(UINT32 lParam)
{
    if (!ImGui::GetIO().WantCaptureMouse)
    {
        nv_helpers_dx12::CameraManip.setMousePosition(-GET_X_LPARAM(lParam), -GET_Y_LPARAM(lParam));
    }
}

void D3D12HelloTriangle::OnMouseMove(UINT8 wParam, UINT32 lParam)
{
    if (!ImGui::GetIO().WantCaptureMouse)
    {
        using nv_helpers_dx12::Manipulator;
        Manipulator::Inputs inputs;
        inputs.lmb = wParam & MK_LBUTTON;
        inputs.mmb = wParam & MK_MBUTTON;
        inputs.rmb = wParam & MK_RBUTTON;
        if (!inputs.lmb && !inputs.rmb && !inputs.mmb)
        {
            return; //No mouse buttons pressed
        }

        inputs.ctrl = GetAsyncKeyState(VK_CONTROL);
        inputs.shift = GetAsyncKeyState(VK_SHIFT);
        inputs.alt = GetAsyncKeyState(VK_MENU);

        CameraManip.mouseMove(-GET_X_LPARAM(lParam), -GET_Y_LPARAM(lParam), inputs);
    }
}

// #DXR Extra: Per-Instance Data
void D3D12HelloTriangle::CreatePlaneVB()
{
    // Define the geometry for a plane.
    float planeScale = 40.0f;
    Vertex planeVertices[] = {
        {{-planeScale, -1.0f, +planeScale}}, // 0
        {{+planeScale, -1.0f, +planeScale}}, // 2
        {{-planeScale, -1.0f, -planeScale}}, // 1
        {{-planeScale, -1.0f, -planeScale}}, // 1
        {{+planeScale, -1.0f, +planeScale}}, // 2
        {{+planeScale, -1.0f, -planeScale}}  // 4
    };
    const UINT planeBufferSize = sizeof(planeVertices);

    // Note: using upload heaps to transfer static data like vert buffers is not
    // recommended. Every time the GPU needs it, the upload heap will be
    // marshalled over. Please read up on Default Heap usage. An upload heap is
    // used here for code simplicity and because there are very few verts to
    // actually transfer.
    CD3DX12_HEAP_PROPERTIES heapProperty = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufferResource = CD3DX12_RESOURCE_DESC::Buffer(planeBufferSize);
    ThrowIfFailed(m_device->CreateCommittedResource(&heapProperty, D3D12_HEAP_FLAG_NONE, &bufferResource, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_planeBuffer)));

    // Copy the triangle data to the vertex buffer.
    UINT8* pVertexDataBegin;
    CD3DX12_RANGE readRange(0, 0); //This resource is not intended to be read from the CPU, so its read size is 0.
    ThrowIfFailed(m_planeBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin)));
    memcpy(pVertexDataBegin, planeVertices, planeBufferSize);
    m_planeBuffer->Unmap(0, nullptr);

    // Initialize the vertex buffer view.
    m_planeBufferView.BufferLocation = m_planeBuffer->GetGPUVirtualAddress();
    m_planeBufferView.StrideInBytes = sizeof(Vertex);
    m_planeBufferView.SizeInBytes = planeBufferSize;
}

void D3D12HelloTriangle::CreateGlobalConstantBuffer()
{
    // Due to HLSL packing rules, we create the CB with 9 float4 (each needs to start on a 16-byte boundary)
    XMVECTOR bufferData[] =
    {
        //A matrix
        XMVECTOR{1.0f, 0.0f, 0.0f, 1.0f},
        XMVECTOR{0.7f, 0.4f, 0.0f, 1.0f},
        XMVECTOR{0.4f, 0.7f, 0.0f, 1.0f},

        //B matrix
        XMVECTOR{0.0f, 1.0f, 0.0f, 1.0f},
        XMVECTOR{0.0f, 0.7f, 0.4f, 1.0f},
        XMVECTOR{0.0f, 0.4f, 0.7f, 1.0f},

        //C matrix
        XMVECTOR{0.0f, 0.0f, 1.0f, 1.0f},
        XMVECTOR{0.4f, 0.0f, 0.7f, 1.0f},
        XMVECTOR{0.7f, 0.0f, 0.4f, 1.0f},
    };

    // Create our buffer
    m_globalConstantBuffer = nv_helpers_dx12::CreateBuffer(m_device.Get(), sizeof(bufferData), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);

    // Copy CPU memory to GPU
    uint8_t* pData;
    ThrowIfFailed(m_globalConstantBuffer->Map(0, nullptr, (void**)&pData)); //Map function maps a buffer in GPU memory to a pointer the CPU can access.
    memcpy(pData, bufferData, sizeof(bufferData)); //This copies all the buffer data into the mapped memory, meaning the GPU will be able to access those bytes.
    m_globalConstantBuffer->Unmap(0, nullptr); //Unmaps the memory, meaning the CPU is done writing to it and GPU can safely use that piece of memory.
    //Forgetting to unmap usually results in quite confusing crashes of the application and the debug layer isn't usually all that helpful, it just says the application passed an invalid command.
}

void D3D12HelloTriangle::CreatePerInstanceConstantBuffers()
{
    // Due to HLSL packing rules, we create the CB with 9 float4 (each needs to start on a 16-byte boundary)
    XMVECTOR bufferData[] = {
        // A
        XMVECTOR{1.0f, 0.0f, 0.0f, 1.0f},
        XMVECTOR{1.0f, 0.4f, 0.0f, 1.0f},
        XMVECTOR{1.f, 0.7f, 0.0f, 1.0f},

        // B
        XMVECTOR{0.0f, 1.0f, 0.0f, 1.0f},
        XMVECTOR{0.0f, 1.0f, 0.4f, 1.0f},
        XMVECTOR{0.0f, 1.0f, 0.7f, 1.0f},

        // C
        XMVECTOR{0.0f, 0.0f, 1.0f, 1.0f},
        XMVECTOR{0.4f, 0.0f, 1.0f, 1.0f},
        XMVECTOR{0.7f, 0.0f, 1.0f, 1.0f},
    };
    m_perInstanceConstantBuffers.resize(3);
    int i = 0;
    for (auto& cb : m_perInstanceConstantBuffers)
    {
        const uint32_t bufferSize = sizeof(XMVECTOR) * 3;
        cb = nv_helpers_dx12::CreateBuffer(m_device.Get(), bufferSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);
        uint8_t* pData;
        ThrowIfFailed(cb->Map(0, nullptr, (void**)&pData));
        memcpy(pData, &bufferData[i * 3], bufferSize);
        cb->Unmap(0, nullptr);
        i++;
    }
}

void D3D12HelloTriangle::CreateDepthBuffer()
{
    // Create the depth buffer for rasterization. This buffer needs to be kept in a separate heap.
    // The depth buffer heap type is specific for that usage, and the heap contents are not visible from the shaders
    m_dsvHeap = nv_helpers_dx12::CreateDescriptorHeap(m_device.Get(), 1, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, false);
    // The depth and stencil can be packed into a single 32-bit texture buffer. Since we do not need
    // stencil, we use the 32 bits to store depth information (DXGI_FORMAT_D32_FLOAT). 
    //If stencil is needed, DXGI_FORMAT_D24_UNORM_S8_UINT can be used to add a stencil component to the buffer.
    D3D12_HEAP_PROPERTIES depthHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC depthResourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, m_width, m_height, 1, 1);
    depthResourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    // The depth values will be initialized to 1
    CD3DX12_CLEAR_VALUE depthOptimizedClearValue(DXGI_FORMAT_D32_FLOAT, 1.0f, 0);

    // Allocate the buffer itself, with a state allowing depth writes
    ThrowIfFailed(m_device->CreateCommittedResource(&depthHeapProperties, D3D12_HEAP_FLAG_NONE, &depthResourceDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthOptimizedClearValue, IID_PPV_ARGS(&m_depthStencil)));

    // Write the depth buffer view into the depth buffer heap
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    m_device->CreateDepthStencilView(m_depthStencil.Get(), &dsvDesc, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

void D3D12HelloTriangle::CreateImGuiFontDescriptorHeap()
{
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_imguiFontDescriptorHeap)));
}

void D3D12HelloTriangle::InitializeImGuiContext(bool darkTheme)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;     // Enable Docking
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;   // Enable Multi-Viewport / Platform Windows

    if (darkTheme)
    {
        ImGui::StyleColorsDark();
    }
    else
    {
        ImGui::StyleColorsLight();
    }

    // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) //If viewports are enabled
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(m_windowHandle);
    const int NUM_FRAMES_IN_FLIGHT = 2;
    CreateImGuiFontDescriptorHeap();
    ImGui_ImplDX12_Init(m_device.Get(),
        NUM_FRAMES_IN_FLIGHT,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        m_imguiFontDescriptorHeap.Get(),
        m_imguiFontDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
        m_imguiFontDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

    //If custom fonts are needed, they are to be implemented in this part of the initialization. For now, default font is fine.
    //The procedure to load and use fonts is as explained below:
    // - If no fonts are loaded,  dear imgui will use the default font. You can also load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
    // - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
    // - If the file cannot be loaded, the function will return a nullptr. Please handle those errors in your application (e.g. use an assertion, or display an error and quit).
    // - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
    // - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use Freetype for higher quality font rendering.
    // - Read 'docs/FONTS.md' for more instructions and details.
    // - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
    //io.Fonts->AddFontDefault();
    //io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
    //ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf", 18.0f, nullptr, io.Fonts->GetGlyphRangesJapanese());
    //IM_ASSERT(font != nullptr);
}

// Compute face normals and distribute them as vertex normals
void D3D12HelloTriangle::ComputeVertexNormals(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
{
    // Step 1: Initialize vertex normals to zero
    std::vector<XMFLOAT3> tempNormals(vertices.size(), XMFLOAT3(0.0f, 0.0f, 0.0f));

    // Step 2: Iterate through each triangle and compute the face normal
    for (size_t i = 0; i < indices.size(); i += 3) {
        uint32_t i0 = indices[i];
        uint32_t i1 = indices[i + 1];
        uint32_t i2 = indices[i + 2];

        XMVECTOR v0 = XMLoadFloat3(&vertices[i0].position);
        XMVECTOR v1 = XMLoadFloat3(&vertices[i1].position);
        XMVECTOR v2 = XMLoadFloat3(&vertices[i2].position);

        // Compute the face normal
        XMVECTOR edge1 = v1 - v0;
        XMVECTOR edge2 = v2 - v0;
        XMVECTOR normal = XMVector3Normalize(XMVector3Cross(edge1, edge2));

        // Accumulate the normal for each vertex of the triangle
        XMStoreFloat3(&tempNormals[i0], XMVectorAdd(XMLoadFloat3(&tempNormals[i0]), normal));
        XMStoreFloat3(&tempNormals[i1], XMVectorAdd(XMLoadFloat3(&tempNormals[i1]), normal));
        XMStoreFloat3(&tempNormals[i2], XMVectorAdd(XMLoadFloat3(&tempNormals[i2]), normal));
    }

    // Step 3: Normalize all vertex normals
    for (size_t i = 0; i < vertices.size(); i++) {
        XMVECTOR normal = XMLoadFloat3(&tempNormals[i]);
        normal = XMVector3Normalize(normal);
        XMStoreFloat3(&vertices[i].normal, -normal);
    }
}

void D3D12HelloTriangle::CreateMaterialsBuffer()
{
    uint64_t bufferSizeInBytes = sizeof(Material) * materials.size();
    materialsBuffer = nv_helpers_dx12::CreateBuffer(m_device.Get(), bufferSizeInBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);
    //Update the buffer right after creating so it doesn't have garbage values in it.
    //UpdateMaterialsBuffer function can also be used if more materials are added to the material array in runtime.
    UpdateMaterialsBuffer();
}

void D3D12HelloTriangle::UpdateMaterialsBuffer()
{
    uint64_t bufferSizeInBytes = sizeof(Material) * materials.size();
    uint8_t* p_gpuData;
    ThrowIfFailed(materialsBuffer->Map(0, nullptr, (void**)&p_gpuData));
    memcpy(p_gpuData, (const void*)materials.data(), bufferSizeInBytes);
    materialsBuffer->Unmap(0, nullptr);
}

void D3D12HelloTriangle::UpdateModelWithPendings()
{
    {
        const UINT vertexBufferSizeInBytes = ROUND_UP(pendingVertices.size() * sizeof(Vertex), 256);
        const UINT indexBufferSizeInBytes = ROUND_UP(pendingIndices.size() * sizeof(UINT), 256);

        //WaitForPreviousFrame();
        if (m_modelVertexBuffer != nullptr)
        {
            m_modelVertexBuffer.Reset();
        }
        if (m_modelIndexBuffer != nullptr)
        {
            m_modelIndexBuffer.Reset();
        }
        m_modelVertexBuffer = nv_helpers_dx12::CreateBuffer(m_device.Get(), vertexBufferSizeInBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);
        m_modelIndexBuffer = nv_helpers_dx12::CreateBuffer(m_device.Get(), indexBufferSizeInBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);

        // Copy the triangle data to the vertex buffer.
        UINT8* pVertexDataBegin;
        CD3DX12_RANGE readRange(0, 0); // We do not intend to read from this resource on the CPU.
        ThrowIfFailed(m_modelVertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin)));
        memcpy(pVertexDataBegin, pendingVertices.data(), vertexBufferSizeInBytes);
        m_modelVertexBuffer->Unmap(0, nullptr);
        m_modelVertexCount = pendingVertices.size();

        // Initialize the vertex buffer view.
        m_modelVertexBufferView.BufferLocation = m_modelVertexBuffer->GetGPUVirtualAddress();
        m_modelVertexBufferView.StrideInBytes = sizeof(Vertex);
        m_modelVertexBufferView.SizeInBytes = vertexBufferSizeInBytes;

        // Copy the triangle data to the index buffer.
        UINT8* pIndexDataBegin;
        ThrowIfFailed(m_modelIndexBuffer->Map(0, &readRange, (void**)&pIndexDataBegin));
        memcpy(pIndexDataBegin, pendingIndices.data(), indexBufferSizeInBytes);
        m_modelIndexBuffer->Unmap(0, nullptr);
        m_modelIndexCount = pendingIndices.size();

        // Initialize the index buffer view.
        m_modelIndexBufferView.BufferLocation = m_modelIndexBuffer->GetGPUVirtualAddress();
        m_modelIndexBufferView.Format = DXGI_FORMAT_R32_UINT;
        m_modelIndexBufferView.SizeInBytes = indexBufferSizeInBytes;
    }

    // Reset command allocator and list before doing any GPU work
    ThrowIfFailed(m_commandAllocator->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr));

    AccelerationStructureBuffers modelBottomLevelBuffers = CreateBottomLevelAS({ { m_modelVertexBuffer.Get(), m_modelVertexCount } }, { { m_modelIndexBuffer.Get(), m_modelIndexCount } });
    AccelerationStructureBuffers planeBottomLevelBuffers = CreateBottomLevelAS({ { m_planeBuffer.Get(), 6 } });

    // Ensure BLAS creation is done before moving onto TLAS
    m_fenceValue++;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_fenceValue));
    ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent));
    WaitForSingleObject(m_fenceEvent, INFINITE);

    m_instances = { TLASParams(modelBottomLevelBuffers.pResult, XMMatrixIdentity(), 0, 0),
                    TLASParams(modelBottomLevelBuffers.pResult, XMMatrixTranslation(-5.0f, 0.0f, 5.0f), 0, 0),
                    TLASParams(modelBottomLevelBuffers.pResult, XMMatrixTranslation(-5.0f, 0.0f, 5.0f), 0, 0),
                    TLASParams(modelBottomLevelBuffers.pResult, XMMatrixTranslation(-5.0f, 0.0f, -5.0f), 0, 0),
                    TLASParams(modelBottomLevelBuffers.pResult, XMMatrixTranslation(5.0f, 0.0f, -5.0f), 0, 0),
                    TLASParams(modelBottomLevelBuffers.pResult, XMMatrixTranslation(5.0f, 0.0f, 5.0f), 0, 0),
                    TLASParams(planeBottomLevelBuffers.pResult, XMMatrixIdentity(), 2, 0),
    };

    // Rebuild TLAS
    CreateTopLevelAS(m_instances);

    // Flush the command list and wait for completion
    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, ppCommandLists);

    m_fenceValue++;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_fenceValue));
    ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent));
    WaitForSingleObject(m_fenceEvent, INFINITE);

    // Recreate the descriptor heap and shader binding table to reflect the new TLAS
    CreateShaderResourceHeap();
    CreateShaderBindingTable();
    //One not so great but probably will work solution is to wait for a little bit here so that 
    //the other pieces of the code can finish what they are working on. It does not 100% guarantee that no crash will happen.
    //This is probably the reason a flickering occurs in the release mode. The proper way is probably using fences. No time so hacky solution stays.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void D3D12HelloTriangle::QueueModelVertexAndIndexBufferUpdates(std::vector<XMFLOAT3>& vertexPoints, std::vector<UINT>& indices)
{
     //Update the vertex and index buffers
     {
        std::vector<Vertex> vertices;
        vertices.reserve(vertexPoints.size());
        for (XMFLOAT3& vertexPoint : vertexPoints)
        {
            vertices.push_back(Vertex(vertexPoint));
        }

        ComputeVertexNormals(vertices, indices);

        //Clear all the data that might exist on the pending buffers.
        pendingVertices.clear();
        pendingIndices.clear();
        for (Vertex& v : vertices)
        {
            pendingVertices.push_back(v);
        }
        for (UINT& index : indices)
        {
            pendingIndices.push_back(index);
        }
        pendingModelUpdate = true;
     }
}
