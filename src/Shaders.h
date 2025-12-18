#pragma once

// 着色器源码 - 所有 GLSL 着色器代码

namespace Shaders {

// 计算着色器 - 粒子初始化
const char* const ComputeInitSaturn = R"(
#version 430 core
layout (local_size_x = 256) in;
struct ParticleData { vec4 pos; vec4 col; float speed; float isRing; float pad[2]; };
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

vec3 hexToRGB(uint hex) {
    return vec3((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF) / 255.0;
}

void main() {
    uint id = gl_GlobalInvocationID.x;
    if (id >= uMaxParticles) return;

    uint rngState = id * 1973u + uSeed * 9277u + 26699u;

    // 随机决定是本体还是环粒子 (25% 本体, 75% 环)
    // CPU代码是前25%固定为本体，后75%为环，然后shuffle。
    // 这里我们直接随机生成类型，达到shuffle的效果。
    float typeRnd = random(rngState);

    float R = 18.0;
    vec4 pPos, pCol;
    float pSpeed, pIsRing;

    if (typeRnd < 0.25) {
        // --- 土星本体粒子 ---
        float th = 6.28318 * random(rngState);
        float ph = acos(2.0 * random(rngState) - 1.0);

        pPos.x = R * sin(ph) * cos(th);
        pPos.y = R * cos(ph) * 0.9;
        pPos.z = R * sin(ph) * sin(th);

        // 纬度颜色计算 - 与CPU代码保持一致
        float lat = (pPos.y / 0.9 / R + 1.0) * 0.5;
        int idxInt = int(lat * 4.0 + cos(lat * 40.0) * 0.8 + cos(lat * 15.0) * 0.4);
        int ci = idxInt - (idxInt / 4) * 4; // 模拟 C++ 的 % 运算
        if (ci < 0) ci = 0; // 与CPU代码一致的负数处理

        vec3 cols[4];
        cols[0] = hexToRGB(0xE3DAC5);
        cols[1] = hexToRGB(0xC9A070);
        cols[2] = hexToRGB(0xE3DAC5);
        cols[3] = hexToRGB(0xB08D55);

        pCol.rgb = cols[ci];
        pPos.w = 1.0 + random(rngState) * 0.8; // scale
        pCol.a = 0.8;                          // opacity
        pSpeed = 0.0;                          // 本体不旋转
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

        pCol.rgb = c;
        pPos.w = s;
        pCol.a = o;
        pSpeed = 8.0 / sqrt(rad);
        pIsRing = 1.0;
    }

    // Fill the struct
    particles[id].pos = pPos;
    particles[id].col = pCol;
    particles[id].speed = pSpeed;
    particles[id].isRing = pIsRing;
}
)";

// 计算着色器 - 粒子物理模拟 (双缓冲)
const char* const ComputeSaturn = R"(
#version 430 core
layout (local_size_x = 256) in;
struct ParticleData { vec4 pos; vec4 col; float speed; float isRing; float pad[2]; };
layout(std430, binding = 0) readonly buffer ParticleBufferIn { ParticleData particlesIn[]; };
layout(std430, binding = 1) writeonly buffer ParticleBufferOut { ParticleData particlesOut[]; };
uniform float uDt;
uniform float uHandScale;
uniform float uHandHas;
uniform uint uParticleCount;

void main() {
    uint id = gl_GlobalInvocationID.x;
    if (id >= uParticleCount) return;

    vec4 pos = particlesIn[id].pos;
    float speed = particlesIn[id].speed;
    float isRing = particlesIn[id].isRing;

    float timeFactor = mix(1.0, uHandScale, uHandHas);
    float rotSpeed = mix(0.03, speed * 0.2, isRing);
    float angle = rotSpeed * uDt * timeFactor;

    float c = cos(angle), s = sin(angle);

    // 写入输出缓冲
    particlesOut[id].pos.x = pos.x * c - pos.z * s;
    particlesOut[id].pos.y = pos.y;
    particlesOut[id].pos.z = pos.x * s + pos.z * c;
    particlesOut[id].pos.w = pos.w;
    particlesOut[id].col = particlesIn[id].col;
    particlesOut[id].speed = speed;
    particlesOut[id].isRing = isRing;
}
)";

