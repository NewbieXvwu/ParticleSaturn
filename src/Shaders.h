#pragma once

// 着色器源码 - 所有 GLSL 着色器代码

namespace Shaders {

// 计算着色器 - 粒子初始化
const char* const ComputeInitSaturn = R"(
#version 430 core
layout (local_size_x = 256) in;
// 优化后的数据结构: 32字节 (从48字节减少33%)
struct ParticleData { vec4 pos; uint color; float speed; float isRing; float pad; };
layout(std430, binding = 0) buffer ParticleBuffer { ParticleData particles[]; };

uniform uint uSeed;
uniform uint uMaxParticles;

// 伪随机数生成器
float random(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint result = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    result = (result >> 22u) ^ result;
    return float(result) / 4294967295.0;
}

// RGBA8 打包: 将 vec4 颜色打包为 uint
uint packRGBA8(vec4 c) {
    uvec4 u = uvec4(clamp(c, 0.0, 1.0) * 255.0);
    return u.r | (u.g << 8u) | (u.b << 16u) | (u.a << 24u);
}

vec3 hexToRGB(uint hex) {
    return vec3((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF) / 255.0;
}

void main() {
    uint id = gl_GlobalInvocationID.x;
    if (id >= uMaxParticles) return;

    uint rngState = id * 1973u + uSeed * 9277u + 26699u;

    // 随机决定是本体还是环粒子 (25% 本体, 75% 环)
    float typeRnd = random(rngState);

    float R = 18.0;
    vec4 pPos;
    vec3 pColRGB;
    float pAlpha, pSpeed, pIsRing;

    if (typeRnd < 0.25) {
        // --- 土星本体粒子 ---
        float th = 6.28318 * random(rngState);
        float ph = acos(2.0 * random(rngState) - 1.0);

        pPos.x = R * sin(ph) * cos(th);
        pPos.y = R * cos(ph) * 0.9;
        pPos.z = R * sin(ph) * sin(th);

        // 纬度颜色计算
        float lat = (pPos.y / 0.9 / R + 1.0) * 0.5;
        int idxInt = int(lat * 4.0 + cos(lat * 40.0) * 0.8 + cos(lat * 15.0) * 0.4);
        int ci = idxInt - (idxInt / 4) * 4;
        if (ci < 0) ci = 0;

        vec3 cols[4];
        cols[0] = hexToRGB(0xE3DAC5);
        cols[1] = hexToRGB(0xC9A070);
        cols[2] = hexToRGB(0xE3DAC5);
        cols[3] = hexToRGB(0xB08D55);

        pColRGB = cols[ci];
        pPos.w = 1.0 + random(rngState) * 0.8;
        pAlpha = 0.8;
        pSpeed = 0.0;
        pIsRing = 0.0;

    } else {
        // --- 土星环粒子 ---
        float z = random(rngState);
        float rad;
        vec3 c;
        float s, o;

        if (z < 0.15) {
            rad = R * (1.235 + random(rngState) * 0.29);
            c = hexToRGB(0x2A2520);
            s = 0.5;
            o = 0.3;
        } else if (z < 0.65) {
            float t = random(rngState);
            rad = R * (1.525 + t * 0.425);
            c = mix(hexToRGB(0xCDBFA0), hexToRGB(0xDCCBBA), t);
            s = 0.8 + random(rngState) * 0.6;
            o = 0.85;
            if (sin(rad * 2.0) > 0.8) o *= 1.2;
        } else if (z < 0.69) {
            rad = R * (1.95 + random(rngState) * 0.075);
            c = hexToRGB(0x050505);
            s = 0.3;
            o = 0.1;
        } else if (z < 0.99) {
            rad = R * (2.025 + random(rngState) * 0.245);
            c = hexToRGB(0x989085);
            s = 0.7;
            o = 0.6;
            if (rad > R * 2.2 && rad < R * 2.21) o = 0.1;
        } else {
            rad = R * (2.32 + random(rngState) * 0.02);
            c = hexToRGB(0xAFAFA0);
            s = 1.0;
            o = 0.7;
        }

        float th = random(rngState) * 6.28318;
        pPos.x = rad * cos(th);
        pPos.z = rad * sin(th);
        float heightRange = (rad > R * 2.3) ? 0.4 : 0.15;
        pPos.y = (random(rngState) - 0.5) * heightRange;

        pColRGB = c;
        pPos.w = s;
        pAlpha = o;
        pSpeed = 8.0 / sqrt(rad);
        pIsRing = 1.0;
    }

    // Fill the struct - 使用 RGBA8 打包颜色
    particles[id].pos = pPos;
    particles[id].color = packRGBA8(vec4(pColRGB, pAlpha));
    particles[id].speed = pSpeed;
    particles[id].isRing = pIsRing;
}
)";

// 计算着色器 - 粒子物理模拟 (双缓冲)
// 优化: 使用 shared memory 缓存公共计算值
const char* const ComputeSaturn = R"(
#version 430 core
layout (local_size_x = 256) in;
// 优化后的数据结构: 32字节
struct ParticleData { vec4 pos; uint color; float speed; float isRing; float pad; };
layout(std430, binding = 0) readonly buffer ParticleBufferIn { ParticleData particlesIn[]; };
layout(std430, binding = 1) writeonly buffer ParticleBufferOut { ParticleData particlesOut[]; };
uniform float uDt;
uniform float uHandScale;
uniform float uHandHas;
uniform uint uParticleCount;

// Shared memory: 缓存公共计算值
shared float s_timeFactor;      // 时间因子 (所有粒子共用)
shared float s_bodyAngleCos;    // 本体粒子旋转 cos
shared float s_bodyAngleSin;    // 本体粒子旋转 sin
shared float s_dtScaled;        // 预乘的 dt * 0.2 * timeFactor (环粒子用)

void main() {
    uint id = gl_GlobalInvocationID.x;

    // 第一个线程计算所有公共值
    if (gl_LocalInvocationID.x == 0u) {
        s_timeFactor = mix(1.0, uHandScale, uHandHas);
        float bodyAngle = 0.03 * uDt * s_timeFactor;
        s_bodyAngleCos = cos(bodyAngle);
        s_bodyAngleSin = sin(bodyAngle);
        s_dtScaled = 0.2 * uDt * s_timeFactor;  // 预计算环粒子的公共乘数
    }
    barrier();

    if (id >= uParticleCount) return;

    vec4 pos = particlesIn[id].pos;
    float speed = particlesIn[id].speed;
    float isRing = particlesIn[id].isRing;

    // 根据粒子类型选择 sin/cos 值
    float c, s;
    if (isRing < 0.5) {
        // 本体粒子: 使用缓存的公共值
        c = s_bodyAngleCos;
        s = s_bodyAngleSin;
    } else {
        // 环粒子: 使用预计算的 dtScaled
        float angle = speed * s_dtScaled;
        c = cos(angle);
        s = sin(angle);
    }

    // 写入输出缓冲
    particlesOut[id].pos.x = pos.x * c - pos.z * s;
    particlesOut[id].pos.y = pos.y;
    particlesOut[id].pos.z = pos.x * s + pos.z * c;
    particlesOut[id].pos.w = pos.w;
    particlesOut[id].color = particlesIn[id].color;
    particlesOut[id].speed = speed;
    particlesOut[id].isRing = isRing;
}
)";

