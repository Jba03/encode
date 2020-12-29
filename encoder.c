#include "encoder.h"

#include <stdio.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

static int out_width, out_height;
static double crf;
static int64_t bitrate;
static const char* x264_preset;

static AVFormatContext* i_vfmt_ctx; /* Input video format context */
static AVFormatContext* i_afmt_ctx; /* Input audio format context */
static AVFormatContext* o_fmt_ctx;  /* Output format context */

static AVCodecContext* i_vcodec_ctx; /* Video decoder context */
static AVCodecContext* i_acodec_ctx; /* Audio decoder context */
static AVCodecContext* o_vcodec_ctx; /* Video encoder context */
static AVCodecContext* o_acodec_ctx; /* Audio encoder context */

static struct SwsContext* sws_ctx;
static AVFrame* scaled_frame;

/*
 * Opens the input video or 
 * audio file and corresponding decoder
 */
static int open_input_file(const char* filename, int is_audio)
{
    AVFormatContext* ifmt_ctx = NULL;
    int error;
    
    /* Open input file */
    if ((error = avformat_open_input(&ifmt_ctx, filename, NULL, NULL)) < 0) {
        fprintf(stderr, "Failed to open input file\n");
        return error;
    }
    
    /* Find stream information */
    if ((error = avformat_find_stream_info(ifmt_ctx, NULL)) < 0) {
        fprintf(stderr, "Could not find stream information\n");
        return error;
    }
    
    /* Find and open decoders */
    for (unsigned int i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream* stream = ifmt_ctx->streams[i];
        AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        AVCodecContext* codec_ctx;
        
        if (!decoder) {
            fprintf(stderr, "Failed to find decoder for stream #%u\n", i);
            return AVERROR_DECODER_NOT_FOUND;
        }
        
        /* Allocate decoder context */
        if (!(codec_ctx = avcodec_alloc_context3(decoder))) {
            fprintf(stderr, "Failed to allocate decoder context for stream #%u\n", i);
            return AVERROR(ENOMEM);
        }
        
        if ((error = avcodec_parameters_to_context(codec_ctx, stream->codecpar)) < 0) {
            fprintf(stderr, "Failed to copy decoder parameters to input decoder context for stream #%u\n", i);
            return error;
        }
        
        if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO || codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
                
                codec_ctx->framerate = av_guess_frame_rate(ifmt_ctx, stream, NULL);
                /* assign input video codec context */
                i_vcodec_ctx = codec_ctx;
            } else if (codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
                
                /* assign input audio codec context */
                i_acodec_ctx = codec_ctx;
            }
            
            /* Open decoder */
            if ((error = avcodec_open2(codec_ctx, decoder, NULL)) < 0) {
                fprintf(stderr, "Failed to open decoder for stream #%u\n", i);
                return error;
            }
        }
    }
    
    is_audio ? (i_afmt_ctx = ifmt_ctx) : (i_vfmt_ctx = ifmt_ctx);
    
    av_dump_format(ifmt_ctx, is_audio, filename, 0);
    return 0;
}

/*
 * Opens the output file and finds the
 * container based on the file extension.
 */
