sudo apt-get install libavcodec-dev libavformat-dev libswscale-dev libavutil-dev libavdevice-dev
sudo apt-get install libsdl2-dev


g++ rtmp_receiver.cpp -o rtmp_receiver -lavformat -lavcodec -lswscale -lavutil -lavdevice -lpthread -lSDL2
./rtmp_reciver