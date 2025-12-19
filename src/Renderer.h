#pragma once

// 渲染器 - OpenGL 渲染工具、FBO 管理、着色器编译

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
    // 行星着色器 (实例化渲染)
    GLint pl_p, pl_v, pl_ld, pl_uFBMTex, pl_uPlanetCount;
    GLuint pl_ubo;  // 行星 UBO
    GLint ui_proj, ui_uColor, ui_uTransform;
    // 模糊着色器 (Kawase Blur)
    GLint blur_uTexture, blur_uTexelSize, blur_uOffset;
    // 全屏四边形着色器
    GLint quad_uTexture, quad_uTransparent;
};

namespace Renderer {

// 创建着色器程序
inline unsigned int CreateProgram(const char* vertexSrc, const char* fragmentSrc) {
    unsigned int program = glCreateProgram();
    unsigned int vs      = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vertexSrc, 0);
    glCompileShader(vs);
    unsigned int fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fragmentSrc, 0);
    glCompileShader(fs);
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

// 初始化 Uniform 缓存
inline void InitUniformCache(UniformCache& uc, unsigned int pComp, unsigned int pSaturn, unsigned int pStar,
                             unsigned int pPlanet, unsigned int pUI, unsigned int pBlur, unsigned int pQuad) {
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

    // 行星着色器 (实例化渲染)
    uc.pl_p            = glGetUniformLocation(pPlanet, "p");
    uc.pl_v            = glGetUniformLocation(pPlanet, "v");
    uc.pl_ld           = glGetUniformLocation(pPlanet, "ld");
    uc.pl_uFBMTex      = glGetUniformLocation(pPlanet, "uFBMTex");
    uc.pl_uPlanetCount = glGetUniformLocation(pPlanet, "uPlanetCount");

    // 创建行星 UBO (使用 GL_DYNAMIC_DRAW 兼容 OpenGL 4.3)
    glGenBuffers(1, &uc.pl_ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, uc.pl_ubo);
    glBufferData(GL_UNIFORM_BUFFER, 8 * sizeof(PlanetInstance), nullptr, GL_DYNAMIC_DRAW);
    // 绑定到 binding point 0
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, uc.pl_ubo);

    uc.ui_proj      = glGetUniformLocation(pUI, "projection");
    uc.ui_uColor    = glGetUniformLocation(pUI, "uColor");
    uc.ui_uTransform = glGetUniformLocation(pUI, "uTransform");

    // 模糊着色器 (Kawase Blur)
    uc.blur_uTexture   = glGetUniformLocation(pBlur, "uTexture");
    uc.blur_uTexelSize = glGetUniformLocation(pBlur, "uTexelSize");
    uc.blur_uOffset    = glGetUniformLocation(pBlur, "uOffset");

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
    GLuint vao[10];           // 每个数字一个 VAO
    GLuint vbo[10];           // 每个数字一个 VBO
    int    vertexCount[10];   // 每个数字的顶点数
    bool   initialized = false;

    void Init() {
        if (initialized) return;

        glGenVertexArrays(10, vao);
        glGenBuffers(10, vbo);

        // 标准化坐标 (0,0) 到 (1,1.8)
        float w = 1.0f, h = 1.8f;
        float p[6][2] = {{0, h}, {w, h}, {w, h/2}, {w, 0}, {0, 0}, {0, h/2}};

        for (int num = 0; num < 10; num++) {
            std::vector<float> verts;
            auto line = [&](int i1, int i2) {
                verts.push_back(p[i1][0]); verts.push_back(p[i1][1]);
                verts.push_back(p[i2][0]); verts.push_back(p[i2][1]);
            };
            if (DIGITS[num][0]) line(0, 1);
            if (DIGITS[num][1]) line(1, 2);
            if (DIGITS[num][2]) line(2, 3);
            if (DIGITS[num][3]) line(3, 4);
            if (DIGITS[num][4]) line(4, 5);
            if (DIGITS[num][5]) line(5, 0);
            if (DIGITS[num][6]) line(5, 2);

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
        if (num < 0 || num > 9) return;
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

// 创建球体网格
inline void CreateSphere(unsigned int& vao, unsigned int& indexCount, float radius) {
    std::vector<float>        data;
    std::vector<unsigned int> indices;
    int                       X = 64, Y = 64;
    float                     PI = 3.14159f;

    for (int y = 0; y <= Y; y++) {
        for (int x = 0; x <= X; x++) {
            float xS = (float)x / X, yS = (float)y / Y;
            float xP = cos(xS * 2 * PI) * sin(yS * PI);
            float yP = cos(yS * PI);
            float zP = sin(xS * 2 * PI) * sin(yS * PI);
            data.insert(data.end(), {xP * radius, yP * radius, zP * radius, xP, yP, zP, xS, yS});
        }
    }

    for (int y = 0; y < Y; y++) {
        for (int x = 0; x < X; x++) {
            indices.insert(indices.end(), {(unsigned)((y + 1) * (X + 1) + x), (unsigned)(y * (X + 1) + x),
                                           (unsigned)(y * (X + 1) + x + 1), (unsigned)((y + 1) * (X + 1) + x),
                                           (unsigned)(y * (X + 1) + x + 1), (unsigned)((y + 1) * (X + 1) + x + 1)});
        }
    }

    indexCount = (unsigned int)indices.size();
    unsigned int vbo, ebo;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, data.size() * 4, data.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * 4, indices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, 0, 32, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, 0, 32, (void*)12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, 0, 32, (void*)24);
}

// 生成噪声纹理
inline unsigned int GenerateNoiseTexture(int width = 256, int height = 256) {
    std::vector<unsigned char> data(width * height * 3);
    std::default_random_engine gen;
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

// 生成 FBM 噪声纹理 (用于行星表面)
inline unsigned int GenerateFBMTexture(int width = 512, int height = 512) {
    // 辅助函数: 2D 哈希噪声
    auto hash = [](float x, float y) -> float {
        return fmodf(sinf(x * 12.9898f + y * 78.233f) * 43758.5453f, 1.0f);
    };

    // 辅助函数: 平滑插值噪声
    auto noise = [&](float x, float y) -> float {
        int   ix = (int)floorf(x);
        int   iy = (int)floorf(y);
        float fx = x - ix;
        float fy = y - iy;
        float ux = fx * fx * (3.0f - 2.0f * fx);
        float uy = fy * fy * (3.0f - 2.0f * fy);

        float a = hash((float)ix, (float)iy);
        float b = hash((float)(ix + 1), (float)iy);
        float c = hash((float)ix, (float)(iy + 1));
        float d = hash((float)(ix + 1), (float)(iy + 1));

        return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
    };

    // FBM: 5 层噪声叠加
    auto fbm = [&](float x, float y) -> float {
        float value = 0.0f;
        float amp   = 0.5f;
        for (int i = 0; i < 5; i++) {
            value += amp * noise(x, y);
            x *= 2.0f;
            y *= 2.0f;
            amp *= 0.5f;
        }
        return value;
    };

    std::vector<unsigned char> data(width * height);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float u     = (float)x / width * 16.0f;
            float v     = (float)y / height * 16.0f;
            float value = fbm(u, v);
            data[y * width + x] = (unsigned char)(value * 255.0f);
        }
    }

    unsigned int tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, data.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glGenerateMipmap(GL_TEXTURE_2D);
    return tex;
}

} // namespace Renderer
