// 标准 HLSL 内建可选头 <canton.hlsl>：构建期与运行期编译器共享此定义，低位槽 b0..b11/t0..t11/s0..s11 留给用户着色器
export const CANTON_HLSL_HEADER = `#ifndef CANTON_HLSL_INCLUDED
#define CANTON_HLSL_INCLUDED

struct CantonInput {
    float4 position : SV_Position;
    float2 fragCoord : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

cbuffer Canton : register(b12) {
    float2 cantonResolution;
    float2 cantonInvResolution;
};

SamplerState cantonLinearClamp  : register(s12);
SamplerState cantonLinearWrap   : register(s13);
SamplerState cantonLinearMirror : register(s14);
SamplerState cantonLinearDecal  : register(s15);
SamplerState cantonPointClamp   : register(s11);

#endif
`
