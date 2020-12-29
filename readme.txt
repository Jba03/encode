encode - a tool for upscaling and encoding video files of tool assisted speedruns

link with:
    libavcodec
    libavformat
    libavutil
    libswscale
    
example usage:
    encode --input video.avi --input audio.sox --scale 2560:2240 --crf 1.0 --output out.mkv
