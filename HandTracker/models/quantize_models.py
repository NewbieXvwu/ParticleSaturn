"""
ONNX 模型量化脚本
将 FP32 模型转换为 FP16，减小约 50% 体积，兼容性好

使用方法:
    pip install onnx onnxruntime onnxconverter-common
    python quantize_models.py
"""

import onnx
from onnxconverter_common import float16
import os

def convert_to_fp16(input_path, output_path):
    """将 ONNX 模型转换为 FP16"""
    print(f"Converting to FP16: {input_path}")
    
    input_size = os.path.getsize(input_path) / 1024
    
    model = onnx.load(input_path)
    model_fp16 = float16.convert_float_to_float16(model, keep_io_types=True)
    onnx.save(model_fp16, output_path)
    
    output_size = os.path.getsize(output_path) / 1024
    reduction = (1 - output_size / input_size) * 100
    
    print(f"  {input_size:.0f} KB -> {output_size:.0f} KB ({reduction:.1f}% reduction)")

if __name__ == "__main__":
    # 切换到脚本所在目录
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)
    print(f"Working directory: {script_dir}")
    
    models = [
        ("palm_detection_nchw.onnx", "palm_detection_nchw_fp16.onnx"),
        ("hand_landmark_nchw.onnx", "hand_landmark_nchw_fp16.onnx"),
    ]
    
    for input_name, output_name in models:
        if os.path.exists(input_name):
            convert_to_fp16(input_name, output_name)
        else:
            print(f"Warning: {input_name} not found")
    
    print("\nDone! Update resources.rc to use the new *_fp16.onnx files.")
