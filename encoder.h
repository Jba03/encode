#ifndef encoder_h
#define encoder_h

#include <stdint.h>

struct encoder
{
    int closed;
    
    const char* i_video_filename; /* Input video */
    const char* i_audio_filename; /* Input audio, if any */
    const char* o_filename; /* Name of output file */
    
    int ow, oh;
    double sx, sy;
    double crf;
    
    const char* x264_preset;
    int64_t bitrate;
};

/*
 * Initializes the encoder,
 * opens input/output files and their respective codecs
 */
int encoder_init(struct encoder* e);

/* Starts the encoder */
int encoder_encode(struct encoder* e);

/* Closes the encoder */
void encoder_close(struct encoder* e);

#endif /* encoder_h */
