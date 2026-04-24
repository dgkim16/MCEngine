#pragma once

#include <d3dcommon.h>
// hlsl shader model 5.1
const D3D_SHADER_MACRO defines[] =
{
	"FOG", "1",
	"FORCE_OPAQUE_ALPHA", "1",
	NULL, NULL
};

const D3D_SHADER_MACRO transparentDefines[] =
{
	"FOG", "1",
	NULL, NULL
};

const D3D_SHADER_MACRO alphaTestDefines[] =
{
	"FOG", "1",
	"ALPHA_TEST", "1",
	NULL, NULL
};

const D3D_SHADER_MACRO depthDebugDefines[] =
{
	"DEPTH_MSAA", "1",
	NULL, NULL
};
