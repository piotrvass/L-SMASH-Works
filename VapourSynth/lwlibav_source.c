/*****************************************************************************
 * lwlibav_source.c
 *****************************************************************************
 * Copyright (C) 2013 L-SMASH Works project
 *
 * Authors: Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *****************************************************************************/

/* This file is available under an ISC license.
 * However, when distributing its binary file, it will be under LGPL or GPL. */

#define NO_PROGRESS_HANDLER

/* Libav (LGPL or GPL) */
#include <libavformat/avformat.h>       /* Codec specific info importer */
#include <libavcodec/avcodec.h>         /* Decoder */
#include <libswscale/swscale.h>         /* Colorspace converter */
#include <libavutil/imgutils.h>

#include "lsmashsource.h"
#include "video_output.h"

/* Dummy definitions.
 * Audio resampler/buffer is NOT used at all in this filter. */
typedef void AVAudioResampleContext;
typedef void audio_samples_t;
int flush_resampler_buffers( AVAudioResampleContext *avr ){ return 0; }
int update_resampler_configuration( AVAudioResampleContext *avr,
                                    uint64_t out_channel_layout, int out_sample_rate, enum AVSampleFormat out_sample_fmt,
                                    uint64_t  in_channel_layout, int  in_sample_rate, enum AVSampleFormat  in_sample_fmt,
                                    int *input_planes, int *input_block_align ){ return 0; }
int resample_audio( AVAudioResampleContext *avr, audio_samples_t *out, audio_samples_t *in ){ return 0; }
void avresample_free( AVAudioResampleContext **avr ){ }

#include "../common/utils.h"
#include "../common/progress.h"
#include "../common/lwlibav_dec.h"
#include "../common/lwlibav_video.h"
#include "../common/lwlibav_audio.h"
#include "../common/lwindex.h"

typedef struct
{
    VSVideoInfo            vi;
    lwlibav_file_handler_t lwh;
    video_decode_handler_t vdh;
    video_output_handler_t voh;
} lwlibav_handler_t;

static void VS_CC vs_filter_init( VSMap *in, VSMap *out, void **instance_data, VSNode *node, VSCore *core, const VSAPI *vsapi )
{
    lwlibav_handler_t *hp = (lwlibav_handler_t *)*instance_data;
    vsapi->setVideoInfo( &hp->vi, 1, node );
}

