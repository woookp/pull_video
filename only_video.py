import cv2
import time
from datetime import datetime
import os
import subprocess

# 拉流摄像头
video_capture = cv2.VideoCapture(0)  # 调用摄像头的 RTSP 协议流

# 推流 URL 地址
push_url = "rtsp://192.168.1.121:8554/stream"

# 获取摄像头的宽度、高度和帧率
width = int(video_capture.get(cv2.CAP_PROP_FRAME_WIDTH))
height = int(video_capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
fps = int(video_capture.get(cv2.CAP_PROP_FPS))
print("width:", width, "height:", height, "fps:", fps)

# FFmpeg 推流命令
command = [
    'ffmpeg',
    '-y',
    '-an',
    '-f', 'rawvideo',
    '-vcodec', 'rawvideo',
    '-pix_fmt', 'bgr24',
    '-s', "{}x{}".format(width, height),
    '-r', str(fps),
    '-i', '-',
    '-c:v', 'libx264',
    '-pix_fmt', 'yuv420p',
    '-preset', 'ultrafast',
    '-tune', 'zerolatency',
    '-max_delay', '0',
    '-bufsize', '100k',
    '-f', 'rtsp',
    '-rtsp_transport', 'tcp',
    push_url
]
pipe = subprocess.Popen(command, shell=False, stdin=subprocess.PIPE)

def frame_handler(frame):
    # 运行目标追踪等处理
    return frame

# 创建 VideoWriter 对象以保存本地视频
current_time1 = datetime.now().strftime("%Y%m%d_%H%M%S")
fourcc = cv2.VideoWriter_fourcc(*'mp4v')
output_directory = './results_video/only_video_results/'
os.makedirs(output_directory, exist_ok=True)
out_video = cv2.VideoWriter(os.path.join(output_directory, f'{current_time1}_raw_img.mp4'), fourcc, fps, (width, height))

while True:
    # 读取视频帧
    ret, frame = video_capture.read()
    if not ret:
        break

    # 处理视频帧
    frame = frame_handler(frame)

    # 保存本地视频
    out_video.write(frame)

    # 推送帧到 RTSP 流
    pipe.stdin.write(frame.tobytes())

    # 键盘输入 'q' 退出
    if  0xFF == ord('q'):
        break

# 释放资源
video_capture.release()
out_video.release()
cv2.destroyAllWindows()
pipe.terminate()
