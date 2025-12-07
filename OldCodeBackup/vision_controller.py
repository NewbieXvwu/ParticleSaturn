import cv2
import mediapipe as mp
import math
import socket
import struct
import time

# --- 配置 ---
UDP_IP = "127.0.0.1"
UDP_PORT = 8888
CAMERA_ID = 0

# 【核心优化开关】
# True: 极速模式，不显示窗口，不绘图，只计算和发送数据 (适合正式使用)
# False: 调试模式，显示窗口和骨骼 (适合调试手感)
HEADLESS_MODE = True

# 【性能优化】
# 0: Lite (最快，精度略低)
# 1: Full (默认)
# 2: Heavy (最准，最慢)
MODEL_COMPLEXITY = 1

# --- 初始化 UDP ---
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

# --- 初始化 MediaPipe ---
mp_hands = mp.solutions.hands
# 仅在需要显示时才加载绘图工具
if not HEADLESS_MODE:
    mp_drawing = mp.solutions.drawing_utils

hands = mp_hands.Hands(
    max_num_hands=1,
    model_complexity=MODEL_COMPLEXITY,
    min_detection_confidence=0.7,
    min_tracking_confidence=0.7
)

# --- 打开摄像头 ---
cap = cv2.VideoCapture(CAMERA_ID)

# 【关键优化】：强制降低分辨率
# 即使你的摄像头支持 4K，对于手势识别来说，640x480 足够了，
# 这能极大减少 CPU 转换图像格式（BGR->RGB）的时间。
cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)

print(f"--- 视觉中枢已启动 (Headless: {HEADLESS_MODE}) ---")
print(f"目标地址: {UDP_IP}:{UDP_PORT}")
if not HEADLESS_MODE:
    print("按 'q' 键退出")
else:
    print("按 'Ctrl + C' 在终端中强制退出")

# 平滑处理变量
smooth_scale = 1.0
smooth_rot_x = 0.5
smooth_rot_y = 0.5
LERP_FACTOR = 0.15 #稍微调快一点响应速度

try:
    while cap.isOpened():
        success, image = cap.read()
        if not success:
            print("忽略空帧")
            continue

        # 镜像翻转 (CPU操作，如果极其追求极致性能，可以在接收端反转数据，这里为了逻辑直观保留)
        image = cv2.flip(image, 1)
        
        # 转换颜色空间 BGR -> RGB (必须步骤)
        # 优化：如果是 Headless 模式，不需要再把 writeable 设回来，因为我们不画图
        image.flags.writeable = False
        image_rgb = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
        
        # 推理
        results = hands.process(image_rgb)
        
        # 默认值
        target_scale = 1.0
        target_rot_x = 0.5
        target_rot_y = 0.5
        has_hand = 0.0 

        if results.multi_hand_landmarks:
            has_hand = 1.0
            # 只取第一只手
            hand_landmarks = results.multi_hand_landmarks[0]
            
            # --- 绘图逻辑 (仅在非 Headless 模式下运行) ---
            if not HEADLESS_MODE:
                image.flags.writeable = True
                mp_drawing.draw_landmarks(
                    image, hand_landmarks, mp_hands.HAND_CONNECTIONS)

            # --- 核心逻辑 ---
            wrist = hand_landmarks.landmark[0]
            target_rot_x = wrist.x 
            target_rot_y = wrist.y

            thumb_tip = hand_landmarks.landmark[4]
            index_tip = hand_landmarks.landmark[8]
            
            # 优化：使用 hypot 计算欧几里得距离
            dist = math.hypot(thumb_tip.x - index_tip.x, thumb_tip.y - index_tip.y)
            
            norm_dist = max(0.0, min(1.0, (dist - 0.02) / 0.25))
            target_scale = 0.5 + norm_dist * 2.0

            # --- 文本显示 (仅在非 Headless 模式下运行) ---
            if not HEADLESS_MODE:
                cv2.putText(image, f"S: {target_scale:.2f}", (10, 30), 
                            cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)

        # --- 平滑插值 ---
        smooth_scale += (target_scale - smooth_scale) * LERP_FACTOR
        smooth_rot_x += (target_rot_x - smooth_rot_x) * LERP_FACTOR
        smooth_rot_y += (target_rot_y - smooth_rot_y) * LERP_FACTOR

        # --- 发送 UDP ---
        packet = struct.pack('4f', has_hand, smooth_scale, smooth_rot_x, smooth_rot_y)
        sock.sendto(packet, (UDP_IP, UDP_PORT))

        # --- 窗口显示逻辑 ---
        if not HEADLESS_MODE:
            cv2.imshow('Saturn Controller', image)
            if cv2.waitKey(1) & 0xFF == ord('q'): # waitKey(1) 比 (5) 响应稍微快点
                break
        
        # 如果是无头模式，可以通过捕获 Ctrl+C 退出，或者在这里加一个简单的 sleep 防止 CPU 100% 空转
        # 但通常图像采集本身就有耗时，所以不需要额外的 sleep

except KeyboardInterrupt:
    print("\n用户中断，正在退出...")

finally:
    # 确保资源正确释放
    cap.release()
    if not HEADLESS_MODE:
        cv2.destroyAllWindows()
    print("资源已释放，程序结束。")