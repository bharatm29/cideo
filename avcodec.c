#include "libavutil/avutil.h"
#include "libavutil/pixfmt.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define SCREEN_WIDTH 1920
#define SCREEN_HEIGHT 1200

static AVFormatContext *fmt_ctx = NULL;
static AVCodecContext *video_dec_ctx = NULL, *audio_dec_ctx;
static int width, height;
static enum AVPixelFormat pix_fmt;
static AVStream *video_stream = NULL, *audio_stream = NULL;
static const char *src_filename = NULL;

static uint8_t *video_dst_data[4] = {NULL};
static uint16_t *audio_output_buffer = NULL;
static int video_dst_linesize[4];
static int video_bufsize;
static int rgba_bufsize;

static int video_stream_idx = -1, audio_stream_idx = -1;
static AVFrame *frame = NULL;
static AVFrame *rgba = NULL; // FIXME: please change name of this thing
static AVPacket *pkt = NULL;
static int video_frame_count = 0;
static int audio_frame_count = 0;
static bool noAudio = false;

static struct SwsContext *sws = NULL;
static struct SwrContext *swr = NULL;

static int output_video_frame(AVFrame *frame) {
    if (frame->width != width || frame->height != height ||
        frame->format != pix_fmt) {
        /* To handle this change, one could call av_image_alloc again and
         * decode the following frames into another rawvideo file. */
        fprintf(stderr,
                "Error: Width, height and pixel format have to be "
                "constant in a rawvideo file, but the width, height or "
                "pixel format of the input video changed:\n"
                "old: width = %d, height = %d, format = %s\n"
                "new: width = %d, height = %d, format = %s\n",
                width, height, av_get_pix_fmt_name(pix_fmt), frame->width,
                frame->height, av_get_pix_fmt_name(frame->format));
        return -1;
    }

    printf("video_frame n:%d\n", video_frame_count++);

    /* copy decoded frame to destination buffer:
     * this is required since rawvideo expects non aligned data */
    av_image_copy2(video_dst_data, video_dst_linesize, frame->data,
                   frame->linesize, pix_fmt, width, height);

    /* write to rawvideo file */
    // fwrite(video_dst_data[0], 1, video_dst_bufsize, video_dst_file);
    return 0;
}

static int submit_audio_packet(AVCodecContext *dec, const AVPacket *pkt) {
    int ret = 0;

    // submit the packet to the decoder
    ret = avcodec_send_packet(dec, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error submitting a packet for decoding (%s)\n",
                av_err2str(ret));
    }

    return ret;
}

static int decode_audio_packet(AVCodecContext *dec) {
    int ret;
    do {
        ret = avcodec_receive_frame(dec, frame);
        if (ret < 0 && !(ret == AVERROR_EOF || ret == AVERROR(EAGAIN))) {
            fprintf(stderr, "Error during audio decoding (%s)\n",
                    av_err2str(ret));
            return ret;
        }
    } while (ret == AVERROR_EOF || ret == AVERROR(EAGAIN));

    // size_t unpadded_linesize = frame->nb_samples *
    // av_get_bytes_per_sample(frame->format); fwrite(frame->extended_data[0],
    // 1, unpadded_linesize, file); for what tho?

    // FIXME: check if planar or not! =>
    // https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/demux_decode.c#L344

    if (audio_dec_ctx->sample_fmt == AV_SAMPLE_FMT_FLTP) {
        int nb_samples = frame->nb_samples;
        int channels = audio_dec_ctx->ch_layout.nb_channels;

        uint8_t *out[] = {
            (uint8_t *)audio_output_buffer}; // same as (uint8_t**)&outputBuffer
                                             // but cleaner?
        int out_samples = swr_convert(swr, out, frame->nb_samples,
                                      (const uint8_t **)frame->extended_data,
                                      frame->nb_samples);

        // size:- out_samples * channels
    }

    av_frame_unref(frame);
    return ret;
}