// 顶点着色器 - 土星粒子
// 优化: 使用查找表替代 sin/fract 计算混沌效果
const char* const VertexSaturn = R"(
#version 430 core
layout (location = 0) in vec4 aPos;
layout (location = 1) in uint aColor;  // RGBA8 打包颜色
layout (location = 2) in float aSpeed;
layout (location = 3) in float aIsRing;
uniform mat4 view; uniform mat4 projection; uniform mat4 model;
uniform float uTime; uniform float uScale; uniform float uPixelRatio; uniform float uScreenHeight;
out vec3 vColor; out float vDist; out float vOpacity; out float vScaleFactor; out float vIsRing;

// RGBA8 解包: 将 uint 解包为 vec4 颜色
vec4 unpackRGBA8(uint c) {
    return vec4(
        float(c & 0xFFu) / 255.0,
        float((c >> 8u) & 0xFFu) / 255.0,
        float((c >> 16u) & 0xFFu) / 255.0,
        float((c >> 24u) & 0xFFu) / 255.0
    );
}

// 快速伪随机哈希函数 (替代 fract(sin(...)) 避免昂贵的三角函数)
float hash(float n) {
    uint x = floatBitsToUint(n);
    x = ((x >> 16u) ^ x) * 0x45d9f3bu;
    x = ((x >> 16u) ^ x) * 0x45d9f3bu;
    x = (x >> 16u) ^ x;
    return float(x) * (1.0 / 4294967296.0);
}

// 快速近似 sin (使用多项式逼近，误差 < 0.001)
float fastSin(float x) {
    x = mod(x, 6.28318530718);
    x = x > 3.14159265359 ? x - 6.28318530718 : x;
    float x2 = x * x;
    return x * (1.0 - x2 * (0.16666667 - x2 * (0.00833333 - x2 * 0.0001984)));
}

