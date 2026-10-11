// post.slang - CitySlice post pass: fullscreen triangle over the HDR scene.
// Screen-space reflections for glossy dielectrics (water, glass), then ACES
// tonemapping into the UNORM swapchain. Compiled by slangc into post.vert.spv
// and post.frag.spv.

// Mirrors CityRenderer::PostUniforms (std140).
struct Post
{
    float4 ivp0;   // inverse view-projection columns
    float4 ivp1;
    float4 ivp2;
    float4 ivp3;
    float4 vp0;    // view-projection columns
    float4 vp1;
    float4 vp2;
    float4 vp3;
    float4 cam;    // xyz eye, w time
    float4 fwd;    // xyz camera forward, w night
    float4 sun;    // xyz direction to the sun, w exposure
    float4 sunColor;
    float4 fog;    // xyz horizon, w proj m10
    float4 zenith; // xyz zenith, w proj m14
    float4 size;   // width, height, 1/width, 1/height
};

[[vk::binding(0, 0)]] ConstantBuffer<Post> gPost;
[[vk::binding(1, 0)]] Texture2D<float4> gHdr;
[[vk::binding(2, 0)]] Texture2D<float4> gAux;
[[vk::binding(3, 0)]] Texture2D<float> gDepth;
[[vk::binding(4, 0)]] SamplerState gLinear;

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

[shader("vertex")]
VSOut vsPost(uint id : SV_VertexID)
{
    VSOut o;
    // One triangle covering the screen: (0,0) (2,0) (0,2) in uv space.
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

float3 worldFromDepth(float2 uv, float depth)
{
    float4 ndc = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 w = gPost.ivp0 * ndc.x + gPost.ivp1 * ndc.y + gPost.ivp2 * ndc.z + gPost.ivp3;
    return w.xyz / w.w;
}

// Projects a world point: xy = uv, z = depth-buffer value, w = view depth.
float4 project(float3 p)
{
    float4 c = gPost.vp0 * p.x + gPost.vp1 * p.y + gPost.vp2 * p.z + gPost.vp3;
    float3 ndc = c.xyz / max(c.w, 1e-4);
    return float4(ndc.xy * 0.5 + 0.5, ndc.z, c.w);
}

float viewDepth(float depth)
{
    // Perspective (CityMath): ndc_z = -m10 + m14 / d  ->  d = m14 / (ndc_z + m10)
    return gPost.zenith.w / (depth + gPost.fog.w);
}

float3 skyReflection(float3 r)
{
    return lerp(gPost.fog.xyz, gPost.zenith.xyz, saturate(r.y * 1.4 + 0.1));
}

float3 tonemap(float3 c)
{
    c *= gPost.sun.w;
    c = (c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14);
    c = lerp(dot(c, float3(0.299, 0.587, 0.114)).xxx, c, 1.22);
    return pow(saturate(c), float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
}

float hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

[shader("fragment")]
float4 psPost(VSOut input) : SV_Target
{
    float2 uv = input.uv;
    float3 color = gHdr.Sample(gLinear, uv).rgb;
    float4 aux = gAux.Sample(gLinear, uv);
    float depth = gDepth.Sample(gLinear, uv);
    float rough = aux.w;
    int debugView = int(gPost.sunColor.w);
    if (debugView == 1) return float4(aux.xyz, 1.0);
    if (debugView == 2) return float4(rough.xxx, 1.0);
    if (debugView == 3) return float4(saturate((viewDepth(depth) / 400.0)).xxx, 1.0);
    if (debugView == 4) return float4(tonemap(color), 1.0);

    if (rough >= 0.0 && depth < 1.0)
    {
        // Screen-space reflection: march the reflected ray through the depth
        // buffer in world space with growing steps, then refine the hit.
        float3 P = worldFromDepth(uv, depth);
        float3 N = normalize(aux.xyz * 2.0 - 1.0);
        float3 V = normalize(gPost.cam.xyz - P);
        float3 R = reflect(-V, N);
        float dist = length(gPost.cam.xyz - P);
        float jitter = hash12(uv * gPost.size.xy + gPost.cam.w * 7.0);
        float stepLen = max(0.35, dist * 0.018);
        float3 pos = P + N * 0.06 + R * stepLen * (0.3 + 0.7 * jitter);
        float3 hitColor = 0.0;
        float hit = 0.0;
        const int STEPS = 28;
        float3 prev = pos;
        for (int i = 0; i < STEPS; ++i)
        {
            float4 s = project(pos);
            if (any(s.xy < 0.0) || any(s.xy > 1.0) || s.w < 0.1)
                break;
            float sceneDepth = gDepth.SampleLevel(gLinear, s.xy, 0);
            float sceneZ = viewDepth(sceneDepth);
            float thickness = stepLen * 2.2 + 0.4;
            if (s.w > sceneZ + 0.02 && s.w - sceneZ < thickness && sceneDepth < 1.0)
            {
                // binary refine between prev and pos
                float3 a = prev, b = pos;
                for (int k = 0; k < 4; ++k)
                {
                    float3 m = (a + b) * 0.5;
                    float4 sm = project(m);
                    float mz = viewDepth(gDepth.SampleLevel(gLinear, sm.xy, 0));
                    if (sm.w > mz) b = m; else a = m;
                }
                float4 sh = project(b);
                float2 edge = abs(sh.xy * 2.0 - 1.0);
                float edgeFade = 1.0 - smoothstep(0.75, 1.0, max(edge.x, edge.y));
                float3 hitN = normalize(gAux.SampleLevel(gLinear, sh.xy, 0).xyz * 2.0 - 1.0);
                float facing = saturate(dot(hitN, -R) * 4.0 + 0.5); // no back-face hits
                hitColor = gHdr.SampleLevel(gLinear, sh.xy, 0).rgb;
                hit = edgeFade * facing;
                break;
            }
            prev = pos;
            pos += R * stepLen;
            stepLen *= 1.16;
        }
        if (hit > 0.0)
        {
            // Swap the sky reflection the scene pass baked in for what the ray hit.
            float3 f0 = float3(0.03, 0.03, 0.03);
            float ndv = max(dot(N, V), 1e-4);
            float3 F = f0 + (max(float3(1.0 - rough, 1.0 - rough, 1.0 - rough), f0) - f0) * pow(1.0 - ndv, 5.0);
            float3 sky = skyReflection(R) * lerp(1.0, 0.35, gPost.fwd.w);
            float weight = (1.0 - rough * 0.85) * hit * (0.7 + 0.3);
            color += F * (hitColor - sky) * weight;
        }
    }

    return float4(tonemap(color), 1.0);
}
