#pragma once
// 预编译头文件 - 包含常用的标准库和第三方库头文件

// Windows 平台相关
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <Windows.h>
#undef NOGDI
#endif

// OpenGL 相关
#include <GLFW/glfw3.h>
#include <glad/glad.h>

// GLM 数学库
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

// ImGui
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

// C++ 标准库
#include <algorithm>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>
