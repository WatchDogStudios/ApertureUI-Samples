// city.slang - CitySlice world shader: HDR scene pass (sky, terrain, water,
// roads, buildings, vehicles) with PBR lighting, the sun shadow map, cloud
// shadows, tile-gridded spotlights and subsurface scattering for foliage and
// water. Outputs linear radiance + a normal/roughness buffer for the post pass.
//
// Compiled by slangc at build time into city.vert.spv / city.frag.spv.
// Push constants (exactly 128 bytes, the portable minimum): the view-projection
// as four explicit columns plus packed lighting/time state. Set 0 carries the
// per-frame `Scene` uniforms, the shadow map, the light grid, the wave and
// terrain heightmaps. Materials travel in the vertex colour's alpha byte (see
// CityRenderer.h).

struct Params
{
    float4 col0;
    float4 col1;
    float4 col2;
    float4 col3;
    float4 sunDir;   // xyz = direction TOWARDS the sun; w = nightness 0..1
    float4 camPos;   // xyz = eye position; w = time (seconds, waves/pulses)
    float4 fog;      // xyz = horizon/fog colour; w = fog end distance
    float4 sunColor; // xyz = sun tint; w = ambient scale
};

[[vk::push_constant]]
ConstantBuffer<Params> gParams;

// Mirrors CityRenderer::SceneUniforms.
struct Scene
{
    float4 lc0;      // sun view-projection columns
    float4 lc1;
    float4 lc2;
    float4 lc3;
    float4 shadow;   // x strength, y texel size, z normal offset (m)
    float4 water;    // x level, y 1/map extent, z wave amplitude, w wave texel
    float4 zenith;   // xyz sky zenith colour, w lights on 0..1
    float4 terrain;  // x 1/(N+1), y 0.5/(N+1), z cell size, w tiles per side
};

// Mirrors CityRenderer::Light (std430, 48 bytes).
struct Light
{
    float3 pos;
    float range;
    float3 dir;
    float cosOuter;
    float3 color;    // premultiplied by intensity
    float cosInner;
};

[[vk::binding(0, 0)]] ConstantBuffer<Scene> gScene;
[[vk::binding(1, 0)]] Texture2D<float> gShadowMap;
[[vk::binding(2, 0)]] SamplerComparisonState gShadowSampler;
[[vk::binding(3, 0)]] StructuredBuffer<Light> gLights;
[[vk::binding(4, 0)]] StructuredBuffer<uint> gLightGrid;   // per tile: offset<<8 | count
[[vk::binding(5, 0)]] StructuredBuffer<uint> gLightRefs;
[[vk::binding(6, 0)]] Texture2D<float> gWave;              // simulated water height (m/amplitude)
[[vk::binding(7, 0)]] Texture2D<float> gTerrain;           // corner heights (m)
[[vk::binding(8, 0)]] SamplerState gLinear;

struct VSInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal   : NORMAL;
    [[vk::location(2)]] float4 color    : COLOR0;
};

struct VSOutput
{
    float4 clipPos  : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float4 color    : COLOR0;
};

struct PSOutput
{
    float4 color : SV_Target0; // linear HDR radiance
    float4 aux   : SV_Target1; // xyz normal * 0.5 + 0.5, w roughness (< 0: no env/SSR in post)
};

// Material ids carried in the vertex alpha (0..255 scaled to 0..1 by the attribute).
static const float MAT_PLAIN    = 255.0;
static const float MAT_UNLIT    = 250.0;
static const float MAT_WATER    = 240.0;
static const float MAT_GRASS    = 230.0;
static const float MAT_FACADE   = 220.0;
static const float MAT_ROOF     = 210.0;
static const float MAT_ROAD     = 200.0;
static const float MAT_EMISSIVE = 190.0;
static const float MAT_FOLIAGE  = 180.0;
static const float MAT_BRICK    = 170.0;
static const float MAT_PLASTER  = 165.0;
static const float MAT_WINDOW   = 160.0;
static const float MAT_METAL    = 150.0;
static const float MAT_SKY      = 140.0;
static const float MAT_CONCRETE = 130.0;
static const float MAT_PAINT    = 120.0;
static const float MAT_OVERLAY  = 110.0;

