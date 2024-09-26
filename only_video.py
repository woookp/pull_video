import cv2
import time
from datetime import datetime
import os
import subprocess

video_capture = cv2.VideoCapture(2)

push_url = "rtsp://192.168.78.200:8554/stream"

width = 1280  # 720p宽度
height = 720  # 720p高度
fps = int(video_capture.get(cv2.CAP_PROP_FPS))
print("width:", width, "height:", height, "fps:", fps)

def start_stream(current_fps):
    command = [
    'ffmpeg',
    '-y',
    '-an',
    '-f', 'rawvideo',
    '-vcodec', 'rawvideo',
    '-pix_fmt', 'bgr24',
    '-s', "{}x{}".format(width, height),
    '-r', str(current_fps),
    '-i', '-',
    '-c:v', 'libx264',
    '-preset', 'ultrafast',
    '-tune', 'zerolatency',
    '-crf', '28',
    '-f', 'rtsp',
    '-rtsp_transport', 'tcp',
    '-buffer_size', '1M',
    '-max_delay', '200000',
    push_url
    ]
    return subprocess.Popen(command, shell=False, stdin=subprocess.PIPE)

pipe = start_stream(fps)

current_time = datetime.now().strftime("%Y%m%d_%H%M%S")
fourcc = cv2.VideoWriter_fourcc(*'mp4v')
output_directory = './results_video/only_video_results/'
os.makedirs(output_directory, exist_ok=True)
out_video = cv2.VideoWriter(os.path.join(output_directory, f'{current_time}_raw_img.mp4'), fourcc, fps, (width, height))

retry_count = 0
max_retries = 100
retry_delay = 2

while True:
    ret, frame = video_capture.read()
    if not ret:
        break

    out_video.write(frame)

    current_fps = fps if retry_count == 0 else max(5, fps-10 - retry_count)

    retry_count = 0  # 在每一帧开始时重置重试计数
    while True:
        try:
            pipe.stdin.write(frame.tobytes())
            pipe.stdin.flush()  # 确保数据立即写入
            break  # 成功写入后跳出内层循环
        except (BrokenPipeError, OSError) as e:
            print(f"Error: {e}. Restarting stream...")
            pipe.terminate()  # 终止现有流
            pipe.wait()  # 等待子进程结束
            time.sleep(retry_delay)  # 等待重试
            
            # 尝试重新启动流
            retry_count += 1
            pipe = start_stream(current_fps)

            # 检查连接是否成功
            if pipe.poll() is not None:  # 如果进程已结束
                print("Stream initialization failed. Retrying...")
                time.sleep(retry_delay)  # 等待重试
            
            # 重新读取并写入当前帧
            ret, frame = video_capture.read()
            if ret:
                out_video.write(frame)
                try:
                    pipe.stdin.write(frame.tobytes())  # 尝试写入当前帧
                    pipe.stdin.flush()
                except Exception as e:
                    print(f"Failed to write frame after reconnecting: {e}")

            # 如果达到最大重试次数，退出程序
            if retry_count >= max_retries:
                print("Max retries reached. Exiting...")
                video_capture.release()
                out_video.release()
                cv2.destroyAllWindows()
                exit(1)

    if cv2.waitKey(1) & 0xFF == ord('q'):
        break

video_capture.release()
out_video.release()
cv2.destroyAllWindows()
pipe.terminate()

