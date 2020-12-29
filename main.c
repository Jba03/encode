#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "encoder.h"

static int got_video;
static int got_audio;
static int got_output;
static int got_bitrate;

static struct option long_options[] =
{
    {"input",       required_argument,  0,  'i'},
    {"output",      required_argument,  0,  'o'},
    {"scale",       required_argument,  0,  's'},
    {"crf",         required_argument,  0,  'c'},
    {"bitrate",     required_argument,  0,  'b'},
    {"x264-preset", required_argument,  0,  'p'},
    {"help",        no_argument,        0,  'h'},
    {0, 0, 0, 0},
};

static const char* extension(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if(!dot || dot == filename) return "";
    return dot + 1;
}

static void usage()
{
    printf("usage: encode [-i input] [-scbp] [-o output]       \n");
    printf("  -i        file input: avi, sox                   \n");
    printf("  -s        set output video scale                 \n");
    printf("  -c        set constant rate factor (1.0 ... inf) \n");
    printf("  -b        set output bitrate                     \n");
    printf("  -p        x264 preset                            \n");
    printf("  -o        file output: mkv                       \n");
}

int main(int argc, char** argv)
{
    int c;
    struct encoder e;
    
    if (argc < 2)
    {
        usage();
        return 0;
    }
    
    while (1)
    {
        int option_index;
        
        c = getopt_long(argc, argv, "i:o:ps:c:b:h", long_options, &option_index);
        if (c == -1)
            break;
        
        switch (c)
        {
            case 'i':
                if (optarg)
                {
                    if (!strcmp(extension(optarg), "avi"))
                    {
                        got_video = 1;
                        e.i_video_filename = optarg;
                        break;
                    }
                    
                    if (!strcmp(extension(optarg), "sox"))
                    {
                        e.i_audio_filename = optarg;
                        got_audio = 1;
                        break;
                    }
                    
                    fprintf(stderr, "Unsupported input format '%s'\n", extension(optarg));
                    return -1;
                }
                break;
                
            case 'o':
                if (optarg) got_output = 1;
                if (!strcmp(extension(optarg), "mkv")) e.o_filename = optarg;
                else got_output = 0;
                break;
                
            case 'p':
                e.x264_preset = optarg;
                break;
                
            case 's':
                if (!optarg)
                {
                    fprintf(stderr, "Invalid scale or output resolution\n");
                    return -1;
                }
                
                if (sscanf(optarg, "%d:%d", &e.ow, &e.oh) < 2)
                {
                    fprintf(stderr, "Invalid scale or output resolution\n");
                    return -1;
                }
                break;
                
            case 'c':                
                e.crf = atof(optarg);
                if (e.crf < 1.0) e.crf = 1.0;
                if (e.crf > 51.0) e.crf = 51.0;
                break;
                
            case 'b':
                if (optarg) got_bitrate = 1;
                e.bitrate = atoi(optarg);
                if (e.bitrate <= 0 || e.bitrate > 100000)
                    e.bitrate = 60000;
                break;
                
            case 'h':
                usage();
                return 0;
                
            default:
                break;
        }
    }
    
    if (!got_video)
    {
        fprintf(stderr, "No video input specified\n");
        return -1;
    }
    
    if (!got_output)
    {
        fprintf(stderr, "No video output\n");
        return -1;
    }
    
    if (e.crf == 0)
        e.crf = 23.0;
    
    if (e.bitrate == 0)
        e.bitrate = 60000;
    
    if (!e.x264_preset)
        e.x264_preset = "veryfast";
    
    
    /* Initialize encoder */
    if (encoder_init(&e) < 0)
    {
        fprintf(stderr, "Failed to initialize encoder\n");
        return -1;
    }
    
    printf("\n\n");
    printf("width   = %d\n", e.ow);
    printf("height  = %d\n", e.oh);
    printf("crf     = %lf\n", e.crf);
    printf("bitrate = %lld\n", e.bitrate);
    printf("x264 preset = %s\n", e.x264_preset);
    
    printf("\n\n");
    
    /* Start encoder */
    if (encoder_encode(&e) < 0)
    {
        fprintf(stderr, "Failed to start encoder\n");
        return -1;
    }
    
    encoder_close(&e);
    
    return 0;
}