static const float SUN_INTENSITY = 3.2;
static const float EMISSIVE_INTENSITY = 3.5;

float2 waveUV(float3 p) { return p.xz * gScene.water.y; }
float terrainHeight(float3 p)
{
    float2 uv = (p.xz / gScene.terrain.z + 0.5) * gScene.terrain.x;
    return gTerrain.SampleLevel(gLinear, uv, 0);
}

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    VSOutput o;
    float3 p = input.position;
    float mat = round(input.color.a * 255.0);
    if (mat == MAT_FOLIAGE)
    {
        // Foliage sways a little in the wind (canopies only: trunks are MAT_PLAIN).
        float t = gParams.camPos.w;
        float ph = p.x * 0.17 + p.z * 0.13;
        p.x += sin(t * 1.1 + ph) * 0.09 + sin(t * 2.3 + ph * 1.7) * 0.03;
        p.z += cos(t * 0.9 + ph) * 0.07;
    }
    else if (mat == MAT_WATER && abs(input.normal.y) > 0.99 && abs(p.y - gScene.water.x) < 0.01)
    {
        // The lake sheet rides the simulated wave field.
        float h = gWave.SampleLevel(gLinear, waveUV(p), 0);
        p.y += h * gScene.water.z;
    }
    o.clipPos = gParams.col0 * p.x + gParams.col1 * p.y + gParams.col2 * p.z + gParams.col3;
    o.worldPos = p;
    o.normal = input.normal;
    o.color = input.color;
    return o;
}

// ---------------------------------------------------------------------------
// Noise
// ---------------------------------------------------------------------------

float hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float valueNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i), b = hash21(i + float2(1, 0));
    float c = hash21(i + float2(0, 1)), d = hash21(i + float2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float hash31(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}

float noise3(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash31(i), n100 = hash31(i + float3(1, 0, 0));
    float n010 = hash31(i + float3(0, 1, 0)), n110 = hash31(i + float3(1, 1, 0));
    float n001 = hash31(i + float3(0, 0, 1)), n101 = hash31(i + float3(1, 0, 1));
    float n011 = hash31(i + float3(0, 1, 1)), n111 = hash31(i + float3(1, 1, 1));
    float a = lerp(lerp(n000, n100, f.x), lerp(n010, n110, f.x), f.y);
    float b = lerp(lerp(n001, n101, f.x), lerp(n011, n111, f.x), f.y);
    return lerp(a, b, f.z);
}

// ---------------------------------------------------------------------------
// Clouds
// ---------------------------------------------------------------------------

static const float CLOUD_BASE = 430.0;
static const float CLOUD_TOP  = 640.0;

float cloudDensity(float3 p, float time, int octaves)
{
    float3 q = p * 0.0021 + float3(time * 0.012, 0.0, time * 0.006);
    float n = noise3(q) * 0.55;
    if (octaves > 1) n += noise3(q * 2.3 + 7.1) * 0.27;
    if (octaves > 2) n += noise3(q * 5.1 + 3.3) * 0.13;
    if (octaves > 3) n += noise3(q * 11.0 + 1.7) * 0.06;
    float coverage = 0.52 + 0.1 * noise3(p * 0.00035 + time * 0.002);
    float h = saturate((p.y - CLOUD_BASE) / (CLOUD_TOP - CLOUD_BASE));
    float profile = smoothstep(0.0, 0.25, h) * (1.0 - smoothstep(0.55, 1.0, h));
    return saturate((n - coverage) * 4.0) * profile;
}

float4 marchClouds(float3 ro, float3 rd, float3 sun, float3 sunColor, float3 skyColor, float night, float time)
{
    if (rd.y < 0.015)
        return float4(0, 0, 0, 1);
    float t0 = (CLOUD_BASE - ro.y) / rd.y;
    float t1 = (CLOUD_TOP - ro.y) / rd.y;
    if (t1 < 0.0)
        return float4(0, 0, 0, 1);
    t0 = max(t0, 0.0);
    const int STEPS = 22;
    float stepLen = (t1 - t0) / STEPS;
    float T = 1.0;
    float3 acc = 0.0;
    float sigma = 0.028;
    float3 ambient = lerp(skyColor * 0.9 + 0.15, skyColor * 0.35, night);
    float3 sunLit = sunColor * (1.35 - night * 0.9);
    float t = t0 + stepLen * hash21(rd.xz * 917.0 + rd.y * 13.0) * 0.8;
    for (int i = 0; i < STEPS; ++i)
    {
        float3 p = ro + rd * t;
        float d = cloudDensity(p, time, 4);
        if (d > 0.002)
        {
            float dl = cloudDensity(p + sun * 70.0, time, 2);
            float shadow = exp(-dl * 70.0 * sigma * 1.6);
            float powder = 1.0 - exp(-d * 30.0);
            float3 light = sunLit * shadow * (0.6 + 0.4 * powder) + ambient * 0.55;
            float a = 1.0 - exp(-d * stepLen * sigma);
            acc += light * a * T;
            T *= 1.0 - a;
            if (T < 0.02)
                break;
        }
        t += stepLen;
    }
    float fade = smoothstep(0.015, 0.14, rd.y);
    return float4(acc * fade, lerp(1.0, T, fade));
}

// ---------------------------------------------------------------------------
// Shadows
// ---------------------------------------------------------------------------

float sunShadow(float3 wp, float3 n, float3 sun)
{
    float strength = gScene.shadow.x;
    if (strength <= 0.0)
        return 1.0;
    float ndl = saturate(dot(n, sun));
    float3 p = wp + n * (gScene.shadow.z * (1.2 - ndl));
    float4 lp = gScene.lc0 * p.x + gScene.lc1 * p.y + gScene.lc2 * p.z + gScene.lc3;
    float2 uv = lp.xy * 0.5 + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0) || lp.z > 1.0)
        return 1.0;
    float ref = lp.z - 0.0006;
    float t = gScene.shadow.y;
    float s = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
            s += gShadowMap.SampleCmpLevelZero(gShadowSampler, uv + float2(x, y) * t, ref);
    }
    return lerp(1.0, s / 9.0, strength);
}

float cloudShadow(float3 wp, float3 sun, float time)
{
    if (sun.y < 0.1)
        return 1.0;
    float3 p = wp + sun * ((CLOUD_BASE + 70.0 - wp.y) / sun.y);
    return 1.0 - 0.5 * saturate(cloudDensity(p, time, 2) * 2.0);
}

// ---------------------------------------------------------------------------
// PBR
// ---------------------------------------------------------------------------

float D_GGX(float ndh, float a2)
{
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265 * d * d + 1e-6);
}

float G_Smith(float ndv, float ndl, float rough)
{
    float k = (rough + 1.0) * (rough + 1.0) * 0.125;
    float gv = ndv / (ndv * (1.0 - k) + k);
    float gl = ndl / (ndl * (1.0 - k) + k);
    return gv * gl;
}

float3 F_Schlick(float3 f0, float vdh)
{
    return f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);
}

// Cook-Torrance BRDF times NdotL: what one light contributes per unit radiance.
float3 brdf(float3 n, float3 v, float3 l, float3 albedo, float rough, float metal, float3 f0, float wrap)
{
    float3 h = normalize(v + l);
    float ndl = saturate((dot(n, l) + wrap) / (1.0 + wrap));
    float ndv = max(dot(n, v), 1e-4);
    float ndh = saturate(dot(n, h));
    float vdh = saturate(dot(v, h));
    float a = max(rough * rough, 0.002);
    float D = D_GGX(ndh, a * a);
    float G = G_Smith(ndv, ndl, rough);
    float3 F = F_Schlick(f0, vdh);
    float3 spec = D * G * F / (4.0 * ndv * max(ndl, 1e-4) + 1e-4);
    float3 diffuse = (1.0 - F) * (1.0 - metal) * albedo / 3.14159265;
    return (diffuse + spec) * ndl;
}

