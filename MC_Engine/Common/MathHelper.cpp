//***************************************************************************************
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
    float mantissa = x / std::pow(10.0f, exponent);

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