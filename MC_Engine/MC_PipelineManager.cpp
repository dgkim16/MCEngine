#include "MCEngine.h"
#include "ShaderLib.h"

void MCEngine::BuildRootSignature()
{
	CD3DX12_DESCRIPTOR_RANGE texTable0;
	texTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[4];
	CD3DX12_ROOT_PARAMETER depthRootParameter[2];

	// Create root CBVs.
	// slotRootParameter[0].InitAsDescriptorTable(1, &cbvTable0); 
	slotRootParameter[0].InitAsConstantBufferView(0); // cbPerObject, b0
	slotRootParameter[1].InitAsConstantBufferView(1); // cbPass, b1
	slotRootParameter[2].InitAsShaderResourceView(0); // materials
	slotRootParameter[3].InitAsShaderResourceView(1); // instanced

	depthRootParameter[0].InitAsDescriptorTable(1, &texTable0, D3D12_SHADER_VISIBILITY_PIXEL); // gDepthTex
	depthRootParameter[1].InitAsConstantBufferView(0); //cbDebug at register(b0)

	auto staticSamplers = d3dUtil::GetStaticSamplers();

	std::vector<CD3DX12_ROOT_SIGNATURE_DESC> rootSigDescs;
	rootSigDescs.push_back(CD3DX12_ROOT_SIGNATURE_DESC(4, slotRootParameter, (UINT)staticSamplers.size(), staticSamplers.data(),
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED ));
	rootSigDescs.push_back(CD3DX12_ROOT_SIGNATURE_DESC(2, depthRootParameter, (UINT)staticSamplers.size(), staticSamplers.data(),
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT));
	mRootSignatures.resize(rootSigDescs.size());
	for (int i = 0; i < rootSigDescs.size(); i++) {
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDescs[i], D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());
		if (errorBlob != nullptr)
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		ThrowIfFailed(hr);

		// create ID3D12RootSignature-type root signature via ID3D12Device, using serialized ID3DBlob-type root signature
		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures[i].GetAddressOf())));
		serializedRootSig.Reset();
		errorBlob.Reset();
	}

	// example for direct indexing in Compute Shader
	CD3DX12_ROOT_PARAMETER computeRootParameters[1];
	computeRootParameters[0].InitAsConstantBufferView(0); //b0
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	CD3DX12_ROOT_SIGNATURE_DESC computeRootSigDesc(
		1, computeRootParameters,
		0,nullptr,
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
	HRESULT hr = D3D12SerializeRootSignature(&computeRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());
	if (errorBlob != nullptr)
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	ThrowIfFailed(hr);
	ThrowIfFailed(md3dDevice->CreateRootSignature(0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(mComputeRootSignature.GetAddressOf())));
	serializedRootSig.Reset();
	errorBlob.Reset();

	// Debug line root signature: root constants (color) + per-pass CBV
	{
		CD3DX12_ROOT_PARAMETER debugParams[2];
		debugParams[0].InitAsConstants(4, 0);           // b0: float4 gColor (4 root constants)
		debugParams[1].InitAsConstantBufferView(1);     // b1: per-pass CB (gViewProj)
		CD3DX12_ROOT_SIGNATURE_DESC debugRSDesc(2, debugParams, 0, nullptr,
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
		ComPtr<ID3DBlob> serializedDebugRS, debugError;
		HRESULT hrD = D3D12SerializeRootSignature(&debugRSDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedDebugRS.GetAddressOf(), debugError.GetAddressOf());
		if (debugError) ::OutputDebugStringA((char*)debugError->GetBufferPointer());
		ThrowIfFailed(hrD);
		ThrowIfFailed(md3dDevice->CreateRootSignature(0,
			serializedDebugRS->GetBufferPointer(), serializedDebugRS->GetBufferSize(),
			IID_PPV_ARGS(mDebugLineRootSig.ReleaseAndGetAddressOf())));
	}

	// Grass culling compute root signature
	// Directly bound (no heap indexing): CBV + SRV + 2x UAV
	{
		CD3DX12_ROOT_PARAMETER grassCullParams[4];
		grassCullParams[0].InitAsConstantBufferView(0);  // b0: GrassCullCB
		grassCullParams[1].InitAsShaderResourceView(0);  // t0: full instance buffer
		grassCullParams[2].InitAsUnorderedAccessView(0); // u0: visible instance buffer
		grassCullParams[3].InitAsUnorderedAccessView(1); // u1: counter buffer
		CD3DX12_ROOT_SIGNATURE_DESC grassCullRSDesc(4, grassCullParams, 0, nullptr,
			D3D12_ROOT_SIGNATURE_FLAG_NONE);
		ComPtr<ID3DBlob> serializedGrassCullRS;
		ComPtr<ID3DBlob> grassCullError;
		HRESULT hrGC = D3D12SerializeRootSignature(&grassCullRSDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedGrassCullRS.GetAddressOf(), grassCullError.GetAddressOf());
		if (grassCullError != nullptr)
			::OutputDebugStringA((char*)grassCullError->GetBufferPointer());
		ThrowIfFailed(hrGC);
		ThrowIfFailed(md3dDevice->CreateRootSignature(0,
			serializedGrassCullRS->GetBufferPointer(),
			serializedGrassCullRS->GetBufferSize(),
			IID_PPV_ARGS(mGrassCullRootSignature.GetAddressOf())));
	}
}

void MCEngine::BuildShadersAndInputLayout()
{
	OutputDebugString(L"Building shaders...");
#if SHDAER_MAJOR >= 6
	ShaderLibDx::GetLib().Init();
#else
	ShaderLib::GetLib().Init();
#endif
	OutputDebugString(L"Building input layout...");
	mInputLayout["default"] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },	 // 32bit * 3 = 12 bytes
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },		 // 12 bytes
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	mInputLayout["treeSprite"] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "SIZE", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
	OutputDebugString(L"attempting to create inputlayout for tess..");

	mInputLayout["tessellation"] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	OutputDebugString(L"success to create inputlayout for tess..");

	mInputLayout["default_Instance"] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 0 },	 // 32bit * 3 = 12 bytes
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 0 },		 // 12 bytes
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 0 },
	};

	mInputLayout["debug_line"] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

}

