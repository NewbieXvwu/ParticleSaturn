/*
 * Particle Saturn - Native Engine (Ultimate Edition)
 * Features: 100% GPU particles + dynamic LOD + smooth animation + FPS display + DLL hand tracking
 */

#ifdef _WIN32
#include <Windows.h>
#include <dwmapi.h>
#include <imm.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "imm32.lib")

// Windows 10 1809+ 深色模式支持
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// Windows 11 22H2+ Mica/Acrylic 背景支持
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38

// DWM_SYSTEMBACKDROP_TYPE 枚举值（仅在旧 SDK 中定义）
enum {
    DWMSBT_AUTO_CUSTOM = 0,            // 自动（默认）
    DWMSBT_NONE_CUSTOM = 1,            // 无背景
    DWMSBT_MAINWINDOW_CUSTOM = 2,      // Mica（主窗口风格）
    DWMSBT_TRANSIENTWINDOW_CUSTOM = 3, // Acrylic（临时窗口风格）
    DWMSBT_TABBEDWINDOW_CUSTOM = 4     // Mica Alt（标签窗口风格）
};
#define DWMSBT_TABBEDWINDOW DWMSBT_TABBEDWINDOW_CUSTOM
#endif
#endif

// For embedded resources (Release_Static build)
#ifdef EMBED_MODELS
#include "resource.h"
#endif

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <iostream>
#include <vector>
#include <string>
#include <random>
#include <sstream>
#include <deque>
#include <mutex>
#include <map>

// --- Dear ImGui ---
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

// --- HandTracker ---
#include "HandTracker.h"

// --- 动画辅助结构体 ---
struct AnimFloat {
    float val = 0.0f, target = 0.0f;
    void Update(float dt, float speed = 15.0f) {
        val += (target - val) * (1.0f - exp(-speed * dt));
        if (std::abs(target - val) < 0.001f) val = target;
    }
};

struct UIAnimState { AnimFloat bgOpacity, knobPos, knobSize; bool active = false; };
std::map<ImGuiID, UIAnimState> g_animStates;

// --- 模糊用 Framebuffer ---
struct BlurFramebuffer {
    GLuint fbo = 0, tex = 0; int w = 0, h = 0;
    void Init(int width, int height) {
        w = width; h = height;
        if (fbo) { glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &tex); }
        glGenFramebuffers(1, &fbo); glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, 0, GL_RGB, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
};

// --- 调试日志捕获系统 ---
class DebugLog {
public:
    static DebugLog& Instance() { static DebugLog inst; return inst; }
    
    void Add(const std::string& msg) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lines.push_back(msg);
        if (m_lines.size() > MAX_LINES) m_lines.pop_front();
        m_scrollToBottom = true;
    }
    
    void Draw() {
        ImGui::BeginChild("LogScroll", ImVec2(0, 200), true);
        for (const auto& line : m_lines) {
            ImGui::TextUnformatted(line.c_str());
        }
        if (m_scrollToBottom) {
            ImGui::SetScrollHereY(1.0f);
            m_scrollToBottom = false;
        }
        ImGui::EndChild();
    }
    
    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lines.clear();
    }
    
    std::string GetAllText() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string result;
        for (const auto& line : m_lines) {
            result += line + "\n";
        }
        return result;
    }
    
private:
    std::deque<std::string> m_lines;
    std::mutex m_mutex;
    bool m_scrollToBottom = false;
    static const size_t MAX_LINES = 200;
};

// 重定向 std::cout 到调试日志的辅助类
class DebugStreamBuf : public std::streambuf {
public:
    DebugStreamBuf(std::streambuf* orig) : m_orig(orig) {}
    
protected:
    int overflow(int c) override {
        if (c != EOF) {
            if (c == '\n') {
                DebugLog::Instance().Add(m_buffer);
                m_buffer.clear();
            } else {
                m_buffer += (char)c;
            }
            if (m_orig) m_orig->sputc(c);
        }
        return c;
    }
    
private:
    std::streambuf* m_orig;
    std::string m_buffer;
};

// --- 常量定义 ---
const unsigned int INIT_WIDTH = 1920;
const unsigned int INIT_HEIGHT = 1080;
const unsigned int MAX_PARTICLES = 1200000;
const unsigned int MIN_PARTICLES = 200000;

// --- 窗口尺寸（可变） ---
unsigned int g_scrWidth = INIT_WIDTH;
unsigned int g_scrHeight = INIT_HEIGHT;
bool g_windowResized = true; // 标记窗口是否需要更新投影矩阵

// --- 背景模式 ---
// 可用模式列表（启动时检测填充）: 0=纯黑, 1=Acrylic, 2=Mica
std::vector<int> g_availableBackdrops = { 0 };  // 纯黑始终可用
int g_backdropIndex = 0;  // 当前在 g_availableBackdrops 中的索引
bool g_useTransparent = false;  // 当前是否使用透明渲染

// --- 全屏状态 ---
bool g_isFullscreen = false;
int g_windowedX = 100, g_windowedY = 100;  // 窗口模式下的位置
int g_windowedW = INIT_WIDTH, g_windowedH = INIT_HEIGHT;  // 窗口模式下的尺寸

// --- 调试状态 ---
bool g_showDebugWindow = false;  // ImGui 调试窗口显示状态
bool g_showCameraDebug = false;  // OpenCV 摄像头调试窗口
float g_dpiScale = 1.0f;  // DPI 缩放因子
bool g_isDarkMode = true;  // 当前是否深色模式
unsigned int activeParticleCount = MAX_PARTICLES;
float currentPixelRatio = 1.0f;

// --- 玻璃模糊效果 ---
bool g_enableImGuiBlur = true;
float g_blurStrength = 2.0f;

// --- ƽ�����ɱ��� ---
struct SmoothState {
    float scale = 1.0f;
    float rotX = 0.4f;
    float rotY = 0.0f;
} currentAnim;

// --- ��������״̬ ---
struct HandState {
    bool hasHand = false;
    float scale = 1.0f;
    float rotX = 0.5f;
    float rotY = 0.5f;
} handState;

// --- �������� ---
float lerp(float a, float b, float f) { return a + f * (b - a); }
glm::vec3 hexToRGB(int hex) {
    return glm::vec3(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f);
}

// ======================= SHADERS =======================

// 1. Compute Shader (Physics) - 优化版本
const char* csSaturn = R"(
#version 430 core
layout (local_size_x = 256) in;  // 增加工作组大小，更好利用GPU
struct ParticleData { vec4 pos; vec4 col; vec4 vel; float isRing; float pad[3]; };
layout(std430, binding = 0) buffer ParticleBuffer { ParticleData particles[]; };
uniform float uDt;
uniform float uHandScale;
uniform float uHandHas;
uniform uint uParticleCount;  // 动态粒子数量
void main() {
    uint id = gl_GlobalInvocationID.x;
    if (id >= uParticleCount) return;
    
    // 只读取需要的数据，减少内存带宽
    vec4 pos = particles[id].pos;
    float speed = particles[id].vel.w;
    float isRing = particles[id].isRing;
    
    // 使用 mix 替代分支，GPU更友好
    float timeFactor = mix(1.0, uHandScale, uHandHas);
    float rotSpeed = mix(0.03, speed * 0.2, isRing);
    float angle = rotSpeed * uDt * timeFactor;
    
    // 使用 sincos 优化（GLSL会自动优化）
    float c = cos(angle), s = sin(angle);
    
    // 只写回修改的位置数据
    particles[id].pos.x = pos.x * c - pos.z * s;
    particles[id].pos.z = pos.x * s + pos.z * c;
}
)";

// 2. Vertex Shader - 优化版本
const char* vsSaturn = R"(
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
    
    // 混沌效果 - 使用 smoothstep 替代分支
    float chaosThreshold = 25.0;
    float chaosIntensity = smoothstep(chaosThreshold, 0.1, dist);
    chaosIntensity = chaosIntensity * chaosIntensity * chaosIntensity; // pow(x, 3)
    
    if (chaosIntensity > 0.001) {  // 只在需要时计算
        float highFreqTime = uTime * 40.0;
        vec3 posScaled = aPos.xyz * 10.0;
        // 简化的噪声计算
        vec3 noiseVec = vec3(
            sin(highFreqTime + posScaled.x) * fract(sin(aPos.y * 43758.5) * 0.5),
            cos(highFreqTime + posScaled.y) * fract(sin(aPos.x * 43758.5) * 0.5),
            sin(highFreqTime * 0.5) * fract(sin(aPos.z * 43758.5) * 0.5)
        ) * chaosIntensity * 3.0;
        mvPosition.xyz += noiseVec;
    }
    gl_Position = projection * mvPosition;
    
    // 点大小计算 - 根据屏幕高度动态缩放
    float invDist = 1.0 / max(dist, 0.1);
    float basePointSize = aPos.w * 350.0 * invDist * 0.55;
    float screenScale = uScreenHeight / 1080.0;  // 基于1080p的缩放因子
    float pointSize = basePointSize * screenScale;
    float ringFactor = mix(mix(1.0, 0.8, step(dist, 50.0)), 1.0, aIsRing);
    pointSize *= ringFactor * pow(uPixelRatio, 0.8);
    gl_PointSize = clamp(pointSize, 0.0, 300.0 * screenScale);

    vColor = aCol.rgb; vOpacity = aCol.a; vScaleFactor = uScale; vIsRing = aIsRing;
}
)";

