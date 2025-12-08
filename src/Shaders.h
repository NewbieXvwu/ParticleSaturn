#pragma once

// 着色器源码 - 所有 GLSL 着色器代码

namespace Shaders {

// 计算着色器 - 粒子物理模拟
const char* const ComputeSaturn = R"(
#version 430 core
layout (local_size_x = 256) in;
struct ParticleData { vec4 pos; vec4 col; vec4 vel; float isRing; float pad[3]; };
layout(std430, binding = 0) buffer ParticleBuffer { ParticleData particles[]; };
uniform float uDt;
uniform float uHandScale;
uniform float uHandHas;
uniform uint uParticleCount;

void main() {
    uint id = gl_GlobalInvocationID.x;
    if (id >= uParticleCount) return;
    
    vec4 pos = particles[id].pos;
    float speed = particles[id].vel.w;
    float isRing = particles[id].isRing;
    
    float timeFactor = mix(1.0, uHandScale, uHandHas);
    float rotSpeed = mix(0.03, speed * 0.2, isRing);
    float angle = rotSpeed * uDt * timeFactor;
    
    float c = cos(angle), s = sin(angle);
    
    particles[id].pos.x = pos.x * c - pos.z * s;
    particles[id].pos.z = pos.x * s + pos.z * c;
}
)";

// 顶点着色器 - 土星粒子
const char* const VertexSaturn = R"(
#version 430 core
layout (location = 0) in vec4 aPos;
layout (location = 1) in vec4 aCol;
layout (location = 2) in vec4 aVel;
layout (location = 3) in float aIsRing;
uniform mat4 view; uniform mat4 projection; uniform mat4 model;
uniform float uTime; uniform float uScale; uniform float uPixelRatio; uniform float uScreenHeight;
out vec3 vColor; out float vDist; out float vOpacity; out float vScaleFactor; out float vIsRing;

void main() {
    vec4 worldPos = model * vec4(aPos.xyz * uScale, 1.0);
    vec4 mvPosition = view * worldPos;
    float dist = -mvPosition.z;
    vDist = dist;
    
    float chaosThreshold = 25.0;
    float chaosIntensity = smoothstep(chaosThreshold, 0.1, dist);
    chaosIntensity = chaosIntensity * chaosIntensity * chaosIntensity;
    
    if (chaosIntensity > 0.001) {
        float highFreqTime = uTime * 40.0;
        vec3 posScaled = aPos.xyz * 10.0;
        vec3 noiseVec = vec3(
            sin(highFreqTime + posScaled.x) * fract(sin(aPos.y * 43758.5) * 0.5),
            cos(highFreqTime + posScaled.y) * fract(sin(aPos.x * 43758.5) * 0.5),
            sin(highFreqTime * 0.5) * fract(sin(aPos.z * 43758.5) * 0.5)
        ) * chaosIntensity * 3.0;
        mvPosition.xyz += noiseVec;
    }
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

// 高斯模糊着色器
const char* const FragmentBlur = R"(
#version 430 core
out vec4 F; in vec2 vUV; uniform sampler2D uTexture; uniform vec2 dir; 
void main(){ 
    vec4 sum = texture(uTexture, vUV) * 0.227027;
    vec2 off1 = dir * 1.3846153846;
    vec2 off2 = dir * 3.2307692308;
    sum += texture(uTexture, vUV + off1) * 0.316216;
    sum += texture(uTexture, vUV - off1) * 0.316216;
    sum += texture(uTexture, vUV + off2) * 0.070270;
    sum += texture(uTexture, vUV - off2) * 0.070270;
    F = sum;
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
float h(vec2 s){return fract(sin(dot(s,vec2(12.9,78.2)))*43758.5);}
float n(vec2 s){vec2 i=floor(s),f=fract(s),u=f*f*(3.-2.*f); return mix(mix(h(i),h(i+vec2(1,0)),u.x),mix(h(i+vec2(0,1)),h(i+1.),u.x),u.y);}
float fbm(vec2 s){float v=0.,a=.5;for(int i=0;i<5;i++,s*=2.,a*=.5)v+=a*n(s);return v;}
void main(){ 
    float x=fbm(U*ns); 
    vec3 c=mix(c1,c2,x)*max(dot(normalize(N),normalize(ld)),.05);
    c+=at*vec3(.5,.6,1.)*pow(1.-dot(normalize(V),normalize(N)),3.); 
    F=vec4(c,1.); 
}
)";

} // namespace Shaders
