#include "Renderer/D3D12/d3dUtil.h"
#include <filesystem>

using namespace Microsoft::WRL;

namespace
{
// ToString()'s text, built from the constructor arguments: the std::runtime_error
// base is initialized before the members exist.
std::wstring DxErrorText(HRESULT hr, const std::wstring& functionName, const std::wstring& filename, int lineNumber)
{
    // Get the string description of the error code.
    _com_error err(hr);
    std::wstring msg = err.ErrorMessage();

    return functionName + L" failed in " + filename + L"; line " + std::to_wstring(lineNumber) + L"; error: " + msg;
}

// The inverse of AnsiToWString, which ThrowIfFailed uses for the file name.
std::string WStringToAnsi(const std::wstring& text)
{
    const int length = WideCharToMultiByte(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string ansi(length > 0 ? length : 0, '\0');
    if (length > 0)
        WideCharToMultiByte(CP_ACP, 0, text.data(), static_cast<int>(text.size()), ansi.data(), length, nullptr, nullptr);
    return ansi;
}
}

DxException::DxException(HRESULT hr, const std::wstring& functionName, const std::wstring& filename, int lineNumber) :
    std::runtime_error(WStringToAnsi(DxErrorText(hr, functionName, filename, lineNumber))),
    ErrorCode(hr),
    FunctionName(functionName),
    Filename(filename),
    LineNumber(lineNumber)
{
}

std::wstring DxException::ToString()const
{
    return DxErrorText(ErrorCode, FunctionName, Filename, LineNumber);
}

ComPtr<ID3D12Resource> d3dUtil::CreateDefaultBuffer(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const void* initData,
    UINT64 byteSize,
    ComPtr<ID3D12Resource>& uploadBuffer
) {
    // create Default buffer
    ComPtr<ID3D12Resource> defaultBuffer;
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto bufferSize = CD3DX12_RESOURCE_DESC::Buffer(byteSize);
    ThrowIfFailed(device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferSize,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(defaultBuffer.GetAddressOf())
    ));