static int decode_video_packet(AVCodecContext *dec, const AVPacket *pkt) {
    int ret = 0;

    // submit the packet to the decoder
    ret = avcodec_send_packet(dec, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error submitting a packet for decoding (%s)\n",
                av_err2str(ret));
        return ret;
    }

    // get next available frame from the decoder
    do {
        ret = avcodec_receive_frame(dec, frame);
        if (ret < 0 && !(ret == AVERROR_EOF || ret == AVERROR(EAGAIN))) {
            fprintf(stderr, "Error during video decoding (%s)\n",
                    av_err2str(ret));
            return ret;
        }
    } while (ret == AVERROR_EOF || ret == AVERROR(EAGAIN));

    sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0,
              frame->height, rgba->data, rgba->linesize);

    av_image_copy2(video_dst_data, video_dst_linesize, rgba->data,
                   rgba->linesize, AV_PIX_FMT_RGBA, rgba->width, rgba->height);

    // read from video_dst_data[0]

    av_frame_unref(frame);

    return ret;
}

void cleanup() {
    avcodec_free_context(&video_dec_ctx);
    avcodec_free_context(&audio_dec_ctx);
    avformat_close_input(&fmt_ctx);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_free(video_dst_data[0]);
}

static int open_codec_context(int *stream_idx, AVCodecContext **dec_ctx,
                              AVFormatContext *fmt_ctx, enum AVMediaType type) {
    int ret, stream_index;
    AVStream *st;
    const AVCodec *dec = NULL;

    ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not find %s stream in input file '%s'\n",
                av_get_media_type_string(type), src_filename);
        return ret;
    } else {
        stream_index = ret;
        st = fmt_ctx->streams[stream_index];

        /* find decoder for the stream */
        dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) {
            fprintf(stderr, "Failed to find %s codec\n",
                    av_get_media_type_string(type));
            return AVERROR(EINVAL);
        }

        /* Allocate a codec context for the decoder */
        *dec_ctx = avcodec_alloc_context3(dec);
        if (!*dec_ctx) {
            fprintf(stderr, "Failed to allocate the %s codec context\n",
                    av_get_media_type_string(type));
            return AVERROR(ENOMEM);
        }

        /* Copy codec parameters from input stream to output codec context */
        if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
            fprintf(stderr,
                    "Failed to copy %s codec parameters to decoder context\n",
                    av_get_media_type_string(type));
            return ret;
        }

        /* Init the decoders */
        if ((ret = avcodec_open2(*dec_ctx, dec, NULL)) < 0) {
            fprintf(stderr, "Failed to open %s codec\n",
                    av_get_media_type_string(type));
            return ret;
        }
        *stream_idx = stream_index;
    }

    return 0;
}

static int get_format_from_sample_fmt(const char **fmt,
                                      enum AVSampleFormat sample_fmt) {
    int i;
    struct sample_fmt_entry {
        enum AVSampleFormat sample_fmt;
        const char *fmt_be, *fmt_le;
    } sample_fmt_entries[] = {
        {AV_SAMPLE_FMT_U8, "u8", "u8"},
        {AV_SAMPLE_FMT_S16, "s16be", "s16le"},
        {AV_SAMPLE_FMT_S32, "s32be", "s32le"},
        {AV_SAMPLE_FMT_FLT, "f32be", "f32le"},
        {AV_SAMPLE_FMT_DBL, "f64be", "f64le"},
    };
    *fmt = NULL;

    for (i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++) {
        struct sample_fmt_entry *entry = &sample_fmt_entries[i];
        if (sample_fmt == entry->sample_fmt) {
            *fmt = AV_NE(entry->fmt_be, entry->fmt_le);
            return 0;
        }
    }

    fprintf(stderr, "sample format %s is not supported as output format\n",
            av_get_sample_fmt_name(sample_fmt));
    return -1;
}

void read_frame() {
    int ret;
    /* read frames from the file */
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        // check if the packet belongs to a stream we are interested in,
        // otherwise skip it
        if (pkt->stream_index == video_stream_idx) {
            // decode immediately to get the frame
            ret = decode_video_packet(video_dec_ctx, pkt);
            break;
        } else if (pkt->stream_index == audio_stream_idx) {
            ret = submit_audio_packet(audio_dec_ctx, pkt); // only submit
            break;
        }

        av_packet_unref(pkt);

        if (ret < 0) {
            fprintf(stderr, "Error decoding packet\n");
            break;
        }
    }
}

