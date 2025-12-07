import onnx
from onnx import helper, TensorProto
import numpy as np

def add_transpose_to_input(model_path, output_path):
    """
    在 ONNX 模型输入后添加 Transpose 层，将 NCHW 输入转换为模型期望的 NHWC
    """
    model = onnx.load(model_path)
    graph = model.graph
    
    # 获取原始输入
    original_input = graph.input[0]
    original_input_name = original_input.name
    
    # 创建新的输入 (NCHW 格式)
    # 原始输入是 [1, 192, 192, 3] (NHWC)
    # 新输入是 [1, 3, 192, 192] (NCHW)
    new_input_name = "input_nchw"
    new_input = helper.make_tensor_value_info(
        new_input_name, 
        TensorProto.FLOAT, 
        [1, 3, 192, 192]  # NCHW
    )
    
    # 创建 Transpose 节点: NCHW -> NHWC
    transpose_output_name = original_input_name + "_transposed"
    transpose_node = helper.make_node(
        'Transpose',
        inputs=[new_input_name],
        outputs=[transpose_output_name],
        perm=[0, 2, 3, 1]  # NCHW -> NHWC
    )
    
    # 更新所有使用原始输入的节点
    for node in graph.node:
        for i, inp in enumerate(node.input):
            if inp == original_input_name:
                node.input[i] = transpose_output_name
    
    # 移除原始输入，添加新输入
    graph.input.remove(original_input)
    graph.input.insert(0, new_input)
    
    # 在图的开头插入 Transpose 节点
    graph.node.insert(0, transpose_node)
    
    # 保存
    onnx.save(model, output_path)
    print(f"Converted: {model_path} -> {output_path}")

def add_transpose_to_landmark(model_path, output_path):
    """
    Hand landmark 模型的转换 (输入是 224x224)
    """
    model = onnx.load(model_path)
    graph = model.graph
    
    original_input = graph.input[0]
    original_input_name = original_input.name
    
    new_input_name = "input_nchw"
    new_input = helper.make_tensor_value_info(
        new_input_name, 
        TensorProto.FLOAT, 
        [1, 3, 224, 224]  # NCHW
    )
    
    transpose_output_name = original_input_name + "_transposed"
    transpose_node = helper.make_node(
        'Transpose',
        inputs=[new_input_name],
        outputs=[transpose_output_name],
        perm=[0, 2, 3, 1]  # NCHW -> NHWC
    )
    
    for node in graph.node:
        for i, inp in enumerate(node.input):
            if inp == original_input_name:
                node.input[i] = transpose_output_name
    
    graph.input.remove(original_input)
    graph.input.insert(0, new_input)
    graph.node.insert(0, transpose_node)
    
    onnx.save(model, output_path)
    print(f"Converted: {model_path} -> {output_path}")

if __name__ == "__main__":
    add_transpose_to_input("palm_detection.onnx", "palm_detection_nchw.onnx")
    add_transpose_to_landmark("hand_landmark.onnx", "hand_landmark_nchw.onnx")
    print("Done!")
