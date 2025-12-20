// AppState.cpp - AppState 辅助函数实现

#include "AppState.h"

#include "pch.h"

AppState* GetAppState(GLFWwindow* window) {
    return static_cast<AppState*>(glfwGetWindowUserPointer(window));
}

void SetAppState(GLFWwindow* window, AppState* state) {
    glfwSetWindowUserPointer(window, state);
}
