#pragma once

// 渲染器 - OpenGL 渲染工具、FBO 管理、着色器编译

// M_PI 可能未定义 (MSVC 需要 _USE_MATH_DEFINES 在 <cmath> 之前)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// 模糊效果帧缓冲
// 优化: 使用 R11F_G11F_B10F 格式 (4字节/像素) 代替 RGB16F (6字节/像素)
// 对于模糊效果，精度足够，节省 33% 内存带宽
struct BlurFramebuffer {
    GLuint fbo = 0, tex = 0;
    int    w = 0, h = 0;

    void Init(int width, int height) {
        w = width;
        h = height;
        if (fbo) {
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &tex);
        }
        glGenFramebuffers(1, &fbo);
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        // 优化: R11F_G11F_B10F 是紧凑的 HDR 格式，每像素 4 字节
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R11F_G11F_B10F, width, height, 0, GL_RGB, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
};

// Uniform 位置缓存（避免重复查询）
struct UniformCache {
    GLint comp_uDt, comp_uHandScale, comp_uHandHas, comp_uParticleCount;
    GLint sat_proj, sat_view, sat_model, sat_uTime, sat_uScale, sat_uPixelRatio, sat_uDensityComp, sat_uScreenHeight,
        sat_uNoiseTexture;
    GLint star_proj, star_view, star_model, star_uTime;
    GLint ui_proj, ui_uColor, ui_uTransform;
    // 模糊着色器 (Kawase Blur)
    GLint blur_uTexture, blur_uTexelSize, blur_uOffset;
    // Acrylic 合成着色器
    GLint acrylic_uTexture, acrylic_uTint, acrylic_uParams;
    // 全屏四边形着色器
    GLint quad_uTexture, quad_uTransparent;
};

namespace Renderer {

// 声明 (实现在 Renderer.cpp)
unsigned int CreateProgramImpl(const char* vertexSrc, const char* fragmentSrc);
bool         CheckShaderCompileStatus(unsigned int shader, const char* type);
bool         CheckProgramLinkStatus(unsigned int program);

// 着色器缓存管理
void FlushShaderCache(); // 保存缓存到磁盘（程序退出前调用）
void ClearShaderCache(); // 清除所有缓存
bool IsCacheEmpty();     // 检查缓存是否为空（用于判断是否需要编译）

// Compute Shader 程序创建（支持缓存）
unsigned int CreateComputeProgramImpl(const char* computeSrc);

inline unsigned int CreateComputeProgram(const char* computeSrc) {
    return CreateComputeProgramImpl(computeSrc);
}

// 创建着色器程序 (转发到实现)
inline unsigned int CreateProgram(const char* vertexSrc, const char* fragmentSrc) {
    return CreateProgramImpl(vertexSrc, fragmentSrc);
}

// 初始化 Uniform 缓存
inline void InitUniformCache(UniformCache& uc, unsigned int pComp, unsigned int pSaturn, unsigned int pStar,
                             unsigned int pUI, unsigned int pBlur, unsigned int pAcrylic, unsigned int pQuad) {
    uc.comp_uDt            = glGetUniformLocation(pComp, "uDt");
    uc.comp_uHandScale     = glGetUniformLocation(pComp, "uHandScale");
    uc.comp_uHandHas       = glGetUniformLocation(pComp, "uHandHas");
    uc.comp_uParticleCount = glGetUniformLocation(pComp, "uParticleCount");

    uc.sat_proj          = glGetUniformLocation(pSaturn, "projection");
    uc.sat_view          = glGetUniformLocation(pSaturn, "view");
    uc.sat_model         = glGetUniformLocation(pSaturn, "model");
    uc.sat_uTime         = glGetUniformLocation(pSaturn, "uTime");
    uc.sat_uScale        = glGetUniformLocation(pSaturn, "uScale");
    uc.sat_uPixelRatio   = glGetUniformLocation(pSaturn, "uPixelRatio");
    uc.sat_uDensityComp  = glGetUniformLocation(pSaturn, "uDensityComp");
    uc.sat_uScreenHeight = glGetUniformLocation(pSaturn, "uScreenHeight");
    uc.sat_uNoiseTexture = glGetUniformLocation(pSaturn, "uNoiseTexture");

    uc.star_proj  = glGetUniformLocation(pStar, "projection");
    uc.star_view  = glGetUniformLocation(pStar, "view");
    uc.star_model = glGetUniformLocation(pStar, "model");
    uc.star_uTime = glGetUniformLocation(pStar, "uTime");

    uc.ui_proj       = glGetUniformLocation(pUI, "projection");
    uc.ui_uColor     = glGetUniformLocation(pUI, "uColor");
    uc.ui_uTransform = glGetUniformLocation(pUI, "uTransform");

    // 模糊着色器 (Kawase Blur)
    uc.blur_uTexture   = glGetUniformLocation(pBlur, "uTexture");
    uc.blur_uTexelSize = glGetUniformLocation(pBlur, "uTexelSize");
    uc.blur_uOffset    = glGetUniformLocation(pBlur, "uOffset");

    // Acrylic 合成着色器
    uc.acrylic_uTexture = glGetUniformLocation(pAcrylic, "uTexture");
    uc.acrylic_uTint    = glGetUniformLocation(pAcrylic, "uTint");
    uc.acrylic_uParams  = glGetUniformLocation(pAcrylic, "uParams");

    // 全屏四边形着色器
    uc.quad_uTexture     = glGetUniformLocation(pQuad, "uTexture");
    uc.quad_uTransparent = glGetUniformLocation(pQuad, "uTransparent");
}

// 七段数码管数字定义（用于 FPS 显示）
const int DIGITS[10][7] = {{1, 1, 1, 1, 1, 1, 0}, {0, 1, 1, 0, 0, 0, 0}, {1, 1, 0, 1, 1, 0, 1}, {1, 1, 1, 1, 0, 0, 1},
                           {0, 1, 1, 0, 0, 1, 1}, {1, 0, 1, 1, 0, 1, 1}, {1, 0, 1, 1, 1, 1, 1}, {1, 1, 1, 0, 0, 0, 0},
                           {1, 1, 1, 1, 1, 1, 1}, {1, 1, 1, 1, 0, 1, 1}};

// 预生成的数字几何数据 (优化: 避免每帧重建)
struct PrebuiltDigits {
    GLuint vao[10];         // 每个数字一个 VAO
    GLuint vbo[10];         // 每个数字一个 VBO
    int    vertexCount[10]; // 每个数字的顶点数
    bool   initialized = false;