// 3. Fragment Shader - 优化版本 (premultiplied alpha for transparent window)
const char* fsSaturn = R"(
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

// 4. UI Shader
const char* vsUI = R"(
#version 430 core
layout (location = 0) in vec2 aPos;
uniform mat4 projection;
void main() { gl_Position = projection * vec4(aPos, 0.0, 1.0); }
)";
const char* fsUI = R"(
#version 430 core
out vec4 FragColor;
uniform vec3 uColor;
void main() { FragColor = vec4(uColor, 1.0); }
)";

// 6. Fullscreen Quad Shader - 将 FBO 渲染到窗口
const char* vsQuad = R"(#version 430 core
layout(location=0) in vec2 aPos;
out vec2 vUV;
void main(){ vUV = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); })";
const char* fsQuad = R"(#version 430 core
out vec4 FragColor;
in vec2 vUV;
uniform sampler2D uTexture;
uniform int uTransparent;  // 0=不透明, 1=透明模式
void main(){
    vec3 col = texture(uTexture, vUV).rgb;
    if (uTransparent == 1) {
        // 透明模式：用最大通道计算 alpha，黑色变透明
        float alpha = max(max(col.r, col.g), col.b);
        FragColor = vec4(col, alpha);
    } else {
        // 不透明模式：直接输出
        FragColor = vec4(col, 1.0);
    }
})";

// --- 玻璃模糊 Shader ---
const char* fsBlur = R"(#version 430 core
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
})";

// 5. Star & Planet Shaders
const char* vsStar = R"(#version 430 core
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aCol; layout(location=2) in float aSize;
uniform mat4 view, projection, model; out vec3 vColor;
void main(){ vec4 p=view*model*vec4(aPos,1.0); gl_Position=projection*p; gl_PointSize=clamp(aSize*(1000.0/-p.z),1.0,8.0); vColor=aCol; })";
const char* fsStar = R"(#version 430 core
out vec4 F; in vec3 vColor; uniform float uTime;
void main(){ vec2 c=2.0*gl_PointCoord-1.0; if(dot(c,c)>1.0)discard; 
float n=fract(sin(dot(gl_FragCoord.xy,vec2(12.9,78.2)))*43758.5); 
// 提高星星亮度，让它们在透明窗口合成后仍然可见
vec3 col = vColor * (0.7 + 0.3 * sin(uTime * 2.0 + n * 10.0)) * 3.0;
F=vec4(col, pow(1.0-dot(c,c),1.5)*0.9); })";
const char* vsPlanet = R"(#version 430 core
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aNorm; layout(location=2) in vec2 aTex;
uniform mat4 m,v,p; out vec2 U; out vec3 N,V;
void main(){ U=aTex; N=normalize(mat3(transpose(inverse(m)))*aNorm); vec4 P=v*m*vec4(aPos,1.0); V=-P.xyz; gl_Position=p*P; })";
const char* fsPlanet = R"(#version 430 core
out vec4 F; in vec2 U; in vec3 N,V; uniform vec3 c1,c2,ld; uniform float ns,at;
float h(vec2 s){return fract(sin(dot(s,vec2(12.9,78.2)))*43758.5);}
float n(vec2 s){vec2 i=floor(s),f=fract(s),u=f*f*(3.-2.*f); return mix(mix(h(i),h(i+vec2(1,0)),u.x),mix(h(i+vec2(0,1)),h(i+1.),u.x),u.y);}
float fbm(vec2 s){float v=0.,a=.5;for(int i=0;i<5;i++,s*=2.,a*=.5)v+=a*n(s);return v;}
void main(){ float x=fbm(U*ns); vec3 c=mix(c1,c2,x)*max(dot(normalize(N),normalize(ld)),.05);
c+=at*vec3(.5,.6,1.)*pow(1.-dot(normalize(V),normalize(N)),3.); F=vec4(c,1.); })";

// --- 窗口大小变化回调 ---
void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    if (width > 0 && height > 0) {
        g_scrWidth = width;
        g_scrHeight = height;
        g_windowResized = true;
        glViewport(0, 0, width, height);
    }
}

// --- ���ߺ��� ---
unsigned int createProg(const char* v, const char* f) {
    unsigned int p = glCreateProgram();
    unsigned int vs = glCreateShader(GL_VERTEX_SHADER); glShaderSource(vs, 1, &v, 0); glCompileShader(vs);
    unsigned int fs = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(fs, 1, &f, 0); glCompileShader(fs);
    glAttachShader(p, vs); glAttachShader(p, fs); glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs); // 释放已链接的shader
    return p;
}

// --- Uniform Location 缓存结构 ---
struct UniformCache {
    // Compute shader
    GLint comp_uDt, comp_uHandScale, comp_uHandHas, comp_uParticleCount;
    // Saturn shader
    GLint sat_proj, sat_view, sat_model, sat_uTime, sat_uScale, sat_uPixelRatio, sat_uDensityComp, sat_uScreenHeight;
    // Star shader
    GLint star_proj, star_view, star_model, star_uTime;
    // Planet shader
    GLint pl_p, pl_v, pl_m, pl_ld, pl_c1, pl_c2, pl_ns, pl_at;
    // UI shader
    GLint ui_proj, ui_uColor;
};

void initUniformCache(UniformCache& uc, unsigned int pComp, unsigned int pSaturn, unsigned int pStar, unsigned int pPlanet, unsigned int pUI) {
    uc.comp_uDt = glGetUniformLocation(pComp, "uDt");
    uc.comp_uHandScale = glGetUniformLocation(pComp, "uHandScale");
    uc.comp_uHandHas = glGetUniformLocation(pComp, "uHandHas");
    uc.comp_uParticleCount = glGetUniformLocation(pComp, "uParticleCount");
    
    uc.sat_proj = glGetUniformLocation(pSaturn, "projection");
    uc.sat_view = glGetUniformLocation(pSaturn, "view");
    uc.sat_model = glGetUniformLocation(pSaturn, "model");
    uc.sat_uTime = glGetUniformLocation(pSaturn, "uTime");
    uc.sat_uScale = glGetUniformLocation(pSaturn, "uScale");
    uc.sat_uPixelRatio = glGetUniformLocation(pSaturn, "uPixelRatio");
    uc.sat_uDensityComp = glGetUniformLocation(pSaturn, "uDensityComp");
    uc.sat_uScreenHeight = glGetUniformLocation(pSaturn, "uScreenHeight");
    
    uc.star_proj = glGetUniformLocation(pStar, "projection");
    uc.star_view = glGetUniformLocation(pStar, "view");
    uc.star_model = glGetUniformLocation(pStar, "model");
    uc.star_uTime = glGetUniformLocation(pStar, "uTime");
    
    uc.pl_p = glGetUniformLocation(pPlanet, "p");
    uc.pl_v = glGetUniformLocation(pPlanet, "v");
    uc.pl_m = glGetUniformLocation(pPlanet, "m");
    uc.pl_ld = glGetUniformLocation(pPlanet, "ld");
    uc.pl_c1 = glGetUniformLocation(pPlanet, "c1");
    uc.pl_c2 = glGetUniformLocation(pPlanet, "c2");
    uc.pl_ns = glGetUniformLocation(pPlanet, "ns");
    uc.pl_at = glGetUniformLocation(pPlanet, "at");
    
    uc.ui_proj = glGetUniformLocation(pUI, "projection");
    uc.ui_uColor = glGetUniformLocation(pUI, "uColor");
}