static int open_output_file(const char* filename)
{
    AVIOContext* output_io_ctx;
    AVCodec *audio_encoder, *video_encoder;
    AVStream *audio_stream, *video_stream;
    int error;
    
    /* Check for invalid file format */
    if (av_guess_format(NULL, filename, NULL) != av_guess_format(NULL, ".mkv", NULL)) {
        fprintf(stderr, "Only .mkv is supported\n");
        return -1;
    }
    
    /* Open output file for writing */
    if ((error = avio_open(&output_io_ctx, filename, AVIO_FLAG_WRITE))) {
        fprintf(stderr, "Failed to open output file '%s'", filename);
        return error;
    }
    
    /* Create output format context */
    if (!(o_fmt_ctx = avformat_alloc_context())) {
        fprintf(stderr, "Failed to allocate output format context\n");
        return AVERROR(ENOMEM);
    }
    
    o_fmt_ctx->pb = output_io_ctx;
    
    /* Guess container format based on file extension */
    if (!(o_fmt_ctx->oformat = av_guess_format(NULL, filename, NULL))) {
        fprintf(stderr, "Could not find output file format\n");
        goto end;
    }
    
    if (!(o_fmt_ctx->url = av_strdup(filename))) {
        fprintf(stderr, "Could not allocate url\n");
        error = AVERROR(ENOMEM);
        goto end;
    }
    
    /* Allocate video stream */
    if (!(video_stream = avformat_new_stream(o_fmt_ctx, NULL))) {
        fprintf(stderr, "Failed to allocate output video stream\n");
        return AVERROR_UNKNOWN;
    }
    
    /* Find x264 encoder */
    if (!(video_encoder = avcodec_find_encoder(AV_CODEC_ID_H264))) {
        fprintf(stderr, "Could not find an appropriate video encoder\n");
        return AVERROR_INVALIDDATA;
    }
    
    /* Allocate audio encoder context */
    if (!(o_vcodec_ctx = avcodec_alloc_context3(video_encoder))) {
        fprintf(stderr, "Failed to allocate video encoder context\n");
        return AVERROR(ENOMEM);
    }
    
    av_opt_set(o_vcodec_ctx->priv_data, "preset", x264_preset, 0);
    
    char buf[8];
    memset(buf, 0, 8 * sizeof(char));
    sprintf(buf, "%.2lf", crf);
    
    av_opt_set(o_vcodec_ctx->priv_data, "crf", buf, 0);
    av_opt_set(o_vcodec_ctx->priv_data, "x264-params", "keyint_min=600:intra_refresh=1:b=0", 0);
    
    if (o_fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        o_vcodec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    
    /* Set video encoder parameters */
    o_vcodec_ctx->bit_rate = bitrate;
    o_vcodec_ctx->width = out_width;
    o_vcodec_ctx->height = out_height;
    o_vcodec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    o_vcodec_ctx->framerate = i_vcodec_ctx->framerate;
    o_vcodec_ctx->time_base = i_vcodec_ctx->time_base;
    
    /* Open video encoder */
    if ((error = avcodec_open2(o_vcodec_ctx, video_encoder, NULL)) < 0) {
        fprintf(stderr, "Failed to open output video encoder (stream 1)\n");
        return error;
    }
    
    /* Copy video codec context parameters */
    if ((error = avcodec_parameters_from_context(video_stream->codecpar, o_vcodec_ctx)) < 0) {
        fprintf(stderr, "Failed to copy encoder parameters to output audio stream 1\n");
        return error;
    }
    
    video_stream->time_base = o_vcodec_ctx->time_base;
    
    /* Allocate audio stream */
    if (!(audio_stream = avformat_new_stream(o_fmt_ctx, NULL))) {
        fprintf(stderr, "Failed to allocate output audio stream\n");
        return AVERROR_UNKNOWN;
    }
    
    /* Find audio encoder */
    if (!(audio_encoder = avcodec_find_encoder(AV_CODEC_ID_PCM_S32LE))) {
        fprintf(stderr, "Could not find an appropriate audio encoder\n");
        return AVERROR_INVALIDDATA;
    }
    
    /* Allocate audio encoder context */
    if (!(o_acodec_ctx = avcodec_alloc_context3(audio_encoder))) {
        fprintf(stderr, "Failed to allocate audio encoder context\n");
        return AVERROR(ENOMEM);
    }
    
    /* Set audio encoder parameters */
    o_acodec_ctx->sample_rate = i_acodec_ctx->sample_rate;
    o_acodec_ctx->channel_layout = i_acodec_ctx->channel_layout;
    o_acodec_ctx->channels = i_afmt_ctx ? i_acodec_ctx->channels :
    av_get_channel_layout_nb_channels(i_acodec_ctx->channels);
    o_acodec_ctx->sample_fmt = audio_encoder->sample_fmts[0];
    o_acodec_ctx->time_base = (AVRational){1, o_acodec_ctx->sample_rate};
    
    /* Open audio encoder */
    if ((error = avcodec_open2(o_acodec_ctx, audio_encoder, NULL)) < 0) {
        fprintf(stderr, "Failed to open output audio encoder (stream 1)\n");
        return error;
    }
    
    /* Copy audio codec context parameters */
    if ((error = avcodec_parameters_from_context(audio_stream->codecpar, o_acodec_ctx)) < 0) {
        fprintf(stderr, "Failed to copy encoder parameters to output audio stream 1\n");
        return error;
    }
    
    audio_stream->time_base = o_acodec_ctx->time_base;
    
    av_dump_format(o_fmt_ctx, 0, filename, 1);
    
    if ((error = avformat_write_header(o_fmt_ctx, NULL)) < 0) {
        fprintf(stderr, "Error occured while opening output file: %s\n", av_err2str(error));
        return error;
    }
    
    return 0;
    
end:
    avcodec_free_context(&o_vcodec_ctx);
    avcodec_free_context(&o_acodec_ctx);
    avio_close(o_fmt_ctx->pb);
    avformat_free_context(o_fmt_ctx);
    
    return error < 0 ? error : AVERROR_EXIT;
}

/* Initializes a data packet */
static void packet_init(AVPacket* pkt)
{
    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
}

/* Initializes a video/audio frame */
static int frame_init(AVFrame** frame)
{
    if (!(*frame = av_frame_alloc())) {
        fprintf(stderr, "Could not allocate frame\n");
        return AVERROR(ENOMEM);
    }
    
    return 0;
}

static int encode_write_frame(AVFrame* frame, AVFormatContext* o_fmt_ctx, AVCodecContext* o_codec_ctx, unsigned int stream_index)
{
    int error;
    
    if ((error = avcodec_send_frame(o_codec_ctx, frame)) < 0) {
        fprintf(stderr, "Error submitting frame for encoding: %s\n", av_err2str(error));
        return error;
    }
    
    while (error >= 0) {
        
        AVPacket pkt;
        packet_init(&pkt);
        
        error = avcodec_receive_packet(o_codec_ctx, &pkt);
        if (error < 0 && error != AVERROR(EAGAIN) && error != AVERROR_EOF) {
            fprintf(stderr, "Error while encoding video frame\n");
            return error;
        } else if (error >= 0) {
            av_packet_rescale_ts(&pkt, o_codec_ctx->time_base, o_fmt_ctx->streams[pkt.stream_index]->time_base);
            pkt.stream_index = stream_index;
            
            fflush(stdout);
            
            if (stream_index == 0)
            printf("Progess: %.2lf%%\r", (double)i_vcodec_ctx->frame_number / 
                   (double)i_vfmt_ctx->streams[0]->nb_frames * 100);
            
            error = av_interleaved_write_frame(o_fmt_ctx, &pkt);
            if (error < 0) {
                fprintf(stderr, "Error while writing video frame\n");
                return error;
            }
        }
    }
    
    return error == AVERROR_EOF;
}

/* Scales a video frame  */
static int scale_video_frame(AVFrame* in, AVFrame* out)
{
    int error;
    
    out->width = out_width;
    out->height = out_height;
    out->format = AV_PIX_FMT_YUV420P;
    
    error = sws_scale(sws_ctx,
                      (uint8_t const* *const)in->data,
                      in->linesize,
                      0,
                      in->height,
                      out->data,
                      out->linesize);
    if (error < 0)
    {
        fprintf(stderr, "sws_scale failed: %s\n", av_err2str(error));
        return error;
    }
    
    return 0;
}

int encoder_init(struct encoder* e)
{
    e->closed = 0;
    
    /* Open input files */
    if (open_input_file(e->i_video_filename, 0) < 0)
        return -1;
    
    if (e->i_audio_filename)
        if (open_input_file(e->i_audio_filename, 1) < 0)
            fprintf(stderr, "Audio file not supplied. Using video audio stream.\n");
            
    if ((e->sx == 0 || e->sy == 0) &&
        (e->ow == 0 || e->oh == 0))
    {
        fprintf(stderr, "Resolution cannot be zero\n");
        return -1;
    }
    
    out_width = e->ow;
    out_height = e->oh;
    crf = e->crf;
    bitrate = e->bitrate;
    x264_preset = e->x264_preset;
    
    if (e->sx != 0 && e->sy != 0)
    {
        out_width = i_vcodec_ctx->width * e->sx;
        out_height = i_vcodec_ctx->height * e->sy;
    }
    
    /* Open output */
    if (open_output_file(e->o_filename) < 0)
        return -1;
    
    frame_init(&scaled_frame);
    
    if (av_image_alloc(scaled_frame->data,
                       scaled_frame->linesize,
                       out_width,
                       out_height,
                       AV_PIX_FMT_YUV420P,
                       4) < 0) {
        fprintf(stderr, "Failed to allocate image\n");
        return -1;
    }
    
    sws_ctx = sws_getContext(i_vcodec_ctx->width,
                             i_vcodec_ctx->height,
                             i_vcodec_ctx->pix_fmt,
                             out_width,
                             out_height,
                             AV_PIX_FMT_YUV420P,
                             SWS_POINT,
                             0, 0, 0);
    if (!sws_ctx)
        return -1;
    
    return 0;
}

int encoder_encode(struct encoder* e)
{
    int error;
    int audio_eof = 0;
    AVPacket pkt;
    AVFrame* frame;
    
    pkt.data = NULL;
    pkt.size = 0;
    frame_init(&frame);
    
    while (1) {
        if ((error = av_read_frame(i_vfmt_ctx, &pkt)))
            break;
        
        if (pkt.stream_index == 0) {
            if ((error = avcodec_send_packet(i_vcodec_ctx, &pkt)) < 0) {
                fprintf(stderr, "Error while sending packet to decoder\n");
                break;
            }
            
            while (error >= 0) {
                error = avcodec_receive_frame(i_vcodec_ctx, frame);
                if (error == AVERROR(EAGAIN) || error == AVERROR_EOF) {
                    break;
                } else if (error < 0) {
                    fprintf(stderr, "Error while receiving a frame from the decoder\n");
                    goto end;
                }
                
                frame->pts = frame->best_effort_timestamp;
                
                /* Scale the frame to set output resolution */
                scale_video_frame(frame, scaled_frame);
                scaled_frame->pts = frame->pts;
                
                /* Write video frame */
                error = encode_write_frame(scaled_frame, o_fmt_ctx, o_vcodec_ctx, 0);
            }
            
            av_frame_unref(frame);
        }  else if (pkt.stream_index == 1 && !audio_eof) {
            
            if (i_afmt_ctx) {
                pkt.data = NULL;
                pkt.size = 0;
                if ((error = av_read_frame(i_afmt_ctx, &pkt)) < 0)
                    audio_eof = 1;
            }
            
            if ((error = avcodec_send_packet(i_acodec_ctx, &pkt)) < 0) {
                fprintf(stderr, "Error while sending packet to decoder\n");
                break;
            }
            
            while (error >= 0) {
                error = avcodec_receive_frame(i_acodec_ctx, frame);
                if (error == AVERROR(EAGAIN) || error == AVERROR_EOF) {
                    break;
                } else if (error < 0) {
                    fprintf(stderr, "Error while receiving a frame from the decoder\n");
                    goto end;
                }
                
                frame->pts = frame->best_effort_timestamp;
                
                /* Write audio frame */
                error = encode_write_frame(frame, o_fmt_ctx, o_acodec_ctx, 1);
            }
            
            av_frame_unref(frame);
        }
        
        av_packet_unref(&pkt);
    }
    
    av_write_trailer(o_fmt_ctx);
    
    e->closed = 1;
    
end:
    printf("Successfully encoded %d out of %lld frames\n",
           i_vcodec_ctx->frame_number,
           i_vfmt_ctx->streams[0]->nb_frames);
    
    return 0;
}

void encoder_close(struct encoder* e)
{
    av_frame_free(&scaled_frame);
    
    avformat_free_context(i_vfmt_ctx);
    avformat_free_context(i_afmt_ctx);
    avformat_free_context(o_fmt_ctx);
    
    avcodec_free_context(&i_vcodec_ctx);
    avcodec_free_context(&i_acodec_ctx);
    avcodec_free_context(&o_vcodec_ctx);
    avcodec_free_context(&o_acodec_ctx);
}