    // Create Intermediate Buffer
    heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    bufferSize = CD3DX12_RESOURCE_DESC::Buffer(byteSize);
    ThrowIfFailed(device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferSize,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(uploadBuffer.GetAddressOf())
    ));

    // Describe Data we want to copy to default buffer
    D3D12_SUBRESOURCE_DATA subResourceData = {};
    subResourceData.pData = initData;
    subResourceData.RowPitch = byteSize;
    subResourceData.SlicePitch = subResourceData.RowPitch;

    // Schedule task of changing default buffer's state as 'copy destination'
    auto transition = CD3DX12_RESOURCE_BARRIER::Transition(defaultBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->ResourceBarrier(1,&transition);

    //      UpdateSubResources<1>()
    // 1. copy CPU memory into intermediate upload heap
    // 2. copies data in intermediate upload heap to default heap
    //    by calling    ID3D12CommandList::CopySubresourceRegion()
    UpdateSubresources<1>(cmdList, defaultBuffer.Get(), uploadBuffer.Get(), 0, 0, 1, &subResourceData);

    // Schedule task of changing default buffer's state to 'generic read'
    transition = CD3DX12_RESOURCE_BARRIER::Transition(defaultBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
    cmdList->ResourceBarrier(1,&transition);
    
    return defaultBuffer;
}

// See "HLSL Compiler | Michael Dougherty | DirectX Developer Day"
// https://www.youtube.com/watch?v=tyyKeTsdtmo
ComPtr<IDxcBlob> d3dUtil::CompileShaderDX(
    const std::wstring& filename,
    std::vector<LPCWSTR>& compileArgs)
{
    static ComPtr<IDxcUtils> utils = nullptr;
    static ComPtr<IDxcCompiler3> compiler = nullptr;
    static ComPtr<IDxcIncludeHandler> defaultIncludeHandler = nullptr;

    if (!std::filesystem::exists(filename))
    {
        std::wstring msg = filename + L" not found.";
        OutputDebugStringW(msg.c_str());
        MessageBox(0, msg.c_str(), 0, 0);
    }

    // Only need one of these.
    if (compiler == nullptr)
    {
        ThrowIfFailed(DxcCreateInstance(
            CLSID_DxcUtils, IID_PPV_ARGS(&utils)));
        ThrowIfFailed(DxcCreateInstance(
            CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));
        ThrowIfFailed(utils->CreateDefaultIncludeHandler(
            &defaultIncludeHandler));
    }

    // Use IDxcUtils to load the text file.
    uint32_t codePage = CP_UTF8;
    ComPtr<IDxcBlobEncoding> sourceBlob = nullptr;
    ThrowIfFailed(utils->LoadFile(filename.c_str(), &codePage, &sourceBlob));

    // Create a DxcBuffer buffer to the source code.
    DxcBuffer sourceBuffer = {};
    sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
    sourceBuffer.Size = sourceBlob->GetBufferSize();
    sourceBuffer.Encoding = DXC_CP_UTF8;

    // Prepend filename so DXC can resolve relative includes correctly.
    std::vector<LPCWSTR> finalArgs;
    finalArgs.reserve(compileArgs.size() + 1);
    finalArgs.push_back(filename.c_str());
    finalArgs.insert(finalArgs.end(), compileArgs.begin(), compileArgs.end());

    ComPtr<IDxcResult> result = nullptr;
    HRESULT hr = compiler->Compile(
        &sourceBuffer,               // source code
        finalArgs.data(),            // arguments
        (UINT)finalArgs.size(),      // argument count
        defaultIncludeHandler.Get(), // include handler
        IID_PPV_ARGS(result.GetAddressOf())); // output

    if (SUCCEEDED(hr))
        result->GetStatus(&hr);

    // Get errors and output them if any.
    ComPtr<IDxcBlobUtf8> errorMsgs = nullptr;
    result->GetOutput(DXC_OUT_ERRORS,
        IID_PPV_ARGS(&errorMsgs), nullptr);

    if (errorMsgs && errorMsgs->GetStringLength())
    {
        std::wstring errorText = AnsiToWString(errorMsgs->GetStringPointer());

        // Replace the hlsl.hlsl placeholder only if it actually exists.
        std::wstring dummyFilename = L"hlsl.hlsl";
        size_t pos = errorText.find(dummyFilename);
        if (pos != std::wstring::npos)
        {
            errorText.replace(pos, dummyFilename.length(), filename);
        }

        OutputDebugStringW(errorText.c_str());
        ThrowIfFailed(E_FAIL);
    }

    ThrowIfFailed(hr);

    // Get the DX intermediate language, which the GPU driver will translate
    // into native GPU code.
    ComPtr<IDxcBlob> dxil = nullptr;
    ThrowIfFailed(result->GetOutput(DXC_OUT_OBJECT,
        IID_PPV_ARGS(&dxil), nullptr));

#if defined(DEBUG) || defined(_DEBUG)
    // Write PDB data for PIX debugging.
    const std::string pdbDirectory = "HLSL PDB/";
    if (!std::filesystem::exists(pdbDirectory))
    {
        std::filesystem::create_directory(pdbDirectory);
    }

    ComPtr<IDxcBlob> pdbData = nullptr;
    ComPtr<IDxcBlobUtf16> pdbPathFromCompiler = nullptr;
    ThrowIfFailed(result->GetOutput(DXC_OUT_PDB, IID_PPV_ARGS(&pdbData), &pdbPathFromCompiler));
    WriteBinaryToFile(pdbData.Get(),
        AnsiToWString(pdbDirectory) +
        std::wstring(pdbPathFromCompiler->GetStringPointer()));
#endif

    // Return the data blob containing the DXIL code.
    return dxil;
}


void d3dUtil::WriteBinaryToFile(IDxcBlob* blob, const std::wstring& filename)
{
    std::ofstream fout(filename, std::ios::binary);
    fout.write((char*)blob->GetBufferPointer(), blob->GetBufferSize());
    fout.close();
}



std::array<const CD3DX12_STATIC_SAMPLER_DESC, 7> d3dUtil::GetStaticSamplers()
{
    // Applications usually only need a handful of samplers.  So just define them all up front
    // and keep them available as part of the root signature.  

    const CD3DX12_STATIC_SAMPLER_DESC pointWrap(
        0, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
        1, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
        2, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
        3, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
        4, // shaderRegister
        D3D12_FILTER_ANISOTROPIC, // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressW
        0.0f,                             // mipLODBias
        8);                               // maxAnisotropy

    const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(
        5, // shaderRegister
        D3D12_FILTER_ANISOTROPIC, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressW
        0.0f,                              // mipLODBias
        8);                                // maxAnisotropy

    const CD3DX12_STATIC_SAMPLER_DESC shadow(
        6,                                                  // s6
        D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,   // comparison filter = hw PCF
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        0.0f, 16,
        D3D12_COMPARISON_FUNC_LESS_EQUAL,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE);            // outside map -> depth 1 -> compare pass -> lit

    return {
        pointWrap, pointClamp,
        linearWrap, linearClamp,
        anisotropicWrap, anisotropicClamp, shadow };
}