// �򵥵� 7����ʾ������������ (����FPS)
const int DIGITS[10][7] = {
    {1,1,1,1,1,1,0}, {0,1,1,0,0,0,0}, {1,1,0,1,1,0,1}, {1,1,1,1,0,0,1}, {0,1,1,0,0,1,1},
    {1,0,1,1,0,1,1}, {1,0,1,1,1,1,1}, {1,1,1,0,0,0,0}, {1,1,1,1,1,1,1}, {1,1,1,1,0,1,1}
};
void addDigitGeometry(std::vector<float>& verts, float x, float y, float w, float h, int num) {
    if (num < 0 || num > 9) return;
    float p[6][2] = { {x,y + h}, {x + w,y + h}, {x + w,y + h / 2}, {x + w,y}, {x,y}, {x,y + h / 2} };
    auto line = [&](int i1, int i2) { verts.push_back(p[i1][0]); verts.push_back(p[i1][1]); verts.push_back(p[i2][0]); verts.push_back(p[i2][1]); };
    if (DIGITS[num][0]) line(0, 1); if (DIGITS[num][1]) line(1, 2); if (DIGITS[num][2]) line(2, 3);
    if (DIGITS[num][3]) line(3, 4); if (DIGITS[num][4]) line(4, 5); if (DIGITS[num][5]) line(5, 0);
    if (DIGITS[num][6]) line(5, 2);
}

void createSphere(unsigned int& vao, unsigned int& idx, float r) {
    std::vector<float> d; std::vector<unsigned int> i;
    int X = 64, Y = 64; float PI = 3.14159f;
    for (int y = 0; y <= Y; y++) for (int x = 0; x <= X; x++) {
        float xS = (float)x / X, yS = (float)y / Y;
        float xP = cos(xS * 2 * PI) * sin(yS * PI), yP = cos(yS * PI), zP = sin(xS * 2 * PI) * sin(yS * PI);
        d.insert(d.end(), { xP * r,yP * r,zP * r, xP,yP,zP, xS,yS });
    }
    for (int y = 0; y < Y; y++) for (int x = 0; x < X; x++) i.insert(i.end(), { (unsigned)((y + 1) * (X + 1) + x), (unsigned)(y * (X + 1) + x), (unsigned)(y * (X + 1) + x + 1), (unsigned)((y + 1) * (X + 1) + x), (unsigned)(y * (X + 1) + x + 1), (unsigned)((y + 1) * (X + 1) + x + 1) });
    idx = i.size(); unsigned int vbo, ebo; glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo); glGenBuffers(1, &ebo);
    glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, d.size() * 4, d.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, i.size() * 4, i.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, 0, 32, (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, 0, 32, (void*)12);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, 0, 32, (void*)24);
}

// --- Main ---
#ifdef _WIN32
// 检测系统是否使用深色模式
bool IsSystemDarkMode() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, 
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD value = 1;
        DWORD size = sizeof(value);
        RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr, (LPBYTE)&value, &size);
        RegCloseKey(hKey);
        return value == 0;  // 0 = 深色, 1 = 浅色
    }
    return true;  // 默认深色
}

// 设置窗口标题栏深色/浅色模式
void SetTitleBarDarkMode(HWND hwnd, bool dark) {
    BOOL useDarkMode = dark ? TRUE : FALSE;
    HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));
    if (FAILED(hr)) {
        DwmSetWindowAttribute(hwnd, 19, &useDarkMode, sizeof(useDarkMode));
    }
}

// 应用 Material You 主题
void ApplyMaterialYouTheme(bool dark) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    style.WindowRounding = 16.0f;
    style.ChildRounding = 12.0f;
    style.FrameRounding = 20.0f;
    style.PopupRounding = 12.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabRounding = 20.0f;
    style.WindowPadding = ImVec2(20, 20);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 12);
    style.WindowBorderSize = 0.0f;

    if (dark) {
        // --- Material You Dark ---
        ImVec4 surface = ImVec4(0.12f, 0.12f, 0.14f, 0.05f);
        ImVec4 cardBg = ImVec4(0.18f, 0.18f, 0.20f, 0.50f);
        ImVec4 buttonBg = ImVec4(0.22f, 0.22f, 0.24f, 1.00f);
        ImVec4 buttonHover = ImVec4(0.28f, 0.28f, 0.30f, 1.00f);
        ImVec4 primary = ImVec4(0.651f, 0.851f, 1.00f, 1.00f);
        ImVec4 primaryDim = ImVec4(0.35f, 0.55f, 0.75f, 1.00f);
        ImVec4 text = ImVec4(0.92f, 0.92f, 0.95f, 1.00f);
        ImVec4 textDim = ImVec4(0.70f, 0.70f, 0.75f, 1.00f);
        ImVec4 outline = ImVec4(0.50f, 0.50f, 0.55f, 0.40f);

        colors[ImGuiCol_WindowBg] = surface;
        colors[ImGuiCol_ChildBg] = cardBg;
        colors[ImGuiCol_PopupBg] = ImVec4(0.15f, 0.15f, 0.17f, 0.98f);
        colors[ImGuiCol_Border] = outline;
        colors[ImGuiCol_FrameBg] = buttonBg;
        colors[ImGuiCol_FrameBgHovered] = buttonHover;
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.32f, 0.32f, 0.35f, 1.00f);
        colors[ImGuiCol_TitleBg] = cardBg;
        colors[ImGuiCol_TitleBgActive] = cardBg;
        colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
        colors[ImGuiCol_ScrollbarGrab] = outline;
        colors[ImGuiCol_ScrollbarGrabHovered] = textDim;
        colors[ImGuiCol_ScrollbarGrabActive] = text;
        colors[ImGuiCol_CheckMark] = primary;
        colors[ImGuiCol_SliderGrab] = primary;
        colors[ImGuiCol_SliderGrabActive] = ImVec4(0.8f, 0.9f, 1.0f, 1.0f);
        colors[ImGuiCol_Button] = buttonBg;
        colors[ImGuiCol_ButtonHovered] = buttonHover;
        colors[ImGuiCol_ButtonActive] = primaryDim;
        colors[ImGuiCol_Text] = text;
        colors[ImGuiCol_TextDisabled] = textDim;
        colors[ImGuiCol_Separator] = outline;
        colors[ImGuiCol_Header] = buttonBg;
        colors[ImGuiCol_HeaderHovered] = buttonHover;
        colors[ImGuiCol_HeaderActive] = primaryDim;
    }
    else {
        // --- Material You Light ---
        ImVec4 surface = ImVec4(0.98f, 0.98f, 0.98f, 0.05f);
        ImVec4 surfaceVar = ImVec4(1.0f, 1.0f, 1.0f, 0.70f);
        ImVec4 primary = ImVec4(0.00f, 0.35f, 0.65f, 1.00f);
        ImVec4 onSurface = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
        ImVec4 onSurfaceVar = ImVec4(0.40f, 0.40f, 0.45f, 1.00f);
        ImVec4 outline = ImVec4(0.50f, 0.50f, 0.50f, 0.20f);

        colors[ImGuiCol_WindowBg] = surface;
        colors[ImGuiCol_ChildBg] = surfaceVar;
        colors[ImGuiCol_PopupBg] = ImVec4(1.00f, 1.00f, 1.00f, 0.98f);
        colors[ImGuiCol_Border] = outline;
        colors[ImGuiCol_FrameBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.05f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.0f, 0.0f, 0.0f, 0.08f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.0f, 0.0f, 0.0f, 0.12f);
        colors[ImGuiCol_CheckMark] = primary;
        colors[ImGuiCol_SliderGrab] = primary;
        colors[ImGuiCol_SliderGrabActive] = ImVec4(0.2f, 0.5f, 0.8f, 1.0f);
        colors[ImGuiCol_Button] = ImVec4(0.0f, 0.0f, 0.0f, 0.05f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.0f, 0.0f, 0.0f, 0.08f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.0f, 0.0f, 0.0f, 0.12f);
        colors[ImGuiCol_Text] = onSurface;
        colors[ImGuiCol_TextDisabled] = onSurfaceVar;
        colors[ImGuiCol_Separator] = outline;
        colors[ImGuiCol_Header] = ImVec4(0.0f, 0.0f, 0.0f, 0.05f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.0f, 0.0f, 0.0f, 0.08f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.0f, 0.0f, 0.0f, 0.12f);
        colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
        colors[ImGuiCol_TitleBg] = surfaceVar;
        colors[ImGuiCol_TitleBgActive] = surfaceVar;
    }
}

