// SPDX-License-Identifier: GPL-2.0-or-later
// Compile on Windows with FXC (not DXC), targeting Shader Model 5.0.
float4 vs_main(uint id : SV_VertexID) : SV_Position {
    const float2 positions[3] = {
        float2(-0.8, -0.8), float2(0.8, -0.8), float2(0.0, 0.8)
    };
    return float4(positions[id], 0.0, 1.0);
}

float4 ps_red() : SV_Target0 {
    return float4(1.0, 0.0, 0.0, 1.0);
}

float4 ps_green() : SV_Target0 {
    return float4(0.0, 1.0, 0.0, 1.0);
}
