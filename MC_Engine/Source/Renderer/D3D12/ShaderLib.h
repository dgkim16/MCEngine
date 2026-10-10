#pragma once

#include <windows.h>
#include <wrl.h>
#include <DirectXMath.h>
#include <d3d12.h>
#include "Renderer/D3D12/ThirdParty/d3dx12.h"
// some shaders use version 6.6
#include "dxc/inc/dxcapi.h"
#include "dxc/inc/d3d12shader.h"
#include <unordered_map>
#include <string>

// hlsl version 6.6 (DX required for compiling)
class ShaderLibDx
{
public:
	ShaderLibDx(const ShaderLibDx& rhs) = delete;
	ShaderLibDx& operator=(const ShaderLibDx& rhs) = delete;
	static ShaderLibDx& GetLib() {
		static ShaderLibDx singleton;
		return singleton;
	}
	bool IsInitialized() const;
	void Init();
	void Reset() { mShaders.clear(); mIsInitialized = false; }
	bool AddShader(const std::string& name, Microsoft::WRL::ComPtr<IDxcBlob> shader);
	// IDxcBlob* operator[](const std::string& name); // returns element from a protected array
	IDxcBlob* operator[](const std::string& name); // returns element from a protected array

private:
	ShaderLibDx() = default;

protected:
	bool mIsInitialized = false;
	std::unordered_map<std::string, Microsoft::WRL::ComPtr<IDxcBlob>> mShaders;
};