// MD3 风格 Toggle 控件
bool ToggleMD3(const char* label, bool* v, float dt) {
    using namespace ImGui;
    ImGuiID id = GetID(label);
    if (g_animStates.find(id) == g_animStates.end()) {
        g_animStates[id].active = *v;
        g_animStates[id].bgOpacity.val = *v ? 1.0f : 0.0f;
        g_animStates[id].knobPos.val = *v ? 1.0f : 0.0f;
    }
    UIAnimState& s = g_animStates[id];
    s.bgOpacity.target = *v ? 1.0f : 0.0f;
    s.knobPos.target = *v ? 1.0f : 0.0f;
    bool hovered = IsItemHovered();
    s.knobSize.target = (*v || hovered) ? 1.0f : 0.0f;
    s.bgOpacity.Update(dt, 18.0f);
    s.knobPos.Update(dt, 14.0f);
    s.knobSize.Update(dt, 20.0f);

    float height = 28.0f * g_dpiScale;
    float width = 52.0f * g_dpiScale;
    ImVec2 p = GetCursorScreenPos();
    ImDrawList* dl = GetWindowDrawList();

    bool pressed = InvisibleButton(label, ImVec2(width, height));
    if (pressed) { *v = !*v; }

    ImVec4 cTrackOff = GetStyle().Colors[ImGuiCol_FrameBg];
    ImVec4 cTrackOn = GetStyle().Colors[ImGuiCol_CheckMark];
    ImVec4 cThumbOff = GetStyle().Colors[ImGuiCol_TextDisabled];
    ImVec4 cThumbOn = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    if (GetStyle().Colors[ImGuiCol_WindowBg].x > 0.5f) cThumbOn = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

    float t = s.bgOpacity.val;
    ImVec4 cTrack = ImVec4(
        cTrackOff.x + (cTrackOn.x - cTrackOff.x) * t,
        cTrackOff.y + (cTrackOn.y - cTrackOff.y) * t,
        cTrackOff.z + (cTrackOn.z - cTrackOff.z) * t,
        cTrackOff.w + (cTrackOn.w - cTrackOff.w) * t
    );

    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), GetColorU32(cTrack), height * 0.5f);
    if (t < 0.95f) {
        ImU32 borderColor = GetColorU32(ImGuiCol_Border);
        dl->AddRect(p, ImVec2(p.x + width, p.y + height), borderColor, height * 0.5f, 0, 1.0f);
    }

    float r_normal = height * 0.25f;
    float r_active = height * 0.38f;
    float r_cur = r_normal + (r_active - r_normal) * s.knobSize.val;
    float pad = height * 0.15f;
    float x_start = p.x + pad + r_active;
    float x_end = p.x + width - pad - r_active;
    float x_cur = x_start + (x_end - x_start) * s.knobPos.val;

    ImU32 colThumb = GetColorU32((s.knobPos.val > 0.5f) ? cThumbOn : cThumbOff);
    dl->AddCircleFilled(ImVec2(x_cur, p.y + height * 0.5f), r_cur, colThumb);

    SameLine(); SetCursorPosX(GetCursorPosX() + 10.0f); Text("%s", label);
    return pressed;
}

// 尝试启用 Acrylic/Mica 效果
// Acrylic (3) 兼容性更好，Mica (2/4) 仅 Windows 11 22H2+
void TryEnableMicaEffect(HWND hwnd) {
    std::cout << "[DWM] Attempting backdrop effect..." << std::endl;

    // 扩展边框到客户区 - 这是让 DWM 背景透过来的关键
    MARGINS margins = { -1, -1, -1, -1 };
    HRESULT hr = DwmExtendFrameIntoClientArea(hwnd, &margins);
    std::cout << "[DWM] ExtendFrameIntoClientArea: " << (SUCCEEDED(hr) ? "OK" : "FAILED") << std::endl;

    // 优先尝试 Acrylic (3)，兼容性更好
    // DWMSBT_TRANSIENTWINDOW (3) = Acrylic
    // DWMSBT_MAINWINDOW (2) = Mica
    // DWMSBT_TABBEDWINDOW (4) = Mica Alt
    int backdropType = 3;  // Acrylic
    hr = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
    std::cout << "[DWM] SystemBackdropType (Acrylic = 3): " << (SUCCEEDED(hr) ? "OK" : "FAILED") << std::endl;

    // 如果 Acrylic 失败，尝试 Mica Alt
    if (FAILED(hr)) {
        backdropType = 4;
        hr = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
        std::cout << "[DWM] SystemBackdropType (Mica Alt = 4): " << (SUCCEEDED(hr) ? "OK" : "FAILED") << std::endl;
    }
}

// 检测系统支持的背景效果
void DetectAvailableBackdrops(HWND hwnd) {
    g_availableBackdrops.clear();
    g_availableBackdrops.push_back(0);  // 纯黑始终可用
    
    // 先扩展边框，这是 DWM 背景效果的前提
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    
    // 测试 Acrylic (DWMSBT_TRANSIENTWINDOW = 3)
    int backdropType = 3;
    HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
    if (SUCCEEDED(hr)) {
        g_availableBackdrops.push_back(1);
        std::cout << "[DWM] Acrylic: Supported" << std::endl;
    } else {
        std::cout << "[DWM] Acrylic: Not supported (0x" << std::hex << hr << std::dec << ")" << std::endl;
    }
    
    // 测试 Mica (DWMSBT_MAINWINDOW = 2)
    backdropType = 2;
    hr = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
    if (SUCCEEDED(hr)) {
        g_availableBackdrops.push_back(2);
        std::cout << "[DWM] Mica: Supported" << std::endl;
    } else {
        std::cout << "[DWM] Mica: Not supported (0x" << std::hex << hr << std::dec << ")" << std::endl;
    }
    
    // 重置
    backdropType = 1;
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
    margins = { 0, 0, 0, 0 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    
    std::cout << "[DWM] Available backdrops: ";
    for (int m : g_availableBackdrops) {
        const char* names[] = { "Black", "Acrylic", "Mica" };
        std::cout << names[m] << " ";
    }
    std::cout << std::endl;
}

// 设置背景模式: 0=纯黑(无DWM), 1=Acrylic, 2=Mica
void SetBackdropMode(HWND hwnd, int mode) {
    // 先重置 DWM 状态
    int resetType = 1;  // DWMSBT_NONE
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &resetType, sizeof(resetType));
    
    if (mode == 0) {
        // 纯黑模式：禁用 DWM 背景
        MARGINS margins = { 0, 0, 0, 0 };
        DwmExtendFrameIntoClientArea(hwnd, &margins);
        g_useTransparent = false;
        std::cout << "[DWM] Backdrop: Solid Black" << std::endl;
    } else {
        // 透明模式：启用 DWM 背景
        MARGINS margins = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(hwnd, &margins);
        
        // Acrylic=3, Mica=2, Mica Alt=4
        // 尝试 Mica (2) 而不是 Mica Alt (4)，可能兼容性更好
        int backdropType = (mode == 1) ? 3 : 2;
        HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
        g_useTransparent = true;
        
        const char* name = (mode == 1) ? "Acrylic" : "Mica";
        std::cout << "[DWM] Backdrop: " << name << " (type=" << backdropType << ") " << (SUCCEEDED(hr) ? "OK" : "FAILED") << std::endl;
    }
    
    // 强制窗口重绘
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
}

// 切换全屏
void ToggleFullscreen(GLFWwindow* window) {
    if (!g_isFullscreen) {
        // 保存当前窗口位置和尺寸
        glfwGetWindowPos(window, &g_windowedX, &g_windowedY);
        glfwGetWindowSize(window, &g_windowedW, &g_windowedH);
        
        // 获取主显示器并切换到全屏
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        g_isFullscreen = true;
        std::cout << "[Window] Fullscreen: " << mode->width << "x" << mode->height << std::endl;
    } else {
        // 恢复窗口模式
        glfwSetWindowMonitor(window, nullptr, g_windowedX, g_windowedY, g_windowedW, g_windowedH, 0);
        g_isFullscreen = false;
        std::cout << "[Window] Windowed: " << g_windowedW << "x" << g_windowedH << std::endl;
    }
}

// 窗口子类化：监听系统主题变化
static WNDPROC g_originalWndProc = nullptr;
static HWND g_mainHwnd = nullptr;

LRESULT CALLBACK ThemeAwareWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_SETTINGCHANGE) {
        // 检查是否是主题相关的设置变化
        if (lParam && wcscmp((LPCWSTR)lParam, L"ImmersiveColorSet") == 0) {
            bool newDarkMode = IsSystemDarkMode();
            if (newDarkMode != g_isDarkMode) {
                g_isDarkMode = newDarkMode;
                // 只更新 ImGui 主题，窗口标题栏始终保持深色（适合星空渲染）
                ApplyMaterialYouTheme(g_isDarkMode);
                std::cout << "[Main] ImGui theme changed: " << (g_isDarkMode ? "Dark" : "Light") << std::endl;
            }
        }
    }
    return CallWindowProcW(g_originalWndProc, hwnd, msg, wParam, lParam);
}