    void Init() {
        if (initialized) {
            return;
        }

        glGenVertexArrays(10, vao);
        glGenBuffers(10, vbo);

        // 标准化坐标 (0,0) 到 (1,1.8)
        float w = 1.0f, h = 1.8f;
        float p[6][2] = {{0, h}, {w, h}, {w, h / 2}, {w, 0}, {0, 0}, {0, h / 2}};

        for (int num = 0; num < 10; num++) {
            std::vector<float> verts;
            auto               line = [&](int i1, int i2) {
                verts.push_back(p[i1][0]);
                verts.push_back(p[i1][1]);
                verts.push_back(p[i2][0]);
                verts.push_back(p[i2][1]);
            };
            if (DIGITS[num][0]) {
                line(0, 1);
            }
            if (DIGITS[num][1]) {
                line(1, 2);
            }
            if (DIGITS[num][2]) {
                line(2, 3);
            }
            if (DIGITS[num][3]) {
                line(3, 4);
            }
            if (DIGITS[num][4]) {
                line(4, 5);
            }
            if (DIGITS[num][5]) {
                line(5, 0);
            }
            if (DIGITS[num][6]) {
                line(5, 2);
            }

            vertexCount[num] = (int)verts.size() / 2;

            glBindVertexArray(vao[num]);
            glBindBuffer(GL_ARRAY_BUFFER, vbo[num]);
            glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), 0);
        }
        glBindVertexArray(0);
        initialized = true;
    }

    void DrawDigit(int num, float x, float y, float size, GLint uTransformLoc) {
        if (num < 0 || num > 9) {
            return;
        }
        // 设置变换: 位置 + 缩放
        glUniform4f(uTransformLoc, x, y, size, size);
        glBindVertexArray(vao[num]);
        glDrawArrays(GL_LINES, 0, vertexCount[num]);
    }
};

inline void AddDigitGeometry(std::vector<float>& verts, float x, float y, float w, float h, int num) {
    if (num < 0 || num > 9) {
        return;
    }
    float p[6][2] = {{x, y + h}, {x + w, y + h}, {x + w, y + h / 2}, {x + w, y}, {x, y}, {x, y + h / 2}};
    auto  line    = [&](int i1, int i2) {
        verts.push_back(p[i1][0]);
        verts.push_back(p[i1][1]);
        verts.push_back(p[i2][0]);
        verts.push_back(p[i2][1]);
    };
    if (DIGITS[num][0]) {
        line(0, 1);
    }
    if (DIGITS[num][1]) {
        line(1, 2);
    }
    if (DIGITS[num][2]) {
        line(2, 3);
    }
    if (DIGITS[num][3]) {
        line(3, 4);
    }
    if (DIGITS[num][4]) {
        line(4, 5);
    }
    if (DIGITS[num][5]) {
        line(5, 0);
    }
    if (DIGITS[num][6]) {
        line(5, 2);
    }
}

// 生成噪声纹理
inline unsigned int GenerateNoiseTexture(int width = 256, int height = 256) {
    std::vector<unsigned char>         data(width * height * 3);
    std::default_random_engine         gen;
    std::uniform_int_distribution<int> rnd(0, 255);

    for (int i = 0; i < width * height * 3; i++) {
        data[i] = (unsigned char)rnd(gen);
    }

    unsigned int tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return tex;
}

} // namespace Renderer