static int prepare_video_decoding( lwlibav_handler_t *hp, VSCore *core, const VSAPI *vsapi )
{
    video_decode_handler_t *vdhp  = &hp->vdh;
    video_output_handler_t *vohp  = &hp->voh;
    VSVideoInfo            *vi    = &hp->vi;
    vs_basic_handler_t     *vsbhp = (vs_basic_handler_t *)vdhp->eh.message_priv;
    /* Allocate video frame buffer. */
    vdhp->input_buffer_size += FF_INPUT_BUFFER_PADDING_SIZE;
    vdhp->input_buffer       = (uint8_t *)av_mallocz( vdhp->input_buffer_size );
    if( !vdhp->input_buffer )
    {
        set_error( vsbhp, "lsmas: failed to allocate video frame buffer." );
        return -1;
    }
    /* Import AVIndexEntrys. */
    if( vdhp->index_entries )
    {
        AVStream *video_stream = vdhp->format->streams[ vdhp->stream_index ];
        for( int i = 0; i < vdhp->index_entries_count; i++ )
        {
            AVIndexEntry *ie = &vdhp->index_entries[i];
            if( av_add_index_entry( video_stream, ie->pos, ie->timestamp, ie->size, ie->min_distance, ie->flags ) < 0 )
            {
                set_error( vsbhp, "lsmas: failed to import AVIndexEntrys for video." );
                return -1;
            }
        }
        av_freep( &vdhp->index_entries );
    }
    /* Set up output format. */
    vdhp->ctx->width   = vdhp->initial_width;
    vdhp->ctx->height  = vdhp->initial_height;
    vdhp->ctx->pix_fmt = vdhp->initial_pix_fmt;
    enum AVPixelFormat input_pixel_format = vdhp->ctx->pix_fmt;
    if( determine_colorspace_conversion( vohp, &vdhp->ctx->pix_fmt ) )
    {
        set_error( vsbhp, "lsmas: %s is not supported", av_get_pix_fmt_name( input_pixel_format ) );
        return -1;
    }
    if( vohp->variable_info )
    {
        vi->format = NULL;
        vi->width  = 0;
        vi->height = 0;
    }
    else
    {
        vi->format = vsapi->getFormatPreset( vohp->vs_output_pixel_format, core );
        vi->width  = vdhp->max_width;
        vi->height = vdhp->max_height;
        vohp->background_frame = vsapi->newVideoFrame( vi->format, vi->width, vi->height, NULL, core );
        if( !vohp->background_frame )
        {
            set_error( vsbhp, "lsmas: failed to allocate memory for the background black frame data." );
            return -1;
        }
        vohp->make_black_background( vohp->background_frame, vsapi );
    }
    /* Set up scaler. */
    video_scaler_handler_t *vshp = &vohp->scaler;
    vshp->flags   = SWS_FAST_BILINEAR;
    vshp->sws_ctx = sws_getCachedContext( NULL,
                                          vdhp->ctx->width, vdhp->ctx->height, vdhp->ctx->pix_fmt,
                                          vdhp->ctx->width, vdhp->ctx->height, vshp->output_pixel_format,
                                          vshp->flags, NULL, NULL, NULL );
    if( !vshp->sws_ctx )
    {
        set_error( vsbhp, "lsmas: failed to get swscale context." );
        return -1;
    }
    /* Find the first valid video frame. */
    vdhp->seek_flags = (vdhp->seek_base & SEEK_FILE_OFFSET_BASED) ? AVSEEK_FLAG_BYTE : vdhp->seek_base == 0 ? AVSEEK_FLAG_FRAME : 0;
    if( vi->numFrames != 1 )
    {
        vdhp->seek_flags |= AVSEEK_FLAG_BACKWARD;
        uint32_t rap_number;
        lwlibav_find_random_accessible_point( vdhp, 1, 0, &rap_number );
        int64_t rap_pos = lwlibav_get_random_accessible_point_position( vdhp, rap_number );
        if( av_seek_frame( vdhp->format, vdhp->stream_index, rap_pos, vdhp->seek_flags ) < 0 )
            av_seek_frame( vdhp->format, vdhp->stream_index, rap_pos, vdhp->seek_flags | AVSEEK_FLAG_ANY );
    }
    for( uint32_t i = 1; i <= vi->numFrames + get_decoder_delay( vdhp->ctx ); i++ )
    {
        AVPacket pkt = { 0 };
        get_av_frame( vdhp->format, vdhp->stream_index, &vdhp->input_buffer, &vdhp->input_buffer_size, &pkt );
        AVFrame *av_frame = vdhp->frame_buffer;
        avcodec_get_frame_defaults( av_frame );
        int got_picture;
        if( avcodec_decode_video2( vdhp->ctx, av_frame, &got_picture, &pkt ) >= 0 && got_picture )
        {
            vohp->first_valid_frame_number = i - MIN( get_decoder_delay( vdhp->ctx ), vdhp->delay_count );
            if( vohp->first_valid_frame_number > 1 || vi->numFrames == 1 )
            {
                vohp->first_valid_frame = new_output_video_frame( vohp, av_frame, NULL, core, vsapi );
                if( !vohp->first_valid_frame )
                {
                    set_error( vsbhp, "lsmas: failed to allocate memory for the first valid video frame data." );
                    return -1;
                }
                if( make_frame( vohp, av_frame, vohp->first_valid_frame, NULL, vsapi ) )
                {
                    vsapi->freeFrame( vohp->first_valid_frame );
                    vohp->first_valid_frame = NULL;
                    continue;
                }
            }
            break;
        }
        else if( pkt.data )
            ++ vdhp->delay_count;
    }
    vdhp->last_frame_number = vi->numFrames + 1;    /* Force seeking at the first reading. */
    return 0;
}