float3 skyReflection(float3 r, float3 horizon, float3 zenith)
{
    return lerp(horizon, zenith, saturate(r.y * 1.4 + 0.1));
}

// Per-material roughness / metalness.
void materialParams(float mat, out float rough, out float metal)
{
    rough = 0.8;
    metal = 0.0;
    if (mat == MAT_WATER) rough = 0.06;
    else if (mat == MAT_GRASS) rough = 0.95;
    else if (mat == MAT_FACADE) rough = 0.18;
    else if (mat == MAT_ROOF) rough = 0.85;
    else if (mat == MAT_ROAD) rough = 0.72;
    else if (mat == MAT_FOLIAGE) rough = 0.75;
    else if (mat == MAT_BRICK) rough = 0.9;
    else if (mat == MAT_PLASTER) rough = 0.82;
    else if (mat == MAT_WINDOW) rough = 0.08;
    else if (mat == MAT_METAL) { rough = 0.42; metal = 0.85; }
    else if (mat == MAT_CONCRETE) rough = 0.78;
    else if (mat == MAT_PAINT) { rough = 0.22; metal = 0.12; }
}

// ---------------------------------------------------------------------------
// Fragment
// ---------------------------------------------------------------------------

[shader("fragment")]
PSOutput psMain(VSOutput input)
{
    PSOutput o;
    float mat = round(input.color.a * 255.0);
    float3 base = input.color.rgb;
    float time = gParams.camPos.w;
    float night = gParams.sunDir.w;
    float3 toCam = gParams.camPos.xyz - input.worldPos;
    float dist = length(toCam);
    float3 v = toCam / max(dist, 0.001);
    float3 sun = normalize(gParams.sunDir.xyz);
    o.aux = float4(0.5, 1.0, 0.5, -1.0);

    // Tool overlays: colour is final (HDR-ish so it stays readable after tonemap).
    if (mat == MAT_UNLIT)
    {
        o.color = float4(base * 1.6, 1.0);
        return o;
    }
    // Coverage overlay: hatched, see-through, unlit.
    if (mat == MAT_OVERLAY)
    {
        float2 wp2 = input.worldPos.xz;
        if (frac((wp2.x + wp2.y) * 0.45 + time * 0.15) > 0.55)
            discard;
        o.color = float4(base * 1.4, 1.0);
        return o;
    }
    // Sky dome: the vertex gradient, a sun/moon disc and the raymarched clouds.
    if (mat == MAT_SKY)
    {
        float3 rd = normalize(input.worldPos - gParams.camPos.xyz);
        float4 cl = marchClouds(gParams.camPos.xyz, rd, sun, gParams.sunColor.xyz, gParams.fog.xyz, night, time);
        float sd = dot(rd, sun);
        float3 disc = gParams.sunColor.xyz * (smoothstep(0.9993, 0.9997, sd) * 6.0 + pow(saturate(sd), 48.0) * 0.25) * (1.0 - night);
        disc += float3(0.75, 0.8, 0.95) * smoothstep(0.99955, 0.99975, sd) * 1.6 * night;
        float3 col = (base + disc) * cl.a + cl.rgb;
        o.color = float4(col, 1.0);
        return o;
    }

    float3 n = normalize(input.normal);
    float3 geoN = n;
    float2 wp = input.worldPos.xz;
    float3 emissive = 0.0;
    float rough, metal;
    materialParams(mat, rough, metal);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), base, metal);
    float wrap = 0.0;
    float3 sss = 0.0;        // subsurface term added after direct lighting
    float3 waterExtra = 0.0; // foam / scattering on the lake
    float shadowScale = 1.0;

    if (mat == MAT_WATER)
    {
        // Normal from the simulated wave field plus fine procedural ripples.
        float2 uv = waveUV(input.worldPos);
        float t = gScene.water.w;
        float texelWorld = t / gScene.water.y;
        float hL = gWave.Sample(gLinear, uv - float2(t, 0)), hR = gWave.Sample(gLinear, uv + float2(t, 0));
        float hD = gWave.Sample(gLinear, uv - float2(0, t)), hU = gWave.Sample(gLinear, uv + float2(0, t));
        float h = gWave.Sample(gLinear, uv);
        float amp = gScene.water.z;
        float w1 = valueNoise(wp * 0.35 + float2(time * 0.35, time * 0.21));
        float w2 = valueNoise(wp * 0.9 - float2(time * 0.27, -time * 0.4));
        float2 grad = float2(w1 - valueNoise(wp * 0.35 + float2(0.6, 0) + float2(time * 0.35, time * 0.21)),
                             w2 - valueNoise(wp * 0.9 + float2(0, 0.35) - float2(time * 0.27, -time * 0.4)));
        float3 simN = float3(-(hR - hL) * amp / (2.0 * texelWorld), 1.0, -(hU - hD) * amp / (2.0 * texelWorld));
        n = normalize(simN + float3(grad.x * 0.35, 0.0, grad.y * 0.35));
        // Depth below the surface from the terrain heightmap: absorption makes
        // deep water dark blue, shallows turquoise, and scattering lights the
        // wave bodies from behind (the water's subsurface term).
        // The lakes are geometrically shallow; scale the depth so they read deep.
        float depth = max(0.0, gScene.water.x - terrainHeight(input.worldPos)) * 5.0;
        float3 absorb = exp(-depth * float3(0.55, 0.22, 0.11));
        float3 deep = float3(0.012, 0.045, 0.09);
        float3 shallow = float3(0.08, 0.30, 0.30);
        base = lerp(deep, shallow, absorb);
        f0 = float3(0.02, 0.02, 0.02);
        float back = pow(saturate(dot(v, -sun)), 3.0);
        float crest = saturate(h * amp * 2.5 + 0.2);
        sss = float3(0.05, 0.28, 0.24) * (back * 0.8 + 0.25) * crest * saturate(1.5 - depth * 0.35) * (1.0 - night * 0.8);
        float foamNoise = valueNoise(wp * 1.7 + time * 0.2) * 0.6 + valueNoise(wp * 4.0 - time * 0.3) * 0.4;
        float shoreFoam = smoothstep(0.7, 0.05, depth) * smoothstep(0.5, 0.8, foamNoise + sin(time * 1.5 + depth * 6.0) * 0.12);
        float crestFoam = smoothstep(0.2, 0.36, abs(h * amp)) * smoothstep(0.5, 0.75, foamNoise);
        waterExtra = float3(0.75, 0.8, 0.82) * saturate(shoreFoam + crestFoam) * 0.6;
        rough = lerp(0.06, 0.5, saturate(shoreFoam + crestFoam));
    }
    else if (mat == MAT_GRASS)
    {
        float fine = valueNoise(wp * 1.6) * 0.5 + valueNoise(wp * 5.0) * 0.5;
        float broad = valueNoise(wp * 0.08);
        base *= 0.86 + fine * 0.22;
        base = lerp(base, base * float3(1.06, 0.98, 0.82), broad * 0.5);
        wrap = 0.15;
    }
    else if (mat == MAT_FACADE)
    {
        // Procedural window grid on vertical faces: lit at night, glassy by day.
        float2 uv = abs(n.x) > 0.5 ? float2(input.worldPos.z, input.worldPos.y) : float2(input.worldPos.x, input.worldPos.y);
        float2 cell = frac(uv * float2(0.55, 0.42));
        float win = step(0.2, cell.x) * step(cell.x, 0.8) * step(0.3, cell.y) * step(cell.y, 0.85);
        float2 id = floor(uv * float2(0.55, 0.42));
        float lit = step(0.35, hash21(id)) * night;
        float3 glass = lerp(base * 0.55, float3(0.5, 0.62, 0.74), 0.35);
        base = lerp(base * 0.9, glass, win * 0.8);
        emissive = win * lit * float3(1.0, 0.85, 0.55) * 0.9;
        rough = lerp(0.6, 0.12, win);
    }
    else if (mat == MAT_ROAD)
    {
        float grain = valueNoise(wp * 6.0);
        base *= 0.92 + grain * 0.16;
        rough = 0.72 - night * 0.25; // evening asphalt reads a little glossier under the lamps
    }
    else if (mat == MAT_ROOF)
    {
        float sloped = step(0.15, n.y) * step(n.y, 0.92);
        float course = frac(input.worldPos.y * 2.4);
        base *= lerp(1.0, 0.82 + 0.18 * step(0.18, course), sloped);
        base *= 0.95 + valueNoise(wp * 3.0) * 0.1;
    }
    else if (mat == MAT_EMISSIVE)
    {
        emissive = base * EMISSIVE_INTENSITY;
        base *= 0.25;
    }
    else if (mat == MAT_FOLIAGE)
    {
        float nz = valueNoise(wp * 2.6 + input.worldPos.y * 1.9);
        base *= 0.78 + nz * 0.42;
        wrap = 0.3;
        // Leaf translucency (Barre-Brisebois): light bleeding through the
        // canopy towards the viewer, strongest when looking into the sun.
        float3 hL = normalize(sun + n * 0.35);
        float trans = pow(saturate(dot(v, -hL)), 3.0) * 0.85 + 0.12;
        sss = base * float3(0.95, 1.15, 0.45) * trans * 0.9;
    }
    else if (mat == MAT_BRICK)
    {
        float2 uv = abs(n.x) > 0.5 ? float2(input.worldPos.z, input.worldPos.y) : float2(input.worldPos.x, input.worldPos.y);
        float row = floor(uv.y * 3.6);
        float2 b = frac(float2(uv.x * 1.8 + fmod(row, 2.0) * 0.5, uv.y * 3.6));
        float mortar = saturate(step(b.x, 0.07) + step(b.y, 0.14));
        base *= 0.9 + valueNoise(uv * 9.0) * 0.2;
        base = lerp(base, float3(0.72, 0.68, 0.62), mortar * 0.55);
        rough = lerp(0.85, 0.98, mortar);
    }
    else if (mat == MAT_PLASTER)
    {
        float2 uv = abs(n.x) > 0.5 ? float2(input.worldPos.z, input.worldPos.y) : float2(input.worldPos.x, input.worldPos.y);
        base *= 0.93 + valueNoise(uv * 3.5) * 0.12 + valueNoise(uv * 14.0) * 0.04;
        base *= lerp(0.86, 1.0, saturate(input.worldPos.y * 0.15 - 0.1));
    }
    else if (mat == MAT_WINDOW)
    {
        // Glass pane: tinted, mostly reflection (handled by the PBR env term +
        // SSR in post), random warm rooms at night.
        base = lerp(base * 0.35, float3(0.08, 0.1, 0.13), 0.5);
        float3 id = floor(input.worldPos * float3(0.6, 0.45, 0.6));
        float room = hash21(id.xz + id.y * 7.31);
        float on = step(0.42, room) * night;
        emissive = on * float3(1.0, 0.82, 0.5) * (0.75 + 0.25 * hash21(id.xy)) * 1.6;
    }
    else if (mat == MAT_METAL)
    {
        float2 uv = abs(n.x) > 0.5 ? float2(input.worldPos.z, input.worldPos.y) : float2(input.worldPos.x, input.worldPos.y);
        float corr = n.y > 0.7 ? sin(input.worldPos.x * 7.0) : sin(uv.x * 7.0);
        base *= 0.86 + 0.14 * corr;
        base *= 0.94 + valueNoise(uv * 2.0) * 0.12;
        f0 = lerp(float3(0.04, 0.04, 0.04), base, metal);
    }
    else if (mat == MAT_CONCRETE)
    {
        base *= 0.94 + valueNoise(wp * 2.2 + input.worldPos.y * 0.7) * 0.12;
    }

    // ---- lighting ---------------------------------------------------------
    float shadow = sunShadow(input.worldPos, geoN, sun) * cloudShadow(input.worldPos, sun, time) * shadowScale;
    float3 sunRadiance = gParams.sunColor.xyz * SUN_INTENSITY * shadow;
    float3 lit = brdf(n, v, sun, base, rough, metal, f0, wrap) * sunRadiance;
    lit += sss * sunRadiance * 0.35;

    // Hemisphere ambient (diffuse) + environment specular from the sky colours.
    float hemi = 0.5 + 0.5 * n.y;
    float3 skyTint = lerp(gScene.zenith.xyz * 1.4 + 0.08, float3(0.12, 0.16, 0.3) * 0.35, night);
    float3 groundTint = lerp(float3(0.35, 0.32, 0.28), float3(0.06, 0.06, 0.08), night);
    float3 ambient = lerp(groundTint, skyTint, hemi) * gParams.sunColor.w * 1.15 * (0.82 + 0.18 * shadow);
    float ndv = max(dot(n, v), 1e-4);
    float3 Fenv = f0 + (max(float3(1.0 - rough, 1.0 - rough, 1.0 - rough), f0) - f0) * pow(1.0 - ndv, 5.0);
    lit += ambient * base * (1.0 - metal) * (1.0 - Fenv);
    float3 r = reflect(-v, n);
    float3 env = skyReflection(r, gParams.fog.xyz, gScene.zenith.xyz) * lerp(1.0, 0.35, night);
    lit += env * Fenv * (1.0 - rough * 0.85) * (0.7 + 0.3 * shadow) * 0.75;

    // Spotlights from the tile grid (street lamps, headlights, signs, floods).
    if (gScene.zenith.w > 0.0)
    {
        int tx = int(input.worldPos.x / gScene.terrain.z);
        int tz = int(input.worldPos.z / gScene.terrain.z);
        int nTiles = int(gScene.terrain.w);
        if (tx >= 0 && tz >= 0 && tx < nTiles && tz < nTiles)
        {
            uint packed = gLightGrid[tz * nTiles + tx];
            uint count = packed & 0xFFu;
            uint offset = packed >> 8;
            for (uint i = 0; i < count; ++i)
            {
                Light L = gLights[gLightRefs[offset + i]];
                float3 Lv = L.pos - input.worldPos;
                float d2 = dot(Lv, Lv);
                float d = sqrt(d2);
                if (d > L.range)
                    continue;
                float3 l = Lv / max(d, 0.01);
                float spot = smoothstep(L.cosOuter, L.cosInner, dot(-l, L.dir));
                float win = saturate(1.0 - pow(d / L.range, 4.0));
                float atten = spot * win * win / (d2 + 0.6);
                if (atten < 1e-4)
                    continue;
                lit += brdf(n, v, l, base, rough, metal, f0, wrap) * L.color * atten;
                lit += sss * L.color * atten * 0.2;
            }
        }
    }

    lit += emissive + waterExtra;

    // Distance fog towards the horizon colour, warmed towards the sun.
    float fogAmt = saturate(dist / gParams.fog.w);
    fogAmt = fogAmt * fogAmt;
    float sunward = pow(saturate(dot(-v, sun)), 6.0) * (1.0 - night);
    float3 fogCol = lerp(gParams.fog.xyz, gParams.sunColor.xyz * 0.8 + gParams.fog.xyz * 0.4, sunward * 0.5);
    lit = lerp(lit, fogCol, fogAmt * 0.85);

    o.color = float4(lit, 1.0);
    // Post needs the shading normal and roughness for SSR; metals and rough
    // dielectrics skip it (their env term above stands).
    float postRough = (metal > 0.5 || rough > 0.45) ? -1.0 : rough;
    o.aux = float4(n * 0.5 + 0.5, postRough);
    return o;
}
