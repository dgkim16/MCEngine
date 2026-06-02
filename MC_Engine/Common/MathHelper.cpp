//***************************************************************************************
// built on top of 
// MathHelper.cpp by Frank Luna (C) 2011 All Rights Reserved.
//***************************************************************************************

#include "MathHelper.h"
#include <float.h>
#include <cmath>

using namespace DirectX;

const float MathHelper::Infinity = FLT_MAX;
const float MathHelper::Pi = 3.1415926535f;

std::string MathHelper::to_scientific_string(float x)
{
    if (x == 0.0f)
        return "0.000 * 10^0";

    int exponent = static_cast<int>(std::floor(std::log10(std::fabs(x))));
    float mantissa = x / static_cast<float>(std::pow(10.0f, exponent));

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(3) << mantissa << " * 10^" << exponent;
    return ss.str();
}

XMMATRIX MathHelper::InverseTranspose(DirectX::CXMMATRIX M) {
    XMMATRIX A = M;
    A.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    XMVECTOR det = XMMatrixDeterminant(A);
    return XMMatrixTranspose(XMMatrixInverse(&det, A));
}

XMMATRIX MathHelper::LerpMatrix(XMMATRIX& A, XMMATRIX& B, float& t)
{
    return XMMATRIX(
        XMVectorLerp(A.r[0], B.r[0], t),
        XMVectorLerp(A.r[1], B.r[1], t),
        XMVectorLerp(A.r[2], B.r[2], t),
        XMVectorLerp(A.r[3], B.r[3], t)
    );
}

XMMATRIX MathHelper::ComposeWorldMatrix(const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT4& rot, const DirectX::XMFLOAT3& scale)
{
    XMVECTOR S = XMLoadFloat3(&scale);
    XMVECTOR R = XMQuaternionNormalize(XMLoadFloat4(&rot));  // renormalize defensively
    XMVECTOR T = XMLoadFloat3(&pos);

    return XMMatrixScalingFromVector(S)
        * XMMatrixRotationQuaternion(R)
        * XMMatrixTranslationFromVector(T);
}

/*
* M = XMMatrixRotationQuaternion(XMVECTOR quat)
* Pitch (\(\theta \)) = \(\arcsin(-M_{32})\)
* Yaw (\(\psi \)) = \(\arctan2(M_{31}, M_{33})\)
* Roll (\(\phi \)) = \(\arctan2(M_{12}, M_{22})\)
*/
XMFLOAT3 MathHelper::QuatToEulerDegrees(const DirectX::XMFLOAT4& Q) {
    XMVECTOR R = XMQuaternionNormalize(XMLoadFloat4(&Q));
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, XMMatrixRotationQuaternion(R));
    XMFLOAT3 euler { 
        XMConvertToDegrees(static_cast<float>(std::asin(-1.0 * m._32))), 
        XMConvertToDegrees(static_cast<float>(std::atan2(m._31, m._33))),
        XMConvertToDegrees(static_cast<float>(std::atan2(m._12,m._22)))
    };
    return euler;
}

XMFLOAT4 MathHelper::EulerDegreesToQuat(const DirectX::XMFLOAT3& E) {
    XMVECTOR Q = XMQuaternionRotationRollPitchYaw(
        XMConvertToRadians(E.x),   // pitch (X)
        XMConvertToRadians(E.y),   // yaw   (Y)
        XMConvertToRadians(E.z));  // roll  (Z)
    XMFLOAT4 out;
    XMStoreFloat4(&out, Q);
    return out;
}