// 顶点着色器 - 土星粒子
const char* const VertexSaturn = R"(
#version 430 core
layout (location = 0) in vec4 aPos;
layout (location = 1) in vec4 aCol;
layout (location = 2) in float aSpeed;
layout (location = 3) in float aIsRing;
uniform mat4 view; uniform mat4 projection; uniform mat4 model;
uniform float uTime; uniform float uScale; uniform float uPixelRatio; uniform float uScreenHeight;
out vec3 vColor; out float vDist; out float vOpacity; out float vScaleFactor; out float vIsRing;

void main() {
    vec4 worldPos = model * vec4(aPos.xyz * uScale, 1.0);
    vec4 mvPosition = view * worldPos;
    float dist = -mvPosition.z;
    vDist = dist;

    // 混沌效果 - 使用 mix() 代替 if 分支避免着色器发散
    float chaosThreshold = 25.0;
    float chaosIntensity = smoothstep(chaosThreshold, 0.1, dist);
    chaosIntensity = chaosIntensity * chaosIntensity * chaosIntensity;

    float highFreqTime = uTime * 40.0;
    vec3 posScaled = aPos.xyz * 10.0;
    vec3 noiseVec = vec3(
        sin(highFreqTime + posScaled.x) * fract(sin(aPos.y * 43758.5) * 0.5),
        cos(highFreqTime + posScaled.y) * fract(sin(aPos.x * 43758.5) * 0.5),
        sin(highFreqTime * 0.5) * fract(sin(aPos.z * 43758.5) * 0.5)
    ) * 3.0;
    // 用 mix 替代 if，当 chaosIntensity 接近 0 时自然衰减
    mvPosition.xyz = mix(mvPosition.xyz, mvPosition.xyz + noiseVec, chaosIntensity);

    gl_Position = projection * mvPosition;
    
    float invDist = 1.0 / max(dist, 0.1);
    float basePointSize = aPos.w * 350.0 * invDist * 0.55;
    float screenScale = uScreenHeight / 1080.0;
    float pointSize = basePointSize * screenScale;
    float ringFactor = mix(mix(1.0, 0.8, step(dist, 50.0)), 1.0, aIsRing);
    pointSize *= ringFactor * pow(uPixelRatio, 0.8);
    gl_PointSize = clamp(pointSize, 0.0, 300.0 * screenScale);

    vColor = aCol.rgb; vOpacity = aCol.a; vScaleFactor = uScale; vIsRing = aIsRing;
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

// UI 着色器
const char* const VertexUI = R"(
#version 430 core
layout (location = 0) in vec2 aPos;
uniform mat4 projection;
void main() { gl_Position = projection * vec4(aPos, 0.0, 1.0); }
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

const char* const FragmentQuad = R"(
#version 430 core
out vec4 FragColor;
in vec2 vUV;
uniform sampler2D uTexture;
uniform int uTransparent;
void main(){
    vec3 col = texture(uTexture, vUV).rgb;
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

// 行星着色器
const char* const VertexPlanet = R"(
#version 430 core
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aNorm; layout(location=2) in vec2 aTex;
uniform mat4 m,v,p; out vec2 U; out vec3 N,V;
void main(){ 
    U=aTex; 
    N=normalize(mat3(transpose(inverse(m)))*aNorm); 
    vec4 P=v*m*vec4(aPos,1.0); 
    V=-P.xyz; 
    gl_Position=p*P; 
}
)";

const char* const FragmentPlanet = R"(
#version 430 core
out vec4 F; in vec2 U; in vec3 N,V; uniform vec3 c1,c2,ld; uniform float ns,at;
uniform sampler2D uFBMTex;  // 预计算的 FBM 噪声纹理
void main(){
    // 使用纹理采样代替程序化 FBM 计算
    float x=texture(uFBMTex, U*ns).r;
    vec3 c=mix(c1,c2,x)*max(dot(normalize(N),normalize(ld)),.05);
    c+=at*vec3(.5,.6,1.)*pow(1.-dot(normalize(V),normalize(N)),3.);
    F=vec4(c,1.);
}
)";

} // namespace Shaders