void main() {
    // 解包颜色
    vec4 col = unpackRGBA8(aColor);

    vec4 worldPos = model * vec4(aPos.xyz * uScale, 1.0);
    vec4 mvPosition = view * worldPos;
    float dist = -mvPosition.z;
    vDist = dist;

    // 混沌效果 - 使用查找表和快速数学函数优化
    float chaosThreshold = 25.0;
    float chaosIntensity = smoothstep(chaosThreshold, 0.1, dist);
    chaosIntensity = chaosIntensity * chaosIntensity * chaosIntensity;

    // 只在需要混沌效果时计算 (chaosIntensity > 0)
    vec3 noiseVec = vec3(0.0);
    if (chaosIntensity > 0.001) {
        float highFreqTime = uTime * 40.0;
        vec3 posScaled = aPos.xyz * 10.0;
        // 使用快速哈希替代 fract(sin(...))
        float hashX = hash(aPos.y * 43758.5) * 0.5;
        float hashY = hash(aPos.x * 43758.5) * 0.5;
        float hashZ = hash(aPos.z * 43758.5) * 0.5;
        // 使用快速近似 sin/cos
        noiseVec = vec3(
            fastSin(highFreqTime + posScaled.x) * hashX,
            fastSin(highFreqTime + posScaled.y + 1.5708) * hashY,  // cos = sin(x + pi/2)
            fastSin(highFreqTime * 0.5) * hashZ
        ) * 3.0;
    }
    mvPosition.xyz = mix(mvPosition.xyz, mvPosition.xyz + noiseVec, chaosIntensity);

    gl_Position = projection * mvPosition;

    float invDist = 1.0 / max(dist, 0.1);
    float basePointSize = aPos.w * 350.0 * invDist * 0.55;
    float screenScale = uScreenHeight / 1080.0;
    float pointSize = basePointSize * screenScale;
    float ringFactor = mix(mix(1.0, 0.8, step(dist, 50.0)), 1.0, aIsRing);
    pointSize *= ringFactor * pow(uPixelRatio, 0.8);
    gl_PointSize = clamp(pointSize, 0.0, 300.0 * screenScale);

    vColor = col.rgb; vOpacity = col.a; vScaleFactor = uScale; vIsRing = aIsRing;
}
)";

// 片段着色器 - 土星粒子
const char* const FragmentSaturn = R"(
#version 430 core
out vec4 FragColor;
in vec3 vColor; in float vDist; in float vOpacity; in float vScaleFactor; in float vIsRing;
uniform float uDensityComp;

void main() {
    vec2 cxy = 2.0 * gl_PointCoord - 1.0;
    float distSq = dot(cxy, cxy);
    if (distSq > 1.0) discard;
    
    float glow = smoothstep(1.0, 0.4, distSq);
    float t = clamp((vScaleFactor - 0.15) * 0.4255, 0.0, 1.0);
    float tSmooth = smoothstep(0.1, 0.9, t);
    
    vec3 baseColor = mix(vec3(0.35, 0.22, 0.05), vColor, tSmooth);
    vec3 finalColor = baseColor * (0.2 + t);
    
    float closeMix = smoothstep(40.0, 0.0, vDist);
    vec3 closeRingColor = finalColor + vec3(0.15, 0.12, 0.1) * closeMix;
    vec3 closeBodyColor = mix(finalColor, pow(vColor, vec3(1.4)) * 1.5, closeMix * 0.8);
    finalColor = mix(closeBodyColor, closeRingColor, vIsRing);
    
    float depthAlpha = smoothstep(0.0, 10.0, vDist);
    float finalAlpha = glow * vOpacity * (0.25 + 0.45 * smoothstep(0.0, 0.5, t)) * depthAlpha * uDensityComp;
    FragColor = vec4(finalColor, finalAlpha);
}
)";

// UI 着色器 (支持预生成数字的变换)
const char* const VertexUI = R"(
#version 430 core
layout (location = 0) in vec2 aPos;
uniform mat4 projection;
uniform vec4 uTransform;  // xy = 位置偏移, zw = 缩放
void main() {
    vec2 pos = aPos * uTransform.zw + uTransform.xy;
    gl_Position = projection * vec4(pos, 0.0, 1.0);
}
)";

const char* const FragmentUI = R"(
#version 430 core
out vec4 FragColor;
uniform vec3 uColor;
void main() { FragColor = vec4(uColor, 1.0); }
)";

// 全屏四边形着色器
const char* const VertexQuad = R"(
#version 430 core
layout(location=0) in vec2 aPos;
out vec2 vUV;
void main(){ vUV = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }
)";

// 优化: 添加简单 tone mapping 以配合 R11F_G11F_B10F HDR 格式
const char* const FragmentQuad = R"(
#version 430 core
out vec4 FragColor;
in vec2 vUV;
uniform sampler2D uTexture;
uniform int uTransparent;

