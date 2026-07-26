#include <metal_stdlib>

using namespace metal;

// 全屏三角形顶点——基础设施样板（与 glsl410/FullscreenTriangle.vert 对应）。
// 单源试点（D-004）只单源化像素阶段算法；顶点样板各后端保留本地实现。
struct FullscreenVertexOut {
    float4 position [[position]];
};

vertex FullscreenVertexOut FullscreenTriangleVertex(uint vertexId [[vertex_id]]) {
    const float2 positions[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    FullscreenVertexOut out;
    out.position = float4(positions[vertexId], 0.0, 1.0);
    return out;
}
