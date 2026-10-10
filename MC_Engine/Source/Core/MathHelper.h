#pragma once
#include <cstdlib>
#include <DirectXMath.h>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <string>

class MathHelper
{
public:
	template<typename T>
	static T Clamp(const T& x, const T& low, const T& high)
	{
		return x < low ? low : (x > high ? high : x);
	}

	static DirectX::XMFLOAT4X4 Identity4x4() {
		static DirectX::XMFLOAT4X4 I(
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f);
		return I;
	}
	// [0,1]
	static float RandF() { 
		return (float)(rand()) / (float)RAND_MAX; }
	// [-1,1]
	// float v = 2.0f * ((float)rand() / (float)RAND_MAX) - 1.0f;

	// Returns random float in [a, b).
	static float RandF(float a, float b){
		return a + RandF() * (b - a);}
	static int Rand(int a, int b){
		return a + rand() % ((b - a) + 1);}
	static float EaseOut(float t) { 
		return 1.0f - (1.0f - t) * (1.0f - t); }
	static DirectX::XMMATRIX InverseTranspose(DirectX::CXMMATRIX M);
	static DirectX::XMMATRIX LerpMatrix(DirectX::XMMATRIX& A, DirectX::XMMATRIX& B, float& t);
	static DirectX::XMMATRIX ComposeWorldMatrix(const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT4& rot, const DirectX::XMFLOAT3& scale);
	static DirectX::XMFLOAT3 QuatToEulerDegrees(const DirectX::XMFLOAT4& Q);
	static DirectX::XMFLOAT4 EulerDegreesToQuat(const DirectX::XMFLOAT3& E);
	static const float Infinity;
	static const float Pi;

	static std::string to_scientific_string(float x);
};