void MCEngine::BuildPSOs()
{
#if SHDAER_MAJOR >= 6
	ShaderLibDx& mShaders = ShaderLibDx::GetLib();
#ifndef GETBYTE
#define GETBYTE d3dUtil::ByteCodeFromBlobXc
#endif
#else
	ShaderLib& mShaders = ShaderLib::GetLib();
#define GETBYTE d3dUtil::ByteCodeFromBlob
#endif
	auto sv = mShaders["standardVS"];
	if (sv == nullptr) {
		OutputDebugString(L"BuildPSO - nullptr\n");
	}
	// PSO for opaque objects.
	D3D12_GRAPHICS_PIPELINE_STATE_DESC opaquePsoDesc;
	{
		ZeroMemory(&opaquePsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
		opaquePsoDesc.InputLayout = { mInputLayout["default"].data(), (UINT)mInputLayout["default"].size() };
		opaquePsoDesc.pRootSignature = mRootSignatures[0].Get();
		opaquePsoDesc.VS = GETBYTE(mShaders["standardVS"]);
		opaquePsoDesc.PS = GETBYTE(mShaders["opaquePS"]);
		opaquePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		opaquePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		opaquePsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		opaquePsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		opaquePsoDesc.SampleMask = UINT_MAX;
		opaquePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		opaquePsoDesc.NumRenderTargets = 1;
		opaquePsoDesc.RTVFormats[0] = mSceneFormat;
		opaquePsoDesc.SampleDesc.Count = 1;
		opaquePsoDesc.SampleDesc.Quality = 0;
		opaquePsoDesc.DSVFormat = mDepthStencilFormat;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaquePsoDesc, IID_PPV_ARGS(&mPSOs["opaque"])));
	}
	RegisterPSOVariants("opaque", opaquePsoDesc, true, true);
	/*
	D3D12_GRAPHICS_PIPELINE_STATE_DESC opaquePsoDescMSAA = opaquePsoDesc;
	opaquePsoDescMSAA.SampleDesc.Count = 4;
	opaquePsoDescMSAA.SampleDesc.Quality = m4xMsaaQuality - 1;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaquePsoDescMSAA, IID_PPV_ARGS(&mPSOs["opaque_MSAA"])));

	// PSO for opaque wireframe objects.
	D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueWireframePsoDesc = opaquePsoDesc;
	{
		opaqueWireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaqueWireframePsoDesc, IID_PPV_ARGS(&mPSOs["opaque_wireframe"])));
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueWireframePsoDescMSAA = opaquePsoDescMSAA;
	opaqueWireframePsoDescMSAA.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaqueWireframePsoDescMSAA, IID_PPV_ARGS(&mPSOs["opaque_wireframe_MSAA"])));
	*/
	D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueInstancedPsoDesc = opaquePsoDesc;
	opaqueInstancedPsoDesc.VS = GETBYTE(mShaders["instancedVS"]);
	opaqueInstancedPsoDesc.PS = GETBYTE(mShaders["instancedPS"]);
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaqueInstancedPsoDesc, IID_PPV_ARGS(&mPSOs["opaque_instanced"])));
	RegisterPSOVariants("opaque_instanced", opaqueInstancedPsoDesc, true, true);
	/*
	D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueInstancedPsoDescMSAA = opaqueInstancedPsoDesc;
	opaqueInstancedPsoDescMSAA.SampleDesc = opaquePsoDescMSAA.SampleDesc;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaqueInstancedPsoDescMSAA, IID_PPV_ARGS(&mPSOs["opaque_instanced_MSAA"])));

	D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueWireframeInstancedPsoDesc = opaqueInstancedPsoDesc;
	opaqueWireframeInstancedPsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaqueWireframeInstancedPsoDesc, IID_PPV_ARGS(&mPSOs["opaque_instanced_wireframe"])));

	D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueWireframeInstancedPsoDescMSAA = opaqueInstancedPsoDescMSAA;
	opaqueWireframeInstancedPsoDescMSAA.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaqueWireframeInstancedPsoDescMSAA, IID_PPV_ARGS(&mPSOs["opaque_instanced_wireframe_MSAA"])));
	*/
	D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueInstancedTessPsoDesc = opaqueInstancedPsoDesc;
	opaqueInstancedTessPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
	opaqueInstancedTessPsoDesc.VS = GETBYTE(mShaders["instancedTessVS"]);
	opaqueInstancedTessPsoDesc.HS = GETBYTE(mShaders["instancedTessHS"]);
	opaqueInstancedTessPsoDesc.DS = GETBYTE(mShaders["instancedTessDS"]);
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaqueInstancedTessPsoDesc, IID_PPV_ARGS(&mPSOs["opaque_instanced_tess"])));
	RegisterPSOVariants("opaque_instanced_tess", opaqueInstancedTessPsoDesc);

	D3D12_GRAPHICS_PIPELINE_STATE_DESC grassInstancedPsoDesc = opaqueInstancedTessPsoDesc;
	grassInstancedPsoDesc.VS = GETBYTE(mShaders["grassVS"]);
	grassInstancedPsoDesc.HS = GETBYTE(mShaders["grassHS"]);
	grassInstancedPsoDesc.DS = GETBYTE(mShaders["grassDS"]);
	grassInstancedPsoDesc.PS = GETBYTE(mShaders["grassPS"]);
	grassInstancedPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&grassInstancedPsoDesc, IID_PPV_ARGS(&mPSOs["grass_instanced"])));
	RegisterPSOVariants("grass_instanced", grassInstancedPsoDesc,true,true);

	// PSO for transparent objects
	D3D12_GRAPHICS_PIPELINE_STATE_DESC transparentPsoDesc = opaquePsoDesc;
	{
		transparentPsoDesc.PS = GETBYTE(mShaders["transparentPS"]);

		D3D12_RENDER_TARGET_BLEND_DESC transparencyBlendDesc;
		transparencyBlendDesc.BlendEnable = true;
		transparencyBlendDesc.LogicOpEnable = false;
		transparencyBlendDesc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
		transparencyBlendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		transparencyBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
		transparencyBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
		transparencyBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
		transparencyBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		transparencyBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
		transparencyBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

		transparentPsoDesc.BlendState.RenderTarget[0] = transparencyBlendDesc;
		transparentPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		transparentPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&transparentPsoDesc, IID_PPV_ARGS(&mPSOs["transparent"])));
	}
	RegisterPSOVariants("transparent", transparentPsoDesc, true, false);

	D3D12_GRAPHICS_PIPELINE_STATE_DESC alphaTestedPsoDesc = opaquePsoDesc;
	{
		alphaTestedPsoDesc.PS = GETBYTE(mShaders["alphaTestedPS"]);
		alphaTestedPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&alphaTestedPsoDesc, IID_PPV_ARGS(&mPSOs["alphaTested"])));
	}
	RegisterPSOVariants("alphaTested", alphaTestedPsoDesc, true);
	/*
	D3D12_GRAPHICS_PIPELINE_STATE_DESC alphaTestedPsoDescMSAA = alphaTestedPsoDesc;
	alphaTestedPsoDescMSAA.SampleDesc = opaquePsoDescMSAA.SampleDesc;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&alphaTestedPsoDescMSAA, IID_PPV_ARGS(&mPSOs["alphaTested_MSAA"])));
	*/
	D3D12_GRAPHICS_PIPELINE_STATE_DESC alphaTestedInstancedPsoDesc = alphaTestedPsoDesc;
	alphaTestedInstancedPsoDesc.VS = GETBYTE(mShaders["instancedVS"]);
	alphaTestedInstancedPsoDesc.PS = GETBYTE(mShaders["instancedPS"]);
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&alphaTestedInstancedPsoDesc, IID_PPV_ARGS(&mPSOs["alphaTested_instanced"])));
	RegisterPSOVariants("alphaTested_instanced", alphaTestedInstancedPsoDesc, true, true);
	/*
	D3D12_GRAPHICS_PIPELINE_STATE_DESC alphaTestedInstancedPsoDescMSAA = alphaTestedInstancedPsoDesc;
	alphaTestedInstancedPsoDescMSAA.SampleDesc = opaquePsoDescMSAA.SampleDesc;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&alphaTestedInstancedPsoDescMSAA, IID_PPV_ARGS(&mPSOs["alphaTested_instanced_MSAA"])));

	D3D12_GRAPHICS_PIPELINE_STATE_DESC alphaTestedWireframeInstancedPsoDesc = alphaTestedInstancedPsoDesc;
	alphaTestedWireframeInstancedPsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&alphaTestedWireframeInstancedPsoDesc, IID_PPV_ARGS(&mPSOs["alphaTested_instanced_wireframe"])));

	D3D12_GRAPHICS_PIPELINE_STATE_DESC alphaTestedWireframeInstancedPsoDescMSAA = alphaTestedInstancedPsoDescMSAA;
	alphaTestedWireframeInstancedPsoDescMSAA.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&alphaTestedWireframeInstancedPsoDescMSAA, IID_PPV_ARGS(&mPSOs["alphaTested_instanced_wireframe_MSAA"])));
	*/
	// PSO for depth debug
	D3D12_GRAPHICS_PIPELINE_STATE_DESC depthDebugPsoDesc = {};
	{
		depthDebugPsoDesc.InputLayout = { nullptr, 0 };
		depthDebugPsoDesc.pRootSignature = mRootSignatures[1].Get();
		depthDebugPsoDesc.VS = GETBYTE(mShaders["depthDebugVS"]);
		depthDebugPsoDesc.PS = GETBYTE(mShaders["depthDebugPS"]);
		depthDebugPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		depthDebugPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

		D3D12_DEPTH_STENCIL_DESC dsDesc = {};
		dsDesc.DepthEnable = FALSE;
		dsDesc.StencilEnable = FALSE;
		depthDebugPsoDesc.DepthStencilState = dsDesc;

		depthDebugPsoDesc.SampleMask = UINT_MAX;
		depthDebugPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		depthDebugPsoDesc.NumRenderTargets = 1;
		depthDebugPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		depthDebugPsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
		depthDebugPsoDesc.SampleDesc.Count = 1;
		depthDebugPsoDesc.SampleDesc.Quality = 0;

		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&depthDebugPsoDesc, IID_PPV_ARGS(&mPSOs["depthDebug"])));
	}
	
	D3D12_GRAPHICS_PIPELINE_STATE_DESC depthDebugPsoDescMSAA = depthDebugPsoDesc;
	depthDebugPsoDescMSAA.PS = GETBYTE(mShaders["depthDebugMSAAPS"]);
	// for MSAA, PS uses Texture2DMS<float> instead. That's the only difference
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&depthDebugPsoDescMSAA, IID_PPV_ARGS(&mPSOs["depthDebug_MSAA"])));
	

	// PSO for marking stencil
	D3D12_GRAPHICS_PIPELINE_STATE_DESC markMirrorsPsoDesc = opaquePsoDesc;
	{
		CD3DX12_BLEND_DESC stencilBlendState(D3D12_DEFAULT);
		stencilBlendState.RenderTarget[0].RenderTargetWriteMask = 0;

		D3D12_DEPTH_STENCIL_DESC mirrorDSS;
		mirrorDSS.DepthEnable = true;
		mirrorDSS.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		mirrorDSS.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		mirrorDSS.StencilEnable = true;
		mirrorDSS.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
		mirrorDSS.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;

		mirrorDSS.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		mirrorDSS.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		mirrorDSS.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		mirrorDSS.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

		mirrorDSS.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		mirrorDSS.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		mirrorDSS.BackFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		mirrorDSS.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

		markMirrorsPsoDesc.BlendState = stencilBlendState;
		markMirrorsPsoDesc.DepthStencilState = mirrorDSS;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&markMirrorsPsoDesc, IID_PPV_ARGS(&mPSOs["markStencilMirrors"])));
	}
	RegisterPSOVariants("markStencilMirrors", markMirrorsPsoDesc, true, false);
	/*
	D3D12_GRAPHICS_PIPELINE_STATE_DESC stencilPSOMSAA = markMirrorsPsoDesc;
	stencilPSOMSAA.SampleDesc = opaquePsoDescMSAA.SampleDesc;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&stencilPSOMSAA, IID_PPV_ARGS(&mPSOs["markStencilMirrors_MSAA"])));
	*/

	// PSO for drawing at stencil marked region only
	D3D12_GRAPHICS_PIPELINE_STATE_DESC drawReflectionsPsoDesc = opaquePsoDesc;
	{
		D3D12_DEPTH_STENCIL_DESC reflectionsDSS;
		reflectionsDSS.DepthEnable = true;
		reflectionsDSS.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		reflectionsDSS.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		reflectionsDSS.StencilEnable = true;
		reflectionsDSS.StencilReadMask = 0xff;
		reflectionsDSS.StencilWriteMask = 0xff;

		reflectionsDSS.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		reflectionsDSS.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		reflectionsDSS.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		reflectionsDSS.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;

		reflectionsDSS.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		reflectionsDSS.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		reflectionsDSS.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		reflectionsDSS.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;

		drawReflectionsPsoDesc.DepthStencilState = reflectionsDSS;
		drawReflectionsPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
		drawReflectionsPsoDesc.RasterizerState.FrontCounterClockwise = false;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&drawReflectionsPsoDesc, IID_PPV_ARGS(&mPSOs["drawStencilReflections"])));
	}
	RegisterPSOVariants("drawStencilReflections", drawReflectionsPsoDesc, true, true);
	/*
	D3D12_GRAPHICS_PIPELINE_STATE_DESC drawReflectionsPsoDescMSAA = drawReflectionsPsoDesc;
	drawReflectionsPsoDescMSAA.SampleDesc = opaquePsoDescMSAA.SampleDesc;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&drawReflectionsPsoDescMSAA, IID_PPV_ARGS(&mPSOs["drawStencilReflections_MSAA"])));
	*/
	// PSO for shadow
	D3D12_GRAPHICS_PIPELINE_STATE_DESC shadowPsoDesc = transparentPsoDesc;
	{
		D3D12_DEPTH_STENCIL_DESC shadowDSS;
		shadowDSS.DepthEnable = true;
		shadowDSS.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		shadowDSS.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		shadowDSS.StencilEnable = true;
		shadowDSS.StencilReadMask = 0xff;
		shadowDSS.StencilWriteMask = 0xff;

		shadowDSS.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		shadowDSS.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		shadowDSS.FrontFace.StencilPassOp = D3D12_STENCIL_OP_INCR;
		shadowDSS.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;

		shadowDSS.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		shadowDSS.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		shadowDSS.BackFace.StencilPassOp = D3D12_STENCIL_OP_INCR;
		shadowDSS.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;

		shadowPsoDesc.DepthStencilState = shadowDSS;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&shadowPsoDesc, IID_PPV_ARGS(&mPSOs["shadow"])));
	}
	RegisterPSOVariants("shadow", shadowPsoDesc, true);
	/*
	D3D12_GRAPHICS_PIPELINE_STATE_DESC shadowPsoDescMSAA = shadowPsoDesc;
	shadowPsoDescMSAA.SampleDesc = opaquePsoDescMSAA.SampleDesc;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&shadowPsoDescMSAA, IID_PPV_ARGS(&mPSOs["shadow_MSAA"])));
	*/
	// PSO for tree sprites
	D3D12_GRAPHICS_PIPELINE_STATE_DESC treeSpritePsoDesc = opaquePsoDesc;
	{
		treeSpritePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
		treeSpritePsoDesc.InputLayout = { mInputLayout["treeSprite"].data(), (UINT)mInputLayout["treeSprite"].size() };

		treeSpritePsoDesc.VS = GETBYTE(mShaders["treesVS"]);
		treeSpritePsoDesc.GS = GETBYTE(mShaders["treesGS"]);
		treeSpritePsoDesc.PS = GETBYTE(mShaders["treesPS"]);

		treeSpritePsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&treeSpritePsoDesc, IID_PPV_ARGS(&mPSOs["treeSprites"])));
	}
	RegisterPSOVariants("treeSprites", treeSpritePsoDesc, true, true);
	/*
	D3D12_GRAPHICS_PIPELINE_STATE_DESC treeSpritePsoDescMSAA = treeSpritePsoDesc;
	treeSpritePsoDescMSAA.SampleDesc = opaquePsoDescMSAA.SampleDesc;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&treeSpritePsoDescMSAA, IID_PPV_ARGS(&mPSOs["treeSprites_MSAA"])));
	*/
	D3D12_GRAPHICS_PIPELINE_STATE_DESC tessellationPsoDesc = opaquePsoDesc;
	tessellationPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
	// tessellationPsoDesc.InputLayout = { mInputLayout["tessellation"].data(), (UINT)mInputLayout["tessellation"].size() };
	tessellationPsoDesc.VS = GETBYTE(mShaders["tessVS"]);
	//tessellationPsoDesc.PS = GETBYTE(mShaders["tessPS"]);
	tessellationPsoDesc.HS = GETBYTE(mShaders["tessHS"]);
	tessellationPsoDesc.DS = GETBYTE(mShaders["tessDS"]);
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&tessellationPsoDesc, IID_PPV_ARGS(&mPSOs["tessellation"])));
	RegisterPSOVariants("tessellation", tessellationPsoDesc);

	D3D12_COMPUTE_PIPELINE_STATE_DESC forceAlphaCsPSO = {};
	forceAlphaCsPSO.pRootSignature = mComputeRootSignature.Get();
	forceAlphaCsPSO.CS = GETBYTE(mShaders["forceAlphaCS"]);
	forceAlphaCsPSO.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
	ThrowIfFailed(md3dDevice->CreateComputePipelineState(&forceAlphaCsPSO, IID_PPV_ARGS(&mPSOs["forceAlphaOne"])));

	D3D12_COMPUTE_PIPELINE_STATE_DESC horzBlurPSO = forceAlphaCsPSO;
	D3D12_COMPUTE_PIPELINE_STATE_DESC vertBlurPSO = forceAlphaCsPSO;
	horzBlurPSO.CS = GETBYTE(mShaders["horzBlurCS"]);
	vertBlurPSO.CS = GETBYTE(mShaders["vertBlurCS"]);
	ThrowIfFailed(md3dDevice->CreateComputePipelineState(&horzBlurPSO, IID_PPV_ARGS(&mPSOs["horzBlur"])));
	ThrowIfFailed(md3dDevice->CreateComputePipelineState(&vertBlurPSO, IID_PPV_ARGS(&mPSOs["vertBlur"])));

	D3D12_COMPUTE_PIPELINE_STATE_DESC sobelPSO = forceAlphaCsPSO;
	sobelPSO.CS = GETBYTE(mShaders["sobelCS"]);
	ThrowIfFailed(md3dDevice->CreateComputePipelineState(&sobelPSO, IID_PPV_ARGS(&mPSOs["sobel"])));

	// Grass culling compute PSO
	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC grassCullPSO = {};
		grassCullPSO.pRootSignature = mGrassCullRootSignature.Get();
		grassCullPSO.CS = GETBYTE(mShaders["grassCullCS"]);
		grassCullPSO.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
		ThrowIfFailed(md3dDevice->CreateComputePipelineState(&grassCullPSO, IID_PPV_ARGS(&mPSOs["grass_cull_cs"])));
	}

	// Command signature for ExecuteIndirect (DrawIndexedInstanced indirect)
	{
		D3D12_INDIRECT_ARGUMENT_DESC indirectArgDesc = {};
		indirectArgDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
		D3D12_COMMAND_SIGNATURE_DESC cmdSigDesc = {};
		cmdSigDesc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
		cmdSigDesc.NumArgumentDescs = 1;
		cmdSigDesc.pArgumentDescs = &indirectArgDesc;
		ThrowIfFailed(md3dDevice->CreateCommandSignature(&cmdSigDesc, nullptr,
			IID_PPV_ARGS(mGrassCommandSignature.ReleaseAndGetAddressOf())));
	}

	// PSO for debug line visualization (bounding boxes, frustum)
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC debugLinePsoDesc = {};
		debugLinePsoDesc.InputLayout    = { mInputLayout["debug_line"].data(), (UINT)mInputLayout["debug_line"].size() };
		debugLinePsoDesc.pRootSignature = mDebugLineRootSig.Get();
		debugLinePsoDesc.VS             = GETBYTE(mShaders["debugLineVS"]);
		debugLinePsoDesc.PS             = GETBYTE(mShaders["debugLinePS"]);
		debugLinePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		debugLinePsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		debugLinePsoDesc.BlendState     = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

		// Always visible — disable depth test so debug lines show through geometry
		D3D12_DEPTH_STENCIL_DESC debugDSS = {};
		debugDSS.DepthEnable    = FALSE;
		debugDSS.StencilEnable  = FALSE;
		debugLinePsoDesc.DepthStencilState = debugDSS;

		debugLinePsoDesc.SampleMask           = UINT_MAX;
		debugLinePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
		debugLinePsoDesc.NumRenderTargets     = 1;
		debugLinePsoDesc.RTVFormats[0]        = mSceneFormat;
		debugLinePsoDesc.SampleDesc.Count     = 1;
		debugLinePsoDesc.SampleDesc.Quality   = 0;
		debugLinePsoDesc.DSVFormat            = mDepthStencilFormat;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&debugLinePsoDesc, IID_PPV_ARGS(&mPSOs["debug_line"])));

		D3D12_GRAPHICS_PIPELINE_STATE_DESC debugLineMSAA = debugLinePsoDesc;
		debugLineMSAA.SampleDesc = { 4, m4xMsaaQuality - 1 };
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&debugLineMSAA, IID_PPV_ARGS(&mPSOs["debug_line_MSAA"])));
	}
}

void MCEngine::RegisterPSOVariants(const std::string& name,
	const D3D12_GRAPHICS_PIPELINE_STATE_DESC& base,
	bool withMSAA, bool withWireframe)
{
	DXGI_SAMPLE_DESC msaaSample = { 4, m4xMsaaQuality - 1 };

	if (withMSAA) {
		D3D12_GRAPHICS_PIPELINE_STATE_DESC msaa = base;
		msaa.SampleDesc = msaaSample;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&msaa, IID_PPV_ARGS(&mPSOs[name + "_MSAA"])));
	}
	if (withWireframe) {
		D3D12_GRAPHICS_PIPELINE_STATE_DESC wire = base;
		wire.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&wire, IID_PPV_ARGS(&mPSOs[name + "_wireframe"])));

		if (withMSAA) {
			D3D12_GRAPHICS_PIPELINE_STATE_DESC wireMSAA = wire;
			wireMSAA.SampleDesc = msaaSample;
			ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&wireMSAA, IID_PPV_ARGS(&mPSOs[name + "_wireframe_MSAA"])));
		}
	}
}