int setup_avcodec(const char *filename) {
    src_filename = filename;
    int ret = 0;

    /* open input file, and allocate format context */
    if (avformat_open_input(&fmt_ctx, src_filename, NULL, NULL) < 0) {
        fprintf(stderr, "Could not open source file %s\n", src_filename);
        exit(1);
    }

    /* retrieve stream information */
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "Could not find stream information\n");
        exit(1);
    }

    if (open_codec_context(&video_stream_idx, &video_dec_ctx, fmt_ctx,
                           AVMEDIA_TYPE_VIDEO) >= 0) {
        video_stream = fmt_ctx->streams[video_stream_idx];

        /* allocate image where the decoded image will be put */
        width = video_dec_ctx->width; // FIXME: really can just remove this shit
        height = video_dec_ctx->height;
        pix_fmt = video_dec_ctx->pix_fmt;
        ret = av_image_alloc(video_dst_data, video_dst_linesize, width, height,
                             AV_PIX_FMT_RGBA, 1);
        if (ret < 0) {
            fprintf(stderr, "Could not allocate raw video buffer\n");
            cleanup();
            exit(ret);
        }
        video_bufsize = ret;
    }

    if (!video_stream) {
        fprintf(stderr, "Could not find video stream in the input, aborting\n");
        ret = 1;
        cleanup();
        exit(ret);
    }

    // audio
    ret = open_codec_context(&audio_stream_idx, &audio_dec_ctx, fmt_ctx,
                             AVMEDIA_TYPE_AUDIO);
    if (ret >= 0) {
        audio_stream = fmt_ctx->streams[audio_stream_idx];
    } else {
        noAudio = true;
    }

    frame = av_frame_alloc();
    rgba = av_frame_alloc();

    if (!frame || !rgba) {
        fprintf(stderr, "Could not allocate frame\n");
        ret = AVERROR(ENOMEM);
        cleanup();
        exit(ret);
    }

    float scaleFactor =
        MIN((float)SCREEN_WIDTH / width, (float)SCREEN_HEIGHT / height);
    int scaledW = video_dec_ctx->width * scaleFactor;
    int scaledH = video_dec_ctx->height * scaleFactor;

    rgba->format = AV_PIX_FMT_RGBA;
    rgba->width = scaledW;
    rgba->height = scaledH;

    if (av_frame_get_buffer(rgba, 32) < 0) {
        fprintf(stderr, "Could not allocate RGBA frame buffer\n");
        cleanup();
        exit(ret);
    }

    rgba_bufsize =
        av_image_get_buffer_size(rgba->format, rgba->width, rgba->height, 1);

    sws = sws_getContext(video_dec_ctx->width, video_dec_ctx->height,
                         video_dec_ctx->pix_fmt, scaledW, scaledH,
                         AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);

    if (!noAudio) {
        AVChannelLayout in_ch_layout;
        av_channel_layout_copy(&in_ch_layout, &audio_dec_ctx->ch_layout);

        AVChannelLayout out_ch_layout;
        av_channel_layout_copy(&out_ch_layout, &audio_dec_ctx->ch_layout);

        // we will be converting AV_SAMPLE_FMT_FLTP into AV_SAMPLE_FMT_S16
        swr_alloc_set_opts2(&swr, &out_ch_layout, AV_SAMPLE_FMT_S16,
                            audio_dec_ctx->sample_rate, &in_ch_layout,
                            audio_dec_ctx->sample_fmt,
                            audio_dec_ctx->sample_rate, 0, NULL);

        if (!swr || (ret = swr_init(swr)) < 0) {
            fprintf(stderr, "Failed to initialize SWR context\n");
            cleanup();
            exit(ret);
        }
    }

    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "Could not allocate packet\n");
        ret = AVERROR(ENOMEM);
        cleanup();
        exit(ret);
    }

    if (!noAudio) {
        // output buffer
        audio_output_buffer =
            malloc(sizeof(uint16_t) * (audio_dec_ctx->frame_size *
                                       audio_dec_ctx->ch_layout.nb_channels));
        if (!audio_output_buffer) {
            fprintf(stderr, "Failed to allocate audio output buffer\n");
            return -1;
        }
    }

    return ret;
}
