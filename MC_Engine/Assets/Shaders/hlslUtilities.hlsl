// HLSL has no built-in noise(). Layered sines give smooth spatial variation in [-1, 1].
float noise(float x)
{
    return sin(x * 3.7f) * 0.45f
         + sin(x * 8.1f) * 0.35f
         + sin(x * 15.3f) * 0.2f;
}

float3 CameraUp(float3 camPos, float3 targetPos)
{
    // assumes world Up = (0,1,0) and no tilting
    float3 fwd = normalize(targetPos - camPos);
    // float3 rt = float3(fwd.z, 0.0f, -fwd.x);
    float3 up = float3(-fwd.x * fwd.y, pow(fwd.x, 2) - pow(fwd.z, 2), -fwd.y * fwd.z);
    return up;
}

float2 Fade(float2 t)
{
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

float2 Hash2(float2 p)
{
    p = float2(dot(p, float2(127.1, 311.7)),
               dot(p, float2(269.5, 183.3)));

    return -1.0 + 2.0 * frac(sin(p) * 43758.5453123);
}

float PerlinNoise2D(float x, float y)
{
    float2 p = float2(x, y);

    float2 i = floor(p);
    float2 f = frac(p);

    float2 u = Fade(f);

    float2 g00 = normalize(Hash2(i + float2(0.0, 0.0)));
    float2 g10 = normalize(Hash2(i + float2(1.0, 0.0)));
    float2 g01 = normalize(Hash2(i + float2(0.0, 1.0)));
    float2 g11 = normalize(Hash2(i + float2(1.0, 1.0)));

    float n00 = dot(g00, f - float2(0.0, 0.0));
    float n10 = dot(g10, f - float2(1.0, 0.0));
    float n01 = dot(g01, f - float2(0.0, 1.0));
    float n11 = dot(g11, f - float2(1.0, 1.0));

    float nx0 = lerp(n00, n10, u.x);
    float nx1 = lerp(n01, n11, u.x);

    float nxy = lerp(nx0, nx1, u.y);

    return nxy;
}

// avoid this - very expensive
float3 RotY(float3 p, float rot) // radians
{
    // rotates p around the y-axis
    float x = p.x * cos(rot) + p.z * sin(rot);
    float z = -p.x * sin(rot) + p.z * cos(rot);
    return float3(x, p.y, z);
}

// avoid this - very expensive
float4x4 MakeWorldMatrix(float uniformScale, float rotationY, float3 translation)
{
    float s = uniformScale;
    float c = cos(rotationY);
    float si = sin(rotationY);

    float4x4 S =
    {
        s, 0, 0, 0,
        0, s, 0, 0,
        0, 0, s, 0,
        0, 0, 0, 1
    };

    float4x4 R =
    {
        c, 0, si, 0,
         0, 1, 0, 0,
        -si, 0, c, 0,
         0, 0, 0, 1
    };

    float4x4 T =
    {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        translation.x, translation.y, translation.z, 1
    };

    return mul(mul(S, R), T);
}

float Hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return (float) x / 4294967296.0;
}