static void set_frame_properties( lwlibav_handler_t *hp, AVFrame *picture, VSFrameRef *frame, const VSAPI *vsapi )
{
    video_decode_handler_t *vdhp  = &hp->vdh;
    VSVideoInfo            *vi    = &hp->vi;
    AVCodecContext         *ctx   = vdhp->ctx;
    VSMap                  *props = vsapi->getFramePropsRW( frame );
    /* Sample aspect ratio */
    vsapi->propSetInt( props, "_SARNum", picture->sample_aspect_ratio.num, paReplace );
    vsapi->propSetInt( props, "_SARDen", picture->sample_aspect_ratio.den, paReplace );
    /* Sample duration
     * Variable Frame Rate is not supported yet. */
    vsapi->propSetInt( props, "_DurationNum", vi->fpsDen, paReplace );
    vsapi->propSetInt( props, "_DurationDen", vi->fpsNum, paReplace );
    /* Color format */
    if( ctx )
    {
        vsapi->propSetInt( props, "_ColorRange",  ctx->color_range != AVCOL_RANGE_JPEG, paReplace );
        vsapi->propSetInt( props, "_ColorSpace",  ctx->colorspace,                      paReplace );
        int chroma_loc;
        switch( ctx->chroma_sample_location )
        {
            case AVCHROMA_LOC_LEFT       : chroma_loc = 0;  break;
            case AVCHROMA_LOC_CENTER     : chroma_loc = 1;  break;
            case AVCHROMA_LOC_TOPLEFT    : chroma_loc = 2;  break;
            case AVCHROMA_LOC_TOP        : chroma_loc = 3;  break;
            case AVCHROMA_LOC_BOTTOMLEFT : chroma_loc = 4;  break;
            case AVCHROMA_LOC_BOTTOM     : chroma_loc = 5;  break;
            default                      : chroma_loc = -1; break;
        }
        if( chroma_loc != -1 )
            vsapi->propSetInt( props, "_ChromaLocation", chroma_loc, paReplace );
    }
    /* Picture type */
    char pict_type = av_get_picture_type_char( picture->pict_type );
    vsapi->propSetData( props, "_PictType", &pict_type, 1, paReplace );
    /* Progressive or Interlaced */
    vsapi->propSetInt( props, "_FieldBased", !!picture->interlaced_frame, paReplace );
}

static const VSFrameRef *VS_CC vs_filter_get_frame( int n, int activation_reason, void **instance_data, void **frame_data, VSFrameContext *frame_ctx, VSCore *core, const VSAPI *vsapi )
{
    if( activation_reason != arInitial )
        return NULL;
    lwlibav_handler_t *hp = (lwlibav_handler_t *)*instance_data;
    VSVideoInfo       *vi = &hp->vi;
    uint32_t frame_number = n + 1;     /* frame_number is 1-origin. */
    if( frame_number > vi->numFrames )
    {
        vsapi->setFilterError( "lsmas: exceeded the number of frames.", frame_ctx );
        return NULL;
    }
    video_decode_handler_t *vdhp = &hp->vdh;
    video_output_handler_t *vohp = &hp->voh;
    if( frame_number < vohp->first_valid_frame_number || vi->numFrames == 1 )
    {
        /* Copy the first valid video frame. */
        vdhp->last_frame_number = vi->numFrames + 1;    /* Force seeking at the next access for valid video sample. */
        return vsapi->copyFrame( vohp->first_valid_frame, core );
    }
    if( vdhp->eh.error )
        return vsapi->newVideoFrame( vi->format, vi->width, vi->height, NULL, core );
    /* Set up VapourSynth error handler. */
    vs_basic_handler_t vsbh = { 0 };
    vsbh.out       = NULL;
    vsbh.frame_ctx = frame_ctx;
    vsbh.vsapi     = vsapi;
    vdhp->eh.message_priv  = &vsbh;
    vdhp->eh.error_message = set_error;
    /* Get and decode the desired video frame. */
    if( lwlibav_get_video_frame( vdhp, frame_number, vi->numFrames ) )
        return NULL;
    /* Output the video frame. */
    AVFrame    *av_frame = vdhp->frame_buffer;
    VSFrameRef *vs_frame = new_output_video_frame( vohp, av_frame, frame_ctx, core, vsapi );
    if( vs_frame )
    {
        set_frame_properties( hp, av_frame, vs_frame, vsapi );
        if( make_frame( vohp, av_frame, vs_frame, frame_ctx, vsapi ) )
        {
            vsapi->setFilterError( "lsmas: failed to output a video frame.", frame_ctx );
            return vs_frame;
        }
    }
    return vs_frame;
}

static void VS_CC vs_filter_free( void *instance_data, VSCore *core, const VSAPI *vsapi )
{
    lwlibav_handler_t *hp = (lwlibav_handler_t *)instance_data;
    if( !hp )
        return;
    lwlibav_cleanup_video_decode_handler( &hp->vdh );
    if( hp->voh.first_valid_frame )
        vsapi->freeFrame( hp->voh.first_valid_frame );
    if( hp->voh.scaler.sws_ctx )
        sws_freeContext( hp->voh.scaler.sws_ctx );
    if( hp->lwh.file_path )
        free( hp->lwh.file_path );
    free( hp );
}