void InstallThemeChangeHook(HWND hwnd) {
    g_mainHwnd = hwnd;
    g_originalWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)ThemeAwareWndProc);
    if (g_originalWndProc) {
        std::cout << "[Main] Theme change hook installed" << std::endl;
    }
}
#endif

int main() {
    // 重定向 std::cout 到调试日志系统
    static DebugStreamBuf debugBuf(std::cout.rdbuf());
    std::cout.rdbuf(&debugBuf);
    
    std::cout << "[Main] Particle Saturn starting..." << std::endl;

    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE); glfwWindowHint(GLFW_SAMPLES, 4);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);  // 启用透明帧缓冲
    GLFWwindow* w = glfwCreateWindow(INIT_WIDTH, INIT_HEIGHT, "Particle Saturn", NULL, NULL);
    glfwMakeContextCurrent(w); gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
    glfwSetFramebufferSizeCallback(w, framebuffer_size_callback);
    
#ifdef _WIN32
    // 禁用 IME 输入法，防止按键时切换输入法
    ImmAssociateContext(glfwGetWin32Window(w), NULL);
#endif

#ifdef _WIN32
    HWND hwnd = glfwGetWin32Window(w);
    if (hwnd) {
        // 窗口标题栏始终使用深色模式（适合星空渲染）
        SetTitleBarDarkMode(hwnd, true);
        
        // 检测系统主题（仅用于 ImGui）
        g_isDarkMode = IsSystemDarkMode();
        std::cout << "[DWM] System theme: " << (g_isDarkMode ? "Dark" : "Light") << " (window forced dark)" << std::endl;
        
        // 安装主题变化监听（仅更新 ImGui）
        InstallThemeChangeHook(hwnd);
        
        // 检测系统支持的背景效果
        DetectAvailableBackdrops(hwnd);
        
        // 默认使用最高级的可用效果（Mica > Acrylic > 纯黑）
        g_backdropIndex = (int)g_availableBackdrops.size() - 1;
        SetBackdropMode(hwnd, g_availableBackdrops[g_backdropIndex]);
    }
#endif

    // --- Init HandTracker ---
#ifdef EMBED_MODELS
    // Load models from embedded resources (Release_Static build)
    std::cout << "[Main] Loading embedded models..." << std::endl;
    HRSRC hPalmRes = FindResource(NULL, MAKEINTRESOURCE(IDR_PALM_MODEL), RT_RCDATA);
    HRSRC hHandRes = FindResource(NULL, MAKEINTRESOURCE(IDR_HAND_MODEL), RT_RCDATA);
    if (hPalmRes && hHandRes) {
        HGLOBAL hPalmData = LoadResource(NULL, hPalmRes);
        HGLOBAL hHandData = LoadResource(NULL, hHandRes);
        if (hPalmData && hHandData) {
            const void* palmData = LockResource(hPalmData);
            const void* handData = LockResource(hHandData);
            DWORD palmSize = SizeofResource(NULL, hPalmRes);
            DWORD handSize = SizeofResource(NULL, hHandRes);
            SetEmbeddedModels(palmData, palmSize, handData, handSize);
            std::cout << "[Main] Embedded models loaded: palm=" << palmSize << " hand=" << handSize << std::endl;
        }
    }
    if (!InitTracker(0, nullptr)) {
        std::cerr << "[Main] Warning: Failed to initialize HandTracker with embedded models" << std::endl;
    } else {
        std::cout << "[Main] HandTracker initialized successfully." << std::endl;
    }
#else
    // Load models from files (Release build with DLL)
    std::cout << "[Main] Initializing HandTracker..." << std::endl;
    if (!InitTracker(0, ".")) {
        std::cerr << "[Main] Warning: Failed to initialize HandTracker DLL. (Check if DLL/Models exist?)" << std::endl;
    } else {
        std::cout << "[Main] HandTracker initialized successfully." << std::endl;
    }
#endif

    // --- Init Dear ImGui ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // 禁用 imgui.ini 保存
    
    // 高 DPI 缩放
    float xscale, yscale;
    glfwGetWindowContentScale(w, &xscale, &yscale);
    g_dpiScale = (xscale > yscale) ? xscale : yscale;
    if (g_dpiScale < 1.0f) g_dpiScale = 1.0f;
    
    // 加载系统字体（更现代的外观）
    ImFontConfig fontCfg;
    fontCfg.OversampleH = 2;
    fontCfg.OversampleV = 2;
    float fontSize = 16.0f * g_dpiScale;
    
    // 按优先级尝试加载字体
    const char* fontPaths[] = {
        "C:\\Windows\\Fonts\\CascadiaCode.ttf",   // Cascadia Code（需安装）
        "C:\\Windows\\Fonts\\CascadiaMono.ttf",   // Cascadia Mono 变体
        "C:\\Windows\\Fonts\\consola.ttf",        // Consolas
        "C:\\Windows\\Fonts\\msyh.ttc",           // Microsoft YaHei UI
        "C:\\Windows\\Fonts\\arial.ttf",          // Arial
        nullptr
    };
    ImFont* font = nullptr;
    for (int i = 0; fontPaths[i] && !font; i++) {
        font = io.Fonts->AddFontFromFileTTF(fontPaths[i], fontSize, &fontCfg);
        if (font) {
            std::cout << "[Main] Font loaded: " << fontPaths[i] << std::endl;
        }
    }
    if (!font) {
        fontCfg.SizePixels = fontSize;
        io.Fonts->AddFontDefault(&fontCfg);
        std::cout << "[Main] Using default font" << std::endl;
    }
    // 检测系统主题并应用 Material You 主题
#ifdef _WIN32
    g_isDarkMode = IsSystemDarkMode();