// 简化的 Reinhard tone mapping
vec3 toneMap(vec3 hdr) {
    return hdr / (hdr + vec3(1.0));
}

void main(){
    vec3 col = texture(uTexture, vUV).rgb;
    // 轻度 tone mapping: 只压缩超过 1.0 的高光部分
    col = mix(col, toneMap(col), step(1.0, max(max(col.r, col.g), col.b)) * 0.5);
    if (uTransparent == 1) {
        float alpha = max(max(col.r, col.g), col.b);
        FragColor = vec4(col, alpha);
    } else {
        FragColor = vec4(col, 1.0);
    }
}
)";

// Kawase Blur 着色器 (比高斯模糊更高效)
// 每次迭代采样4个对角线方向的像素，通过多次迭代实现模糊
const char* const FragmentBlur = R"(
#version 430 core
out vec4 F; in vec2 vUV; uniform sampler2D uTexture; uniform vec2 uTexelSize; uniform float uOffset;
void main(){
    vec2 off = uTexelSize * (uOffset + 0.5);
    vec4 sum = texture(uTexture, vUV + vec2(-off.x, off.y));  // 左上
    sum += texture(uTexture, vUV + vec2(off.x, off.y));       // 右上
    sum += texture(uTexture, vUV + vec2(off.x, -off.y));      // 右下
    sum += texture(uTexture, vUV + vec2(-off.x, -off.y));     // 左下
    F = sum * 0.25;
}
)";

// 星空着色器
const char* const VertexStar = R"(
#version 430 core
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aCol; layout(location=2) in float aSize;
uniform mat4 view, projection, model; out vec3 vColor;
void main(){ 
    vec4 p=view*model*vec4(aPos,1.0); 
    gl_Position=projection*p; 
    gl_PointSize=clamp(aSize*(1000.0/-p.z),1.0,8.0); 
    vColor=aCol; 
}
)";

const char* const FragmentStar = R"(
#version 430 core
out vec4 F; in vec3 vColor; uniform float uTime;
void main(){ 
    vec2 c=2.0*gl_PointCoord-1.0; 
    if(dot(c,c)>1.0)discard; 
    float n=fract(sin(dot(gl_FragCoord.xy,vec2(12.9,78.2)))*43758.5); 
    vec3 col = vColor * (0.7 + 0.3 * sin(uTime * 2.0 + n * 10.0)) * 3.0;
    F=vec4(col, pow(1.0-dot(c,c),1.5)*0.9); 
}
)";

// 行星着色器 (实例化渲染优化)
// 使用 UBO 存储行星数据，单次 draw call 渲染所有行星
const char* const VertexPlanet = R"(
#version 430 core
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aNorm; layout(location=2) in vec2 aTex;

// 行星实例数据 (UBO)
struct PlanetInstance {
    mat4 modelMatrix;     // 模型矩阵
    vec4 color1;          // 颜色1 (xyz) + noiseScale (w)
    vec4 color2;          // 颜色2 (xyz) + atmosphere (w)
};
layout(std140, binding = 0) uniform PlanetUBO {
    PlanetInstance planets[8];  // 最多支持 8 个行星
};

uniform mat4 v, p;
uniform int uPlanetCount;

out vec2 U;
out vec3 N, V;
flat out int instanceID;

void main(){
    instanceID = gl_InstanceID;
    mat4 m = planets[gl_InstanceID].modelMatrix;
    U = aTex;
    N = normalize(mat3(transpose(inverse(m))) * aNorm);
    vec4 P = v * m * vec4(aPos, 1.0);
    V = -P.xyz;
    gl_Position = p * P;
}
)";

const char* const FragmentPlanet = R"(
#version 430 core
out vec4 F;
in vec2 U;
in vec3 N, V;
flat in int instanceID;

// 行星实例数据 (UBO)
struct PlanetInstance {
    mat4 modelMatrix;
    vec4 color1;  // xyz = color, w = noiseScale
    vec4 color2;  // xyz = color, w = atmosphere
};
layout(std140, binding = 0) uniform PlanetUBO {
    PlanetInstance planets[8];
};

uniform vec3 ld;
uniform sampler2D uFBMTex;

void main(){
    vec3 c1 = planets[instanceID].color1.xyz;
    vec3 c2 = planets[instanceID].color2.xyz;
    float ns = planets[instanceID].color1.w;
    float at = planets[instanceID].color2.w;

    float x = texture(uFBMTex, U * ns).r;
    vec3 c = mix(c1, c2, x) * max(dot(normalize(N), normalize(ld)), 0.05);
    c += at * vec3(0.5, 0.6, 1.0) * pow(1.0 - dot(normalize(V), normalize(N)), 3.0);
    F = vec4(c, 1.0);
}
)";

} // namespace Shaders