void VS_CC vs_lwlibavsource_create( const VSMap *in, VSMap *out, void *user_data, VSCore *core, const VSAPI *vsapi )
{
    /* Get file path. */
    const char *file_path = vsapi->propGetData( in, "source", 0, NULL );
    if( !file_path )
    {
        vsapi->setError( out, "lsmas: failed to get source file name." );
        return;
    }
    /* Allocate the handler of this filter function. */
    lwlibav_handler_t *hp = lw_malloc_zero( sizeof(lwlibav_handler_t) );
    if( !hp )
    {
        vsapi->setError( out, "lsmas: failed to allocate the LW-Libav handler." );
        return;
    }
    vs_basic_handler_t vsbh = { 0 };
    vsbh.out       = out;
    vsbh.frame_ctx = NULL;
    vsbh.vsapi     = vsapi;
    /* Set up error handler. */
    error_handler_t eh = { 0 };
    eh.message_priv  = &vsbh;
    eh.error_message = set_error;
    /* Get options. */
    int64_t stream_index;
    int64_t threads;
    int64_t cache_index;
    int64_t seek_mode;
    int64_t seek_threshold;
    int64_t variable_info;
    const char *format;
    set_option_int64 ( &stream_index,   0,    "stream_index",   in, vsapi );
    set_option_int64 ( &threads,        0,    "threads",        in, vsapi );
    set_option_int64 ( &cache_index,    1,    "cache_index",    in, vsapi );
    set_option_int64 ( &seek_mode,      0,    "seek_mode",      in, vsapi );
    set_option_int64 ( &seek_threshold, 10,   "seek_threshold", in, vsapi );
    set_option_int64 ( &variable_info,  0,    "variable",       in, vsapi );
    set_option_string( &format,         NULL, "format",         in, vsapi );
    /* Set options. */
    lwlibav_option_t opt;
    opt.file_path         = file_path;
    opt.threads           = threads >= 0 ? threads : 0;
    opt.av_sync           = 0;
    opt.no_create_index   = !cache_index;
    opt.force_video       = (stream_index >= 0);
    opt.force_video_index = stream_index >= 0 ? stream_index : -1;
    opt.force_audio       = 0;
    opt.force_audio_index = -1;
    hp->vdh.seek_mode              = CLIP_VALUE( seek_mode,      0, 2 );
    hp->vdh.forward_seek_threshold = CLIP_VALUE( seek_threshold, 1, 999 );
    hp->voh.variable_info          = CLIP_VALUE( variable_info,  0, 1 );
    hp->voh.vs_output_pixel_format = hp->voh.variable_info ? pfNone : get_vs_output_pixel_format( format );
    /* Set up progress indicator. */
    progress_indicator_t indicator;
    indicator.open   = NULL;
    indicator.update = NULL;
    indicator.close  = NULL;
    /* Construct index. */
    audio_decode_handler_t adh = { 0 };
    audio_output_handler_t aoh = { 0 };
    int ret = lwlibav_construct_index( &hp->lwh, &hp->vdh, &adh, &aoh, &eh, &opt, &indicator, NULL );
    lwlibav_cleanup_audio_decode_handler( &adh );
    lwlibav_cleanup_audio_output_handler( &aoh );
    if( ret < 0 )
    {
        vs_filter_free( hp, core, vsapi );
        set_error( &vsbh, "lsmas: failed to construct index." );
        return;
    }
    /* Get the desired video track. */
    hp->vdh.eh = eh;
    if( lwlibav_get_desired_video_track( hp->lwh.file_path, &hp->vdh, hp->lwh.threads ) < 0 )
    {
        vs_filter_free( hp, core, vsapi );
        return;
    }
    /* Set up timestamp info. */
    hp->vi.numFrames = hp->vdh.frame_count;
    int fps_num;
    int fps_den;
    lwlibav_setup_timestamp_info( &hp->vdh, &fps_num, &fps_den );
    hp->vi.fpsNum = (unsigned int)fps_num;
    hp->vi.fpsDen = (unsigned int)fps_den;
    /* Set up decoders for this stream. */
    if( prepare_video_decoding( hp, core, vsapi ) < 0 )
    {
        vs_filter_free( hp, core, vsapi );
        return;
    }
    vsapi->createFilter( in, out, "LWLibavSource", vs_filter_init, vs_filter_get_frame, vs_filter_free, fmSerial, 0, hp, core );
    return;
}