#endif
    ApplyMaterialYouTheme(g_isDarkMode);
    
    // DPI 缩放
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(g_dpiScale);
    std::cout << "[Main] DPI scale: " << g_dpiScale << std::endl;
    std::cout << "[Main] Theme: " << (g_isDarkMode ? "Dark" : "Light") << std::endl;
    
    ImGui_ImplGlfw_InitForOpenGL(w, true);
    ImGui_ImplOpenGL3_Init("#version 430");
    std::cout << "[Main] Dear ImGui initialized." << std::endl;

    // Init Shaders
    unsigned int pSaturn = createProg(vsSaturn, fsSaturn);
    unsigned int pStar = createProg(vsStar, fsStar);
    unsigned int pPlanet = createProg(vsPlanet, fsPlanet);
    unsigned int pUI = createProg(vsUI, fsUI);
    unsigned int pQuad = createProg(vsQuad, fsQuad);  // 全屏四边形 shader
    unsigned int pBlur = createProg(vsQuad, fsBlur);  // 模糊 shader
    unsigned int cs = glCreateShader(GL_COMPUTE_SHADER); glShaderSource(cs, 1, &csSaturn, 0); glCompileShader(cs);
    unsigned int pComp = glCreateProgram(); glAttachShader(pComp, cs); glLinkProgram(pComp);
    
    // --- FBO for offscreen rendering (black background, then composite to transparent window) ---
    unsigned int fbo, fboTex, rbo;
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &fboTex);
    glGenRenderbuffers(1, &rbo);
    
    auto resizeFBO = [&](int width, int height) {
        glBindTexture(GL_TEXTURE_2D, fboTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
        
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fboTex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    };
    resizeFBO(g_scrWidth, g_scrHeight);
    
    // --- 模糊用 FBO ---
    BlurFramebuffer fboBlur1, fboBlur2;
    fboBlur1.Init(g_scrWidth / 6, g_scrHeight / 6);
    fboBlur2.Init(g_scrWidth / 6, g_scrHeight / 6);
    
    // --- Fullscreen quad VAO ---
    unsigned int vaoQuad, vboQuad;
    float quadVerts[] = { -1,-1, 1,-1, -1,1, 1,1 };
    glGenVertexArrays(1, &vaoQuad); glGenBuffers(1, &vboQuad);
    glBindVertexArray(vaoQuad);
    glBindBuffer(GL_ARRAY_BUFFER, vboQuad);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), 0);
    glBindVertexArray(0);

    // --- Init Particles ---
    struct TP { float x, y, z, r, g, b, s, o, v, ring; };
    std::vector<TP> tP; tP.reserve(MAX_PARTICLES);
    std::default_random_engine g; std::uniform_real_distribution<float> rnd(0, 1);
    std::vector<glm::vec3> cols = { hexToRGB(0xE3DAC5), hexToRGB(0xC9A070), hexToRGB(0xE3DAC5), hexToRGB(0xB08D55) };
    float R = 18.0f;

    for (int i = 0; i < MAX_PARTICLES; i++) {
        TP p;
        if (i < MAX_PARTICLES * 0.25) {
            float th = 6.283f * rnd(g), ph = acos(2 * rnd(g) - 1);
            p.x = R * sin(ph) * cos(th); p.y = R * cos(ph) * 0.9f; p.z = R * sin(ph) * sin(th);
            float lat = (p.y / 0.9f / R + 1) * 0.5f; int ci = (int)(lat * 4 + cos(lat * 40) * 0.8 + cos(lat * 15) * 0.4) % 4; if (ci < 0)ci = 0;
            p.r = cols[ci].x; p.g = cols[ci].y; p.b = cols[ci].z; p.s = 1 + rnd(g) * 0.8f; p.o = 0.8f; p.v = 0; p.ring = 0;
        }
        else {
            float z = rnd(g), rad; glm::vec3 c;
            if (z < 0.15) { rad = R * (1.235f + rnd(g) * 0.29f); c = hexToRGB(0x2A2520); p.s = 0.5f; p.o = 0.3f; }
            else if (z < 0.65) { float t = rnd(g); rad = R * (1.525f + t * 0.425f); c = mix(hexToRGB(0xCDBFA0), hexToRGB(0xDCCBBA), t); p.s = 0.8f + rnd(g) * 0.6f; p.o = 0.85f; if (sin(rad * 2) > 0.8)p.o *= 1.2; }
            else if (z < 0.69) { rad = R * (1.95f + rnd(g) * 0.075f); c = hexToRGB(0x050505); p.s = 0.3f; p.o = 0.1f; }
            else if (z < 0.99) { rad = R * (2.025f + rnd(g) * 0.245f); c = hexToRGB(0x989085); p.s = 0.7f; p.o = 0.6f; if (rad > R * 2.2 && rad < R * 2.21)p.o = 0.1; }
            else { rad = R * (2.32f + rnd(g) * 0.02f); c = hexToRGB(0xAFAFA0); p.s = 1.0f; p.o = 0.7f; }
            float th = rnd(g) * 6.283f; p.x = rad * cos(th); p.z = rad * sin(th); p.y = (rnd(g) - 0.5f) * ((rad > R * 2.3) ? 0.4f : 0.15f);
            p.r = c.x; p.g = c.y; p.b = c.z; p.v = 8.0f / sqrt(rad); p.ring = 1;
        }
        tP.push_back(p);
    }
    for (int i = MAX_PARTICLES - 1; i > 0; i--) std::swap(tP[i], tP[std::uniform_int_distribution<int>(0, i)(g)]);

    struct GP { glm::vec4 p, c, v; float isR, pad[3]; };
    std::vector<GP> gP(MAX_PARTICLES);
    for (int i = 0; i < MAX_PARTICLES; i++) gP[i] = { {tP[i].x,tP[i].y,tP[i].z,tP[i].s}, {tP[i].r,tP[i].g,tP[i].b,tP[i].o}, {0,0,0,tP[i].v}, tP[i].ring };

    unsigned int ssbo, vaoP;
    glGenBuffers(1, &ssbo); glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, gP.size() * sizeof(GP), gP.data(), GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);
    glGenVertexArrays(1, &vaoP); glBindVertexArray(vaoP); glBindBuffer(GL_ARRAY_BUFFER, ssbo);
    for (int i = 0; i < 4; i++) { glEnableVertexAttribArray(i); glVertexAttribPointer(i, (i == 3 ? 1 : 4), GL_FLOAT, 0, sizeof(GP), (void*)(i * 16)); }

    unsigned int vaoS, vboS; glGenVertexArrays(1, &vaoS); glGenBuffers(1, &vboS); glBindVertexArray(vaoS);
    std::vector<float> sD;
    for (int i = 0; i < 50000; i++) {
        float r = 400 + rnd(g) * 3000, th = rnd(g) * 6.28, ph = acos(2 * rnd(g) - 1);
        glm::vec3 c = cols[i % 4];
        sD.insert(sD.end(), { r * sin(ph) * cos(th), r * cos(ph), r * sin(ph) * sin(th), c.x,c.y,c.z, 1 + rnd(g) * 3 });
    }
    glBindBuffer(GL_ARRAY_BUFFER, vboS); glBufferData(GL_ARRAY_BUFFER, sD.size() * 4, sD.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, 0, 28, 0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, 0, 28, (void*)12);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 1, GL_FLOAT, 0, 28, (void*)24);

    unsigned int vaoPl, idxPl; createSphere(vaoPl, idxPl, 1.0f);
    unsigned int vaoUI, vboUI; glGenVertexArrays(1, &vaoUI); glGenBuffers(1, &vboUI);
    // 预配置 UI VAO
    glBindVertexArray(vaoUI); glBindBuffer(GL_ARRAY_BUFFER, vboUI);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), 0);
    glBindVertexArray(0);

    glEnable(GL_BLEND);
    glEnable(GL_PROGRAM_POINT_SIZE); glDepthMask(GL_FALSE);
    // 注意：渲染到 FBO 时用 additive，渲染到屏幕时用 premultiplied alpha

    // --- 初始化 Uniform 缓存 ---
    UniformCache uc;
    initUniformCache(uc, pComp, pSaturn, pStar, pPlanet, pUI);

    // --- 矩阵（会在窗口大小变化时更新） ---
    glm::mat4 proj = glm::perspective(1.047f, (float)g_scrWidth / g_scrHeight, 1.f, 10000.f);
    glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 100), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    glm::mat4 projUI = glm::ortho(0.0f, (float)g_scrWidth, 0.0f, (float)g_scrHeight);

    // --- 预分配 UI 顶点缓冲 ---
    std::vector<float> uiVerts;
    uiVerts.reserve(256); // 预分配足够空间

    // --- Loop Variables ---
    float lastFrame = 0, autoTime = 0;
    int frameCount = 0; float lastFpsTime = 0;
    float currentFps = 60.0f;
    int lastDisplayedFps = 60; // 缓存上次显示的FPS，避免每帧重建UI

    while (!glfwWindowShouldClose(w)) {
        float t = (float)glfwGetTime();
        float dt = t - lastFrame;
        lastFrame = t;

        // --- 窗口大小变化时更新投影矩阵和 FBO ---
        if (g_windowResized) {
            g_windowResized = false;
            proj = glm::perspective(1.047f, (float)g_scrWidth / g_scrHeight, 1.f, 10000.f);
            projUI = glm::ortho(0.0f, (float)g_scrWidth, 0.0f, (float)g_scrHeight);
            lastDisplayedFps = -1; // 强制重建UI几何体
            resizeFBO(g_scrWidth, g_scrHeight);  // 重建 FBO
            fboBlur1.Init(g_scrWidth / 6, g_scrHeight / 6);
            fboBlur2.Init(g_scrWidth / 6, g_scrHeight / 6);
        }

        // --- Poll Hand Data from DLL ---
        bool hasNewData = GetHandData(&handState.scale, &handState.rotX, &handState.rotY, &handState.hasHand);
        // ��� DLL �����Ѹ��£�handState ���Զ���������ֵ

        // 1. ��̬����ƽ��
        frameCount++;
        if (t - lastFpsTime >= 0.5f) {
            currentFps = frameCount / (t - lastFpsTime);
            frameCount = 0; lastFpsTime = t;
            if (currentFps < 45.0f) {
                // 低性能：先减粒子数，再减像素比例
                if (activeParticleCount > MIN_PARTICLES) activeParticleCount = (unsigned int)(activeParticleCount * 0.9f);
                else if (currentPixelRatio > 0.7f) currentPixelRatio -= 0.05f;
            }
            else if (currentFps > 58.0f) {
                // 高性能：只增加粒子数，不增加像素比例（避免越来越亮）
                if (currentPixelRatio < 1.0f) currentPixelRatio += 0.05f;  // 只恢复到1.0，不超过
                else if (activeParticleCount < MAX_PARTICLES) activeParticleCount = (unsigned int)(activeParticleCount * 1.1f);
            }
        }

        // 2. ƽ�������߼�
        float targetScale, targetRotX, targetRotY;

        if (!handState.hasHand) { // �����ƣ��Զ���ʾ
            autoTime += 0.005f;
            targetScale = 1.0f + sin(autoTime) * 0.2f;
            targetRotX = 0.4f + sin(autoTime * 0.3f) * 0.15f;
            targetRotY = 0.0f;
        }
        else {
            // �����ƣ��ֿ�ģʽ
            // HandTracker.cpp ����� rotX/rotY �� 0.0~1.0 �Ĺ�һ������
            // ԭ�߼���
            // rotX (Pitch) = -0.6f + rotY * 1.6f  (��ֱ������Ƹ���)
            // rotY (Yaw)   = (rotX - 0.5f) * 2.0f (ˮƽ���������ת)
            targetScale = handState.scale;
            targetRotX = -0.6f + handState.rotY * 1.6f;
            targetRotY = (handState.rotX - 0.5f) * 2.0f;
        }

        // 注意：HandTracker DLL 内部已经做了平滑 (LERP_FACTOR=0.15)
        // 这里只对自动演示模式做平滑，手势控制直接使用 DLL 输出
        if (!handState.hasHand) {
            // 自动演示模式：需要平滑
            float lerpFactor = 0.08f;
            currentAnim.scale = lerp(currentAnim.scale, targetScale, lerpFactor);
            currentAnim.rotX = lerp(currentAnim.rotX, targetRotX, lerpFactor);
            currentAnim.rotY = lerp(currentAnim.rotY, targetRotY, lerpFactor);
        } else {
            // 手势控制模式：直接使用目标值（DLL已平滑）
            currentAnim.scale = targetScale;
            currentAnim.rotX = targetRotX;
            currentAnim.rotY = targetRotY;
        }

        // 3. Compute Physics - 使用缓存的 uniform location
        glUseProgram(pComp);
        glUniform1f(uc.comp_uDt, dt);
        glUniform1f(uc.comp_uHandScale, currentAnim.scale);
        glUniform1f(uc.comp_uHandHas, handState.hasHand ? 1.0f : 0.0f);
        glUniform1ui(uc.comp_uParticleCount, activeParticleCount);
        glDispatchCompute((activeParticleCount + 255) / 256, 1, 1);  // 匹配新的工作组大小
        glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);

        // 4. Render to FBO (black background, additive blending)
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glClearColor(0, 0, 0, 1);  // 黑色背景
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);  // Additive blending for glow effect

        glm::mat4 mSat = glm::mat4(1.f);
        mSat = glm::rotate(mSat, currentAnim.rotX, glm::vec3(1, 0, 0));
        mSat = glm::rotate(mSat, currentAnim.rotY, glm::vec3(0, 1, 0));
        mSat = glm::rotate(mSat, 0.466f, glm::vec3(0, 0, 1));

        // Stars - 使用缓存的 uniform location
        glUseProgram(pStar);
        glUniformMatrix4fv(uc.star_proj, 1, 0, &proj[0][0]);
        glUniformMatrix4fv(uc.star_view, 1, 0, &view[0][0]);
        glm::mat4 mStar = glm::rotate(glm::mat4(1.f), t * 0.005f, glm::vec3(0, 1, 0));
        glUniformMatrix4fv(uc.star_model, 1, 0, &mStar[0][0]);
        glUniform1f(uc.star_uTime, t);
        glBindVertexArray(vaoS); glDrawArrays(GL_POINTS, 0, 50000);

        // Saturn - 使用缓存的 uniform location
        glUseProgram(pSaturn);
        glUniformMatrix4fv(uc.sat_proj, 1, 0, &proj[0][0]);
        glUniformMatrix4fv(uc.sat_view, 1, 0, &view[0][0]);
        glUniformMatrix4fv(uc.sat_model, 1, 0, &mSat[0][0]);
        glUniform1f(uc.sat_uTime, t);
        glUniform1f(uc.sat_uScale, currentAnim.scale);
        glUniform1f(uc.sat_uPixelRatio, currentPixelRatio);
        float ratio = (float)activeParticleCount / MAX_PARTICLES;
        // 密度补偿需要同时考虑粒子数和像素比例，避免高性能设备上过亮
        float densityComp = 0.6f / pow(ratio, 0.7f) / pow(currentPixelRatio, 0.5f);
        glUniform1f(uc.sat_uDensityComp, densityComp);
        glUniform1f(uc.sat_uScreenHeight, (float)g_scrHeight);
        glBindVertexArray(vaoP); glDrawArrays(GL_POINTS, 0, activeParticleCount);

        // Planets - 使用缓存的 uniform location
        glDepthMask(GL_TRUE); glEnable(GL_DEPTH_TEST);
        glUseProgram(pPlanet);
        glUniformMatrix4fv(uc.pl_p, 1, 0, &proj[0][0]);
        glUniformMatrix4fv(uc.pl_v, 1, 0, &view[0][0]);
        glUniform3f(uc.pl_ld, 1, .5, 1);
        glBindVertexArray(vaoPl); // 只绑定一次VAO
        auto drawPl = [&](glm::vec3 p, float r, glm::vec3 c1, glm::vec3 c2, float ns, float at) {
            glm::mat4 m = glm::rotate(glm::mat4(1.f), t * 0.02f, glm::vec3(0, 1, 0));
            m = glm::translate(m, p); m = glm::rotate(m, t * 0.1f, glm::vec3(0, 1, 0)); m = glm::scale(m, glm::vec3(r));
            glUniformMatrix4fv(uc.pl_m, 1, 0, &m[0][0]);
            glUniform3f(uc.pl_c1, c1.x, c1.y, c1.z);
            glUniform3f(uc.pl_c2, c2.x, c2.y, c2.z);
            glUniform1f(uc.pl_ns, ns); glUniform1f(uc.pl_at, at);
            glDrawElements(GL_TRIANGLES, idxPl, GL_UNSIGNED_INT, 0);
        };
        drawPl({ -300,120,-450 }, 10, hexToRGB(0xb33a00), hexToRGB(0xd16830), 8, .3f);
        drawPl({ 380,-100,-600 }, 14, hexToRGB(0x001e4d), hexToRGB(0xffffff), 5, .6f);
        drawPl({ -180,-220,-350 }, 6, hexToRGB(0x666666), hexToRGB(0xaaaaaa), 15, .1f);
        glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);

        // UI - 只在FPS变化时重建几何体 (渲染到 FBO)
        int displayFps = (int)currentFps;
        if (displayFps != lastDisplayedFps) {
            lastDisplayedFps = displayFps;
            uiVerts.clear();
            std::string fpsStr = std::to_string(displayFps);
            float xCursor = (float)g_scrWidth - 60.0f;
            float numSize = 20.0f;
            for (int i = (int)fpsStr.length() - 1; i >= 0; i--) {
                addDigitGeometry(uiVerts, xCursor, (float)g_scrHeight - 40, numSize, numSize * 1.8f, fpsStr[i] - '0');
                xCursor -= (numSize + 10.0f);
            }
            glBindBuffer(GL_ARRAY_BUFFER, vboUI);
            glBufferData(GL_ARRAY_BUFFER, uiVerts.size() * sizeof(float), uiVerts.data(), GL_DYNAMIC_DRAW);
        }
        glUseProgram(pUI);
        glUniformMatrix4fv(uc.ui_proj, 1, 0, &projUI[0][0]);
        glm::vec3 fpsCol = (currentFps > 50) ? glm::vec3(0.3, 1.0, 0.3) : ((currentFps > 30) ? glm::vec3(1.0, 0.6, 0.0) : glm::vec3(1.0, 0.2, 0.2));
        glUniform3fv(uc.ui_uColor, 1, &fpsCol[0]);
        glBindVertexArray(vaoUI);
        glLineWidth(2.0f);
        glDrawArrays(GL_LINES, 0, (GLsizei)(uiVerts.size() / 2));

        // 5. 模糊 pass (用于 ImGui 玻璃效果)
        if (g_enableImGuiBlur) {
            glBlendFunc(GL_ONE, GL_ZERO);  // 模糊时禁用混合
            glViewport(0, 0, fboBlur1.w, fboBlur1.h);
            glUseProgram(pBlur);
            GLint locDir = glGetUniformLocation(pBlur, "dir");
            GLint locTex = glGetUniformLocation(pBlur, "uTexture");
            glUniform1i(locTex, 0);
            glActiveTexture(GL_TEXTURE0);

            glBindFramebuffer(GL_FRAMEBUFFER, fboBlur1.fbo);
            glBindTexture(GL_TEXTURE_2D, fboTex);
            glUniform2f(locDir, 1.0f / fboBlur1.w, 0.0f);
            glBindVertexArray(vaoQuad);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            glBindFramebuffer(GL_FRAMEBUFFER, fboBlur2.fbo);
            glBindTexture(GL_TEXTURE_2D, fboBlur1.tex);
            glUniform2f(locDir, 0.0f, 1.0f / fboBlur1.h);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            int iterations = (int)g_blurStrength;
            for (int i = 0; i < iterations; i++) {
                glBindFramebuffer(GL_FRAMEBUFFER, fboBlur1.fbo);
                glBindTexture(GL_TEXTURE_2D, fboBlur2.tex);
                glUniform2f(locDir, 1.0f / fboBlur1.w, 0.0f);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

                glBindFramebuffer(GL_FRAMEBUFFER, fboBlur2.fbo);
                glBindTexture(GL_TEXTURE_2D, fboBlur1.tex);
                glUniform2f(locDir, 0.0f, 1.0f / fboBlur1.h);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
            glViewport(0, 0, g_scrWidth, g_scrHeight);
        }

        // 6. Composite FBO to screen
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (g_useTransparent) {
            glClearColor(0, 0, 0, 0);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        } else {
            glClearColor(0, 0, 0, 1);
            glBlendFunc(GL_ONE, GL_ZERO);
        }
        glClear(GL_COLOR_BUFFER_BIT);
        
        glUseProgram(pQuad);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, fboTex);
        glUniform1i(glGetUniformLocation(pQuad, "uTexture"), 0);
        glUniform1i(glGetUniformLocation(pQuad, "uTransparent"), g_useTransparent ? 1 : 0);
        glBindVertexArray(vaoQuad);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        // 7. ImGui 渲染
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        
        if (g_showDebugWindow) {
            ImGui::SetNextWindowSize(ImVec2(450 * g_dpiScale, 600 * g_dpiScale), ImGuiCond_FirstUseEver);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ResizeGrip, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, ImVec4(0, 0, 0, 0));
            ImGui::Begin("Debug Panel", &g_showDebugWindow, ImGuiWindowFlags_NoCollapse);
            
            // 玻璃背景效果
            ImVec2 pos = ImGui::GetWindowPos();
            ImVec2 size = ImGui::GetWindowSize();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImGuiStyle& style = ImGui::GetStyle();

            if (g_enableImGuiBlur) {
                ImVec2 uv0 = ImVec2(pos.x / g_scrWidth, 1.0f - pos.y / g_scrHeight);
                ImVec2 uv1 = ImVec2((pos.x + size.x) / g_scrWidth, 1.0f - (pos.y + size.y) / g_scrHeight);
                dl->AddImage((ImTextureID)(intptr_t)fboBlur2.tex, pos, ImVec2(pos.x + size.x, pos.y + size.y), uv0, uv1);

                ImU32 tintColor;
                if (g_isDarkMode) tintColor = IM_COL32(20, 20, 25, 180);
                else              tintColor = IM_COL32(245, 245, 255, 150);

                dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), tintColor, style.WindowRounding);

                ImU32 highlight = g_isDarkMode ? IM_COL32(255, 255, 255, 40) : IM_COL32(255, 255, 255, 120);
                dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), highlight, style.WindowRounding, 0, 1.0f);
            }
            else {
                ImVec4 bgCol = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
                bgCol.w = 0.95f;
                dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(bgCol), style.WindowRounding);
            }
            ImGui::PopStyleColor(4);  // 弹出 WindowBg + 3个 ResizeGrip 颜色
            
            // 性能信息
            if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("FPS: %.1f", currentFps);
                ImGui::Text("Particles: %u / %u", activeParticleCount, MAX_PARTICLES);
                ImGui::Text("Pixel Ratio: %.2f", currentPixelRatio);
                ImGui::Text("Resolution: %u x %u", g_scrWidth, g_scrHeight);
            }
            
            // 手部追踪信息
            if (ImGui::CollapsingHeader("Hand Tracking", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("Hand Detected: %s", handState.hasHand ? "Yes" : "No");
                ImGui::Text("Scale: %.3f", handState.scale);
                ImGui::Text("Rot X: %.3f", handState.rotX);
                ImGui::Text("Rot Y: %.3f", handState.rotY);
                ImGui::Separator();
                ImGui::Text("Animation Scale: %.3f", currentAnim.scale);
                ImGui::Text("Animation RotX: %.3f", currentAnim.rotX);
                ImGui::Text("Animation RotY: %.3f", currentAnim.rotY);
                ImGui::Separator();
                bool cameraDebug = GetTrackerDebugMode();
                if (ToggleMD3("Show Camera Debug Window", &cameraDebug, dt)) {
                    SetTrackerDebugMode(cameraDebug);
                    g_showCameraDebug = cameraDebug;
                }
            }
            
            // 视觉设置
            if (ImGui::CollapsingHeader("Visuals")) {
                if (ToggleMD3("Dark Mode", &g_isDarkMode, dt)) {
                    ApplyMaterialYouTheme(g_isDarkMode);
                }
                ImGui::Dummy(ImVec2(0, 5));
                ToggleMD3("Glass Blur", &g_enableImGuiBlur, dt);
                if (g_enableImGuiBlur) {
                    ImGui::Indent(10);
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##BlurStr", &g_blurStrength, 0.0f, 5.0f, "Blur Strength: %.0f");
                    ImGui::Unindent(10);
                }
            }
            
            // 窗口设置
            if (ImGui::CollapsingHeader("Window")) {
                const char* backdropNames[] = { "Solid Black", "Acrylic", "Mica" };
                if (g_backdropIndex < (int)g_availableBackdrops.size()) {
                    ImGui::Text("Backdrop: %s", backdropNames[g_availableBackdrops[g_backdropIndex]]);
                }
                ImGui::Text("Fullscreen: %s", g_isFullscreen ? "Yes" : "No");
                ImGui::Text("Transparent: %s", g_useTransparent ? "Yes" : "No");
            }
            
            // 调试日志
            if (ImGui::CollapsingHeader("Log", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::Button("Clear")) {
                    DebugLog::Instance().Clear();
                }
                ImGui::SameLine();
                if (ImGui::Button("Copy All")) {
                    std::string allText = DebugLog::Instance().GetAllText();
                    ImGui::SetClipboardText(allText.c_str());
                }
                DebugLog::Instance().Draw();
            }
            
            ImGui::End();
        }
        
        // 渲染 ImGui
        ImGui::Render();
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);  // 标准混合模式
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(w); glfwPollEvents();
        

        
        // 按键处理（带防抖）
        static bool keyB_pressed = false, keyF11_pressed = false, keyF3_pressed = false;
        
        // F3 键：切换调试窗口
        if (glfwGetKey(w, GLFW_KEY_F3) == GLFW_PRESS) {
            if (!keyF3_pressed) {
                keyF3_pressed = true;
                g_showDebugWindow = !g_showDebugWindow;
                std::cout << "[Main] Debug window: " << (g_showDebugWindow ? "shown" : "hidden") << std::endl;
            }
        } else keyF3_pressed = false;
        
        // B 键：在可用的背景模式之间切换
        if (glfwGetKey(w, GLFW_KEY_B) == GLFW_PRESS) {
            if (!keyB_pressed) {
                keyB_pressed = true;
#ifdef _WIN32
                if (!g_availableBackdrops.empty()) {
                    g_backdropIndex = (g_backdropIndex + 1) % (int)g_availableBackdrops.size();
                    SetBackdropMode(hwnd, g_availableBackdrops[g_backdropIndex]);
                }
#endif
            }
        } else keyB_pressed = false;
        
        // F11 键：切换全屏
        if (glfwGetKey(w, GLFW_KEY_F11) == GLFW_PRESS) {
            if (!keyF11_pressed) {
                keyF11_pressed = true;
#ifdef _WIN32
                ToggleFullscreen(w);
                // 全屏切换后重新应用 DWM 效果
                if (!g_isFullscreen && g_availableBackdrops[g_backdropIndex] > 0) {
                    SetBackdropMode(hwnd, g_availableBackdrops[g_backdropIndex]);
                }
#endif
            }
        } else keyF11_pressed = false;
        
        if (glfwGetKey(w, GLFW_KEY_ESCAPE)) break;
    }

    // --- Cleanup ---
    std::cout << "[Main] Shutting down..." << std::endl;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    ReleaseTracker();
    glfwTerminate();
    return 0;
}