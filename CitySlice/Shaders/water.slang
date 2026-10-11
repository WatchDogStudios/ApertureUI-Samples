// water.slang - CitySlice water surface simulation: a damped 2D wave equation
// on three rotating R32F images (previous, current, next height). Impulses
// (cursor, tools, boat wakes) are pushed in as radial bumps; land (terrain at
// or above the water level) clamps the field to zero so ripples die on shore.
// Compiled by slangc into water.comp.spv; dispatched at 16x16 per group.

struct WavePush
{
    float4 impulses[6]; // uv x, uv y, radius (uv), strength
    float4 params;      // damping, c^2, water level, impulse count
};

[[vk::push_constant]]
ConstantBuffer<WavePush> gPush;

[[vk::binding(0, 0)]] RWTexture2D<float> gPrev;
[[vk::binding(1, 0)]] RWTexture2D<float> gCur;
[[vk::binding(2, 0)]] RWTexture2D<float> gNext;
[[vk::binding(3, 0)]] Texture2D<float> gTerrain;
[[vk::binding(4, 0)]] SamplerState gLinear;

[shader("compute")]
[numthreads(16, 16, 1)]
void csWater(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    gCur.GetDimensions(w, h);
    if (id.x >= w || id.y >= h)
        return;
    int2 p = int2(id.xy);
    int2 mx = int2(w - 1, h - 1);
    float c = gCur[p];
    float l = gCur[clamp(p + int2(-1, 0), int2(0, 0), mx)];
    float r = gCur[clamp(p + int2(1, 0), int2(0, 0), mx)];
    float d = gCur[clamp(p + int2(0, -1), int2(0, 0), mx)];
    float u = gCur[clamp(p + int2(0, 1), int2(0, 0), mx)];
    float prev = gPrev[p];
    float lap = l + r + d + u - 4.0 * c;
    float next = (2.0 * c - prev + gPush.params.y * lap) * gPush.params.x;

    float2 uv = (float2(p) + 0.5) / float2(w, h);
    int count = int(gPush.params.w);
    for (int i = 0; i < count; ++i)
    {
        float4 imp = gPush.impulses[i];
        float dist = length(uv - imp.xy) / max(imp.z, 1e-5);
        if (dist < 1.0)
        {
            float k = 1.0 - dist * dist;
            next -= imp.w * k * k;
        }
    }

    // Shoreline: the terrain heightmap covers the same map; damp towards land.
    float terrain = gTerrain.SampleLevel(gLinear, uv * (96.0 / 97.0) + 0.5 / 97.0, 0); // corner-centred texels
    float land = smoothstep(-0.6, 0.0, terrain - gPush.params.z);
    next *= 1.0 - land;
    gNext[p] = clamp(next, -1.5, 1.5);
}
