/*
 * Copyright © 2018 zoff@zoff.cc and mail@strfry.org
 *
 * This file is part of Tox, the free peer to peer instant messenger.
 *
 * Tox is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Tox is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "../../audio.h"
#include "../../video.h"
#include "../../msi.h"
#include "../../ring_buffer.h"
#include "../../rtp.h"
#include "../../tox_generic.h"
#include "../../../toxcore/mono_time.h"
#include "../toxav_codecs.h"

// for H264 ----------
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
// for H264 ----------


int global_h264_enc_profile_high_enabled = 0;
int global_h264_enc_profile_high_enabled_switch = 0;

/* ---------------------------------------------------
 *
 * Hardware specific defines for encoders and decoder
 * use -DXXXXXX to enable at compile time, otherwise defaults will be used
 *
 * ---------------------------------------------------
 */


/* ---------------------------------------------------
 * DEFAULT
 */
#define ACTIVE_HW_CODEC_CONFIG_NAME "_DEFAULT_"
#define H264_WANT_ENCODER_NAME "not used with x264"
#define H264_WANT_DECODER_NAME "h264"
#define X264_ENCODE_USED 1
// #define RAPI_HWACCEL_ENC 1
// #define RAPI_HWACCEL_DEC 1
/* !!multithreaded H264 decoding adds about 80ms of delay!! (0 .. disable, 1 .. disable also?) */
#define H264_DECODER_THREADS 4
#define H264_DECODER_THREAD_FRAME_ACTIVE 1
/* multithreaded encoding seems to add less delay (0 .. disable) */
#define X264_ENCODER_THREADS 4
#define X264_ENCODER_SLICES 4
#define H264_ENCODE_MAX_BITRATE_OVER_ALLOW 1.3
#define H264_ENCODER_STARTWITH_PROFILE_HIGH 0
/* ---------------------------------------------------
 * DEFAULT
 */


#ifdef HW_CODEC_CONFIG_TRIFA
/* ---------------------------------------------------
 * TRIfA
 */
#undef ACTIVE_HW_CODEC_CONFIG_NAME
#undef H264_WANT_ENCODER_NAME
#undef H264_WANT_DECODER_NAME
#undef X264_ENCODE_USED
#undef RAPI_HWACCEL_ENC
#undef RAPI_HWACCEL_DEC
#undef H264_DECODER_THREADS
#undef H264_DECODER_THREAD_FRAME_ACTIVE
#undef X264_ENCODER_THREADS
#undef X264_ENCODER_SLICES
#undef H264_ENCODER_STARTWITH_PROFILE_HIGH
// --
#define ACTIVE_HW_CODEC_CONFIG_NAME "HW_CODEC_CONFIG_TRIFA"
#define _TRIFA_CODEC_DECODER_ 1
#define H264_WANT_DECODER_NAME "h264_mediacodec"
// #define H264_WANT_DECODER_NAME "h264"
#define X264_ENCODE_USED 1
// #define RAPI_HWACCEL_DEC 1
#define H264_DECODER_THREADS 3
#define H264_DECODER_THREAD_FRAME_ACTIVE 0
#define X264_ENCODER_THREADS 3
#define X264_ENCODER_SLICES 3
#define H264_ENCODER_STARTWITH_PROFILE_HIGH 0
/* ---------------------------------------------------
 * TRIfA
 */
#endif

#ifdef HW_CODEC_CONFIG_RPI3_TBW_BIDI
/* ---------------------------------------------------
 * RPI3 tbw-bidi
 */
#undef ACTIVE_HW_CODEC_CONFIG_NAME
#undef H264_WANT_ENCODER_NAME
#undef H264_WANT_DECODER_NAME
#undef X264_ENCODE_USED
#undef RAPI_HWACCEL_ENC
#undef RAPI_HWACCEL_DEC
#undef H264_DECODER_THREADS
#undef H264_DECODER_THREAD_FRAME_ACTIVE
#undef X264_ENCODER_THREADS
#undef X264_ENCODER_SLICES
#undef H264_ENCODER_STARTWITH_PROFILE_HIGH
// --
#define ACTIVE_HW_CODEC_CONFIG_NAME "HW_CODEC_CONFIG_RPI3_TBW_BIDI"
#define H264_WANT_ENCODER_NAME "h264_omx"
#define H264_WANT_DECODER_NAME "h264_mmal"
#define X264_ENCODE_USED 1
// #define RAPI_HWACCEL_ENC 1
#define RAPI_HWACCEL_DEC 1
#define H264_DECODER_THREADS 0
#define H264_DECODER_THREAD_FRAME_ACTIVE 0
#define X264_ENCODER_THREADS 1
#define X264_ENCODER_SLICES 1
#define H264_ENCODER_STARTWITH_PROFILE_HIGH 1
/* ---------------------------------------------------
 * RPI3 bidi
 */
#endif

#ifdef HW_CODEC_CONFIG_RPI3_TBW_TV
/* ---------------------------------------------------
 * RPI3 tbw-TV
 */
#undef ACTIVE_HW_CODEC_CONFIG_NAME
#undef H264_WANT_ENCODER_NAME
#undef H264_WANT_DECODER_NAME
#undef X264_ENCODE_USED
#undef RAPI_HWACCEL_ENC
#undef RAPI_HWACCEL_DEC
#undef H264_DECODER_THREADS
#undef H264_DECODER_THREAD_FRAME_ACTIVE
#undef X264_ENCODER_THREADS
#undef X264_ENCODER_SLICES
#undef H264_ENCODER_STARTWITH_PROFILE_HIGH
// --
#define ACTIVE_HW_CODEC_CONFIG_NAME "HW_CODEC_CONFIG_RPI3_TBW_TV"
#define H264_WANT_DECODER_NAME "h264_mmal"
//#define H264_WANT_DECODER_NAME "h264"
#define X264_ENCODE_USED 1
#define RAPI_HWACCEL_DEC 1
#define H264_DECODER_THREADS 0
#define H264_DECODER_THREAD_FRAME_ACTIVE 0
#define X264_ENCODER_THREADS 0
#define X264_ENCODER_SLICES 0
#define H264_ENCODER_STARTWITH_PROFILE_HIGH 0
/* ---------------------------------------------------
 * RPI3 tbw-TV
 */
#endif

#ifdef HW_CODEC_CONFIG_UTOX_LINNVENC
/* ---------------------------------------------------
 * UTOX linux
 */
#undef ACTIVE_HW_CODEC_CONFIG_NAME
#undef H264_WANT_ENCODER_NAME
#undef H264_WANT_DECODER_NAME
#undef X264_ENCODE_USED
#undef RAPI_HWACCEL_ENC
#undef RAPI_HWACCEL_DEC
#undef H264_DECODER_THREADS
#undef H264_DECODER_THREAD_FRAME_ACTIVE
#undef X264_ENCODER_THREADS
#undef X264_ENCODER_SLICES
#undef H264_ENCODER_STARTWITH_PROFILE_HIGH
// --
#define ACTIVE_HW_CODEC_CONFIG_NAME "HW_CODEC_CONFIG_UTOX_LINNVENC"
#define H264_WANT_ENCODER_NAME "h264_nvenc"
#define H264_WANT_DECODER_NAME "h264"
// #define X264_ENCODE_USED 1
#define RAPI_HWACCEL_ENC 1
#define H264_DECODER_THREADS 4
#define H264_DECODER_THREAD_FRAME_ACTIVE 0
#define X264_ENCODER_THREADS 4
#define X264_ENCODER_SLICES 4
#define H264_ENCODER_STARTWITH_PROFILE_HIGH 1
/* ---------------------------------------------------
 * UTOX linux
 */
#endif

#ifdef HW_CODEC_CONFIG_TBW_LINNVENC
/* ---------------------------------------------------
 * tbw usb linux
 */
#undef ACTIVE_HW_CODEC_CONFIG_NAME
#undef H264_WANT_ENCODER_NAME
#undef H264_WANT_DECODER_NAME
#undef X264_ENCODE_USED
#undef RAPI_HWACCEL_ENC
#undef RAPI_HWACCEL_DEC
#undef H264_DECODER_THREADS
#undef H264_DECODER_THREAD_FRAME_ACTIVE
#undef X264_ENCODER_THREADS
#undef X264_ENCODER_SLICES
#undef H264_ENCODER_STARTWITH_PROFILE_HIGH
// --
#define ACTIVE_HW_CODEC_CONFIG_NAME "HW_CODEC_CONFIG_TBW_LINNVENC"
#define H264_WANT_ENCODER_NAME "h264_nvenc"
#define H264_WANT_DECODER_NAME "h264" // "h264_cuvid"
// #define X264_ENCODE_USED 1
#define RAPI_HWACCEL_ENC 1
// #define RAPI_HWACCEL_DEC 1
#define H264_DECODER_THREADS 4
#define H264_DECODER_THREAD_FRAME_ACTIVE 0
#define X264_ENCODER_THREADS 4
#define X264_ENCODER_SLICES 4
#define H264_ENCODER_STARTWITH_PROFILE_HIGH 1
/* ---------------------------------------------------
 * tbw usb linux
 */
#endif

#ifdef HW_CODEC_CONFIG_UTOX_WIN7
/* ---------------------------------------------------
 * UTOX win7
 */
#undef ACTIVE_HW_CODEC_CONFIG_NAME
#undef H264_WANT_ENCODER_NAME
#undef H264_WANT_DECODER_NAME
#undef X264_ENCODE_USED
#undef RAPI_HWACCEL_ENC
#undef RAPI_HWACCEL_DEC
#undef H264_DECODER_THREADS
#undef H264_DECODER_THREAD_FRAME_ACTIVE
#undef X264_ENCODER_THREADS
#undef X264_ENCODER_SLICES
#undef H264_ENCODER_STARTWITH_PROFILE_HIGH
// --
#define ACTIVE_HW_CODEC_CONFIG_NAME "HW_CODEC_CONFIG_UTOX_WIN7"
#define H264_WANT_ENCODER_NAME "h264_nvenc"
#define H264_WANT_DECODER_NAME "h264"
// #define X264_ENCODE_USED 1
#define RAPI_HWACCEL_ENC 1
// #define RAPI_HWACCEL_DEC 1
#define H264_DECODER_THREADS 2
#define H264_DECODER_THREAD_FRAME_ACTIVE 1
#define X264_ENCODER_THREADS 6
#define X264_ENCODER_SLICES 6
#define H264_ENCODER_STARTWITH_PROFILE_HIGH 1
/* ---------------------------------------------------
 * UTOX win7
 */
#endif


#ifdef HW_CODEC_CONFIG_UTOX_UB81
/* ---------------------------------------------------
 * UTOX win7
 */
#undef ACTIVE_HW_CODEC_CONFIG_NAME
//#undef H264_WANT_ENCODER_NAME
//#undef H264_WANT_DECODER_NAME
#undef H264_DECODER_THREADS
#undef H264_DECODER_THREAD_FRAME_ACTIVE
#undef X264_ENCODER_THREADS
#undef X264_ENCODER_SLICES
#undef H264_ENCODER_STARTWITH_PROFILE_HIGH
// --
#define ACTIVE_HW_CODEC_CONFIG_NAME "HW_CODEC_CONFIG_UTOX_UB81"
#define H264_DECODER_THREADS 2
#define H264_DECODER_THREAD_FRAME_ACTIVE 1
#define X264_ENCODER_THREADS 6
#define X264_ENCODER_SLICES 6
#define H264_ENCODER_STARTWITH_PROFILE_HIGH 1
/* ---------------------------------------------------
 * UTOX win7
 */
#endif

Logger *global__log = NULL;

#if 0
void my_printf(char* format, ...)
{
    uint8_t *output_chars = calloc(1, 5000);
    uint32_t pos = 0;

    va_list argp;
    va_start(argp, format);
    while (*format != '\0')
    {
        if (*format == '%')
        {
            format++;
            if (*format == '%')
            {
                output_chars[pos] = '%';
                pos++;
            }
            else if (*format == 'c')
            {
                char char_to_print = (char)va_arg(argp, int);
                output_chars[pos] = char_to_print;
                pos++;
            }
            else if (*format == 's')
            {
                char* char_to_print = va_arg(argp, char *);
                output_chars[pos] = char_to_print;
                pos++;
                output_chars[pos] = char_to_print;
                pos++;
                output_chars[pos] = char_to_print;
                pos++;
            }
            else
            {
                // fputs("Not implemented", stdout);
            }
        }
        else
        {
            // putchar(*format);
        }
        format++;
    }
    va_end(argp);

    LOGGER_WARNING(global__log, output_chars);
}
#endif

void my_log_callback(void *ptr, int level, const char *fmt, va_list vargs)
{
    // !!WARNING!! ffmpeg somehow gives back bad strings, calling "strlen" will crash
    //             so no printf type of function will work!!
    // my_printf(fmt, vargs);
    // LOGGER_WARNING(global__log, fmt, vargs);
}

VCSession *vc_new_h264(Logger *log, ToxAV *av, uint32_t friend_number, toxav_video_receive_frame_cb *cb, void *cb_data,
                       VCSession *vc)
{


    // ENCODER -------

    LOGGER_WARNING(log, "HW CODEC CONFIG ACTIVE: %s", ACTIVE_HW_CODEC_CONFIG_NAME);

#ifdef X264_ENCODE_USED

    x264_param_t param;

    // -- set inital value for H264 encoder profile --
    global_h264_enc_profile_high_enabled = H264_ENCODER_STARTWITH_PROFILE_HIGH;
    // -- set inital value for H264 encoder profile --

    // "ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow", "placebo"

    if (global_h264_enc_profile_high_enabled == 1) {
        if (x264_param_default_preset(&param, "superfast", "zerolatency,fastdecode") < 0) {
            // goto fail;
        }
    } else {
        if (x264_param_default_preset(&param, "ultrafast", "zerolatency,fastdecode") < 0) {
            // goto fail;
        }
    }

    /* Configure non-default params */
    // param.i_bitdepth = 8;
    param.i_csp = X264_CSP_I420;
    param.i_width  = 1920;
    param.i_height = 1080;
    vc->h264_enc_width = param.i_width;
    vc->h264_enc_height = param.i_height;

    param.i_threads = X264_ENCODER_THREADS;
    param.b_sliced_threads = true;
    param.i_slice_count = X264_ENCODER_SLICES;

    param.b_deterministic = false;
    //#// param.i_sync_lookahead = 0;
    //#// param.i_lookahead_threads = 0;
    //**// param.b_intra_refresh = 1;
    param.i_bframe = 0;
    // param.b_open_gop = 20;
    param.i_keyint_max = VIDEO_MAX_KF_H264;
    // param.rc.i_rc_method = X264_RC_CRF; // X264_RC_ABR;
    // param.i_nal_hrd = X264_NAL_HRD_CBR;

    //#// param.i_frame_reference = 1;

    param.b_vfr_input = 1; /* VFR input.  If 1, use timebase and timestamps for ratecontrol purposes.
                            * If 0, use fps only. */
    param.i_timebase_num = 1;       // 1 ms = timebase units = (1/1000)s
    param.i_timebase_den = 1000;   // 1 ms = timebase units = (1/1000)s
    param.b_repeat_headers = 1;
    param.b_annexb = 1;

    param.rc.f_rate_tolerance = VIDEO_F_RATE_TOLERANCE_H264;
    param.rc.i_vbv_buffer_size = VIDEO_BITRATE_INITIAL_VALUE_H264 * VIDEO_BUF_FACTOR_H264;
    param.rc.i_vbv_max_bitrate = VIDEO_BITRATE_INITIAL_VALUE_H264 * 1;
    // param.rc.i_bitrate = VIDEO_BITRATE_INITIAL_VALUE_H264 * VIDEO_BITRATE_FACTOR_H264;

    param.rc.i_qp_min = 3;
    param.rc.i_qp_max = 51; // max quantizer for x264

    vc->h264_enc_bitrate = VIDEO_BITRATE_INITIAL_VALUE_H264 * 1000;

    param.rc.b_stat_read = 0;
    param.rc.b_stat_write = 0;

#if 0
    x264_param_apply_fastfirstpass(&param);
#endif

    /* Apply profile restrictions. */

    if (global_h264_enc_profile_high_enabled == 1) {
        if (x264_param_apply_profile(&param,
                                     "high") < 0) { // "baseline", "main", "high", "high10", "high422", "high444"
            // goto fail;
            LOGGER_WARNING(log, "h264: setting high encoder failed");
        } else {
            LOGGER_WARNING(log, "h264: setting high encoder OK");
        }
    } else {
        if (x264_param_apply_profile(&param,
                                     "baseline") < 0) { // "baseline", "main", "high", "high10", "high422", "high444"
            // goto fail;
            LOGGER_WARNING(log, "h264: setting BASELINE encoder failed");
        } else {
            LOGGER_WARNING(log, "h264: setting BASELINE encoder OK");
        }
    }

    if (x264_picture_alloc(&(vc->h264_in_pic), param.i_csp, param.i_width, param.i_height) < 0) {
        // goto fail;
    }

    // vc->h264_in_pic.img.plane[0] --> Y
    // vc->h264_in_pic.img.plane[1] --> U
    // vc->h264_in_pic.img.plane[2] --> V

    vc->h264_encoder = x264_encoder_open(&param);

#else

    vc->h264_encoder = NULL;

// ------ ffmpeg encoder ------
    AVCodec *codec2 = NULL;
    vc->h264_encoder2 = NULL;

    // avcodec_register_all();

    codec2 = NULL;

#ifdef RAPI_HWACCEL_ENC
    codec2 = avcodec_find_encoder_by_name(H264_WANT_ENCODER_NAME);

    if (!codec2) {
        LOGGER_WARNING(log, "codec not found HW Accel H264 on encoder, trying software decoder ...");
        codec2 = avcodec_find_encoder_by_name("libx264");
    } else {
        LOGGER_ERROR(log, "FOUND: *HW Accel* H264 encoder: %s", H264_WANT_ENCODER_NAME);
    }

#else
    codec2 = avcodec_find_encoder_by_name("libx264");
#endif

    vc->h264_encoder2 = avcodec_alloc_context3(codec2);

    vc->h264_out_pic2 = av_packet_alloc();

    // -- set inital value for H264 encoder profile --
    global_h264_enc_profile_high_enabled = H264_ENCODER_STARTWITH_PROFILE_HIGH;
    // -- set inital value for H264 encoder profile --

    if (global_h264_enc_profile_high_enabled == 1) {
        av_opt_set(vc->h264_encoder2->priv_data, "profile", "high", 0);
        vc->h264_encoder2->profile               = FF_PROFILE_H264_HIGH; // FF_PROFILE_H264_HIGH;
        // av_opt_set(vc->h264_encoder2->priv_data, "level", "4.0", AV_OPT_SEARCH_CHILDREN);
        // vc->h264_encoder2->level = 40; // 4.0
    } else {
        av_opt_set(vc->h264_encoder2->priv_data, "profile", "baseline", 0);
        vc->h264_encoder2->profile               = FF_PROFILE_H264_BASELINE; // FF_PROFILE_H264_HIGH;
        // av_opt_set(vc->h264_encoder2->priv_data, "level", "4.0", AV_OPT_SEARCH_CHILDREN);
        // vc->h264_encoder2->level = 40; // 4.0
    }

#if 0
    //print
    char *val_str = (char *)av_malloc(20000);
    av_opt_get(vc->h264_encoder2->priv_data, "preset", 0, (uint8_t **)&val_str);
    LOGGER_ERROR(log, "preset val: %s", val_str);
    av_opt_get(vc->h264_encoder2->priv_data, "tune", 0, (uint8_t **)&val_str);
    LOGGER_ERROR(log, "tune val: %s", val_str);
    av_opt_get(vc->h264_encoder2->priv_data, "profile", 0, (uint8_t **)&val_str);
    LOGGER_ERROR(log, "profile val: %s", val_str);
    av_opt_get(vc->h264_encoder2->priv_data, "level", 0, (uint8_t **)&val_str);
    LOGGER_ERROR(log, "level val: %s", val_str);
    av_free(val_str);
#endif

    av_opt_set(vc->h264_encoder2->priv_data, "annex_b", "1", 0);
    av_opt_set(vc->h264_encoder2->priv_data, "repeat_headers", "1", 0);
    av_opt_set(vc->h264_encoder2->priv_data, "tune", "zerolatency", 0);
    av_opt_set(vc->h264_encoder2->priv_data, "b", "100000", 0);
    av_opt_set(vc->h264_encoder2->priv_data, "bitrate", "100000", 0);
    // av_opt_set_int(vc->h264_encoder2->priv_data, "minrate", 100000, 0);
    // av_opt_set_int(vc->h264_encoder2->priv_data, "maxrate", (int)((float)100000 * H264_ENCODE_MAX_BITRATE_OVER_ALLOW), 0);

    av_opt_set_int(vc->h264_encoder2->priv_data, "cbr", true, 0);
    av_opt_set(vc->h264_encoder2->priv_data, "rc", "cbr", 0);
    av_opt_set_int(vc->h264_encoder2->priv_data, "delay", 0, 0);
    // av_opt_set_int(vc->h264_encoder2->priv_data, "rc-lookahead", 0, 0);
    av_opt_set(vc->h264_encoder2->priv_data, "preset", "llhp", 0);
    av_opt_set_int(vc->h264_encoder2->priv_data, "bf", 0, 0);
    av_opt_set_int(vc->h264_encoder2->priv_data, "qmin", 3, 0);
    av_opt_set_int(vc->h264_encoder2->priv_data, "qmax", 51, 0);
    av_opt_set(vc->h264_encoder2->priv_data, "forced-idr", "true", 0);
    av_opt_set_int(vc->h264_encoder2->priv_data, "zerolatency", 1, AV_OPT_SEARCH_CHILDREN);
    //y// av_opt_set_int(vc->h264_encoder2->priv_data, "refs", 0, 0);

    av_opt_set_int(vc->h264_encoder2->priv_data, "threads", X264_ENCODER_THREADS, 0);

    if (X264_ENCODER_SLICES > 0) {
        av_opt_set(vc->h264_encoder2->priv_data, "sliced_threads", "1", 0);
    } else {
        av_opt_set(vc->h264_encoder2->priv_data, "sliced_threads", "0", 0);
    }

    av_opt_set_int(vc->h264_encoder2->priv_data, "slice_count", X264_ENCODER_SLICES, 0);

    /* put sample parameters */
    vc->h264_encoder2->bit_rate = 100 * 1000;
    vc->h264_enc_bitrate = 100 * 1000;

    /* resolution must be a multiple of two */
    vc->h264_encoder2->width = 1920;
    vc->h264_encoder2->height = 1080;

    vc->h264_enc_width = vc->h264_encoder2->width;
    vc->h264_enc_height = vc->h264_encoder2->height;

    vc->h264_encoder2->gop_size = VIDEO_MAX_KF_H264;
    // vc->h264_encoder2->keyint_min = VIDEO_MAX_KF_H264;
    vc->h264_encoder2->max_b_frames = 0;
    vc->h264_encoder2->pix_fmt = AV_PIX_FMT_YUV420P;

    vc->h264_encoder2->time_base.num = 1;
    vc->h264_encoder2->time_base.den = 1000;

    // without these it won't work !! ---------------------
    vc->h264_encoder2->time_base = (AVRational) {
        40, 1000
    };
    vc->h264_encoder2->framerate = (AVRational) {
        1000, 40
    };
    // without these it won't work !! ---------------------

    // vc->h264_encoder2->flags2 |= AV_CODEC_FLAG2_LOCAL_HEADER;
    // vc->h264_encoder2->flags2 |= AV_CODEC_FLAG2_FAST;

    AVDictionary *opts = NULL;

    if (avcodec_open2(vc->h264_encoder2, codec2, &opts) < 0) {
        LOGGER_ERROR(log, "could not open codec H264 on encoder");
    }

    av_dict_free(&opts);


// ------ ffmpeg encoder ------

#endif



//    goto good;

//fail:
//    vc->h264_encoder = NULL;

//good:

    // ENCODER -------


    // DECODER -------
    AVCodec *codec = NULL;
    vc->h264_decoder = NULL;

    // avcodec_register_all();

    codec = NULL;

#ifdef RAPI_HWACCEL_DEC

    codec = avcodec_find_decoder_by_name(H264_WANT_DECODER_NAME);

    if (!codec) {
        LOGGER_WARNING(log, "codec not found HW Accel H264 on decoder, trying software decoder ...");
        codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    } else {
        LOGGER_WARNING(log, "FOUND: *HW Accel* H264 on decoder");
    }

#else
    codec = avcodec_find_decoder(AV_CODEC_ID_H264);
#endif


    if (!codec) {
        LOGGER_WARNING(log, "codec not found H264 on decoder");
    }

    vc->h264_decoder = avcodec_alloc_context3(codec);

    if (codec)
    {
        if (codec->capabilities & AV_CODEC_CAP_TRUNCATED) {
            vc->h264_decoder->flags |= AV_CODEC_FLAG_TRUNCATED; /* we do not send complete frames */
        }

        if (codec->capabilities & AV_CODEC_FLAG_LOW_DELAY) {
            vc->h264_decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
        }

        // vc->h264_decoder->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
        vc->h264_decoder->flags |= AV_CODEC_FLAG2_SHOW_ALL;
        // vc->h264_decoder->flags2 |= AV_CODEC_FLAG2_FAST;
        // vc->h264_decoder->flags |= AV_CODEC_FLAG_TRUNCATED;
        // vc->h264_decoder->flags2 |= AV_CODEC_FLAG2_CHUNKS;

        if (H264_DECODER_THREADS > 0) {
            if (codec->capabilities & AV_CODEC_CAP_SLICE_THREADS) {
                vc->h264_decoder->thread_count = H264_DECODER_THREADS;
                vc->h264_decoder->thread_type = FF_THREAD_SLICE;
                vc->h264_decoder->active_thread_type = FF_THREAD_SLICE;
            }

            if (H264_DECODER_THREAD_FRAME_ACTIVE == 1) {
                if (codec->capabilities & AV_CODEC_CAP_FRAME_THREADS) {
                    vc->h264_decoder->thread_count = H264_DECODER_THREADS;
                    vc->h264_decoder->thread_type |= FF_THREAD_FRAME;
                    vc->h264_decoder->active_thread_type |= FF_THREAD_FRAME;
                }
            }
        }

    #if (defined (HW_CODEC_CONFIG_RPI3_TBW_TV) || defined (HW_CODEC_CONFIG_RPI3_TBW_BIDI)) && defined (RAPI_HWACCEL_DEC)
        LOGGER_WARNING(log, "setting up h264_mmal decoder ...");
        av_opt_set_int(vc->h264_decoder->priv_data, "extra_buffers)", 20, AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(vc->h264_decoder->priv_data, "extra_decoder_buffers)", 20, AV_OPT_SEARCH_CHILDREN);
        LOGGER_WARNING(log, "extra_buffers, extra_decoder_buffers");
    #endif


        vc->h264_decoder->refcounted_frames = 0;
        /*   When AVCodecContext.refcounted_frames is set to 0, the returned
        *             reference belongs to the decoder and is valid only until the
        *             next call to this function or until closing or flushing the
        *             decoder. The caller may not write to it.
        */

        vc->h264_decoder->delay = 0;
        av_opt_set_int(vc->h264_decoder->priv_data, "delay", 0, AV_OPT_SEARCH_CHILDREN);

        vc->h264_decoder->time_base = (AVRational) {
            40, 1000
        };
        vc->h264_decoder->framerate = (AVRational) {
            1000, 40
        };




    #ifdef _TRIFA_CODEC_DECODER_

        LOGGER_WARNING(log, "setting up h264_mediacodec decoder ...");


        const uint8_t sps[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x80, 0x0C, 0xE4, 0x40, 0xA0, 0xFD, 0x00, 0xDA, 0x14, 0x26, 0xA0};
        const uint8_t pps[] = {0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x38, 0x80};
        const size_t sps_pps_size = sizeof(sps) + sizeof(pps);

        vc->h264_decoder->extradata = (uint8_t *)av_mallocz(sps_pps_size + AV_INPUT_BUFFER_PADDING_SIZE);
        vc->h264_decoder->extradata_size = sps_pps_size;
        // memset(&vc->h264_decoder->extradata[vc->h264_decoder->extradata_size], 0, AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(vc->h264_decoder->extradata, sps, sizeof(sps));
        memcpy(vc->h264_decoder->extradata + sizeof(sps), pps, sizeof(pps));


        vc->h264_decoder->codec_type = AVMEDIA_TYPE_VIDEO;
        vc->h264_decoder->codec_id   = AV_CODEC_ID_H264;

        if (codec->capabilities & AV_CODEC_CAP_SLICE_THREADS) {
            vc->h264_decoder->thread_count = 1;
            vc->h264_decoder->thread_type = FF_THREAD_SLICE;
            vc->h264_decoder->active_thread_type = FF_THREAD_SLICE;
        }

        //if (codec->capabilities & AV_CODEC_CAP_FRAME_THREADS) {
        //    vc->h264_decoder->thread_count = 1;
        //    vc->h264_decoder->thread_type |= FF_THREAD_FRAME;
        //    vc->h264_decoder->active_thread_type |= FF_THREAD_FRAME;
        //}

    /*
        vc->h264_decoder->codec_tag  = 0x31637661; // ('1'<<24) + ('c'<<16) + ('v'<<8) + 'a';
        vc->h264_decoder->bit_rate              = 2500 * 1000;
        // codec->bits_per_coded_sample = par->bits_per_coded_sample;
        vc->h264_decoder->bits_per_raw_sample   = 8;
        vc->h264_decoder->profile               = FF_PROFILE_H264_HIGH;
        vc->h264_decoder->level                 = 40;
        vc->h264_decoder->has_b_frames          = 2;
    */


        vc->h264_decoder->pix_fmt                = AV_PIX_FMT_YUV420P;
        vc->h264_decoder->width                  = 480;
        vc->h264_decoder->height                 = 640;

        /*
            codec->field_order            = par->field_order;
            codec->color_range            = par->color_range;
            codec->color_primaries        = par->color_primaries;
            codec->color_trc              = par->color_trc;
            codec->colorspace             = par->color_space;
            codec->chroma_sample_location = par->chroma_location;
            codec->sample_aspect_ratio    = par->sample_aspect_ratio;
            codec->has_b_frames           = par->video_delay;
        */

        LOGGER_WARNING(log, "setting up h264_mediacodec decoder ... DONE");

        av_log_set_level(AV_LOG_ERROR);
        // av_log_set_level(AV_LOG_DEBUG);
        global__log = log;
        // av_log_set_callback(my_log_callback);

    #endif



        if (avcodec_open2(vc->h264_decoder, codec, NULL) < 0) {
            LOGGER_WARNING(log, "could not open codec H264 on decoder");
        }

        vc->h264_decoder->refcounted_frames = 0;
        /*   When AVCodecContext.refcounted_frames is set to 0, the returned
        *             reference belongs to the decoder and is valid only until the
        *             next call to this function or until closing or flushing the
        *             decoder. The caller may not write to it.
        */
    }


    // DECODER -------

    return vc;
}

int vc_reconfigure_encoder_h264(Logger *log, VCSession *vc, uint32_t bit_rate,
                                uint16_t width, uint16_t height,
                                int16_t kf_max_dist)
{
    if (!vc) {
        return -1;
    }

    if (global_h264_enc_profile_high_enabled_switch == 1) {
        global_h264_enc_profile_high_enabled_switch = 0;
        kf_max_dist = -2;
        LOGGER_WARNING(log, "switching H264 encoder profile ...");
    }

    if ((vc->h264_enc_width == width) &&
            (vc->h264_enc_height == height) &&
            (vc->video_rc_max_quantizer == vc->video_rc_max_quantizer_prev) &&
            (vc->video_rc_min_quantizer == vc->video_rc_min_quantizer_prev) &&
            (vc->h264_enc_bitrate != bit_rate) &&
            (kf_max_dist != -2)) {
        // only bit rate changed

        if (vc->h264_encoder) {

            x264_param_t param;
            x264_encoder_parameters(vc->h264_encoder, &param);

            LOGGER_DEBUG(log, "vc_reconfigure_encoder_h264:vb=%d [bitrate only]", (int)(bit_rate / 1000));

            param.rc.f_rate_tolerance = VIDEO_F_RATE_TOLERANCE_H264;
            param.rc.i_vbv_buffer_size = (bit_rate / 1000) * VIDEO_BUF_FACTOR_H264;
            param.rc.i_vbv_max_bitrate = (bit_rate / 1000) * 1;

            // param.rc.i_bitrate = (bit_rate / 1000) * VIDEO_BITRATE_FACTOR_H264;
            vc->h264_enc_bitrate = bit_rate;

            int res = x264_encoder_reconfig(vc->h264_encoder, &param);
        } else {

#ifndef X264_ENCODE_USED
            vc->h264_encoder2->bit_rate = bit_rate;
            vc->h264_enc_bitrate = bit_rate;

            av_opt_set_int(vc->h264_encoder2->priv_data, "b", bit_rate, 0);
            av_opt_set_int(vc->h264_encoder2->priv_data, "bitrate", bit_rate, 0);
            // av_opt_set_int(vc->h264_encoder2->priv_data, "minrate", bit_rate, 0);
            // av_opt_set_int(vc->h264_encoder2->priv_data, "maxrate", (int)((float)bit_rate * H264_ENCODE_MAX_BITRATE_OVER_ALLOW), 0);
#endif

        }


    } else {
        if ((vc->h264_enc_width != width) ||
                (vc->h264_enc_height != height) ||
                (vc->h264_enc_bitrate != bit_rate) ||
                (vc->video_rc_max_quantizer != vc->video_rc_max_quantizer_prev) ||
                (vc->video_rc_min_quantizer != vc->video_rc_min_quantizer_prev) ||
                (kf_max_dist == -2)
           ) {
            // input image size changed

#ifdef X264_ENCODE_USED

            if (vc->h264_encoder) {

                x264_param_t param;

                if (global_h264_enc_profile_high_enabled == 1) {
                    if (x264_param_default_preset(&param, "superfast", "zerolatency,fastdecode") < 0) {
                        // goto fail;
                    }
                } else {
                    if (x264_param_default_preset(&param, "ultrafast", "zerolatency,fastdecode") < 0) {
                        // goto fail;
                    }
                }

                /* Configure non-default params */
                // param.i_bitdepth = 8;
                param.i_csp = X264_CSP_I420;
                param.i_width  = width;
                param.i_height = height;
                vc->h264_enc_width = param.i_width;
                vc->h264_enc_height = param.i_height;
                param.i_threads = X264_ENCODER_THREADS;
                param.b_sliced_threads = true;
                param.i_slice_count = X264_ENCODER_SLICES;

                param.b_deterministic = false;
                //#// param.i_sync_lookahead = 0;
                //#// param.i_lookahead_threads = 0;
                //**// param.b_intra_refresh = 1;
                param.i_bframe = 0;
                // param.b_open_gop = 20;
                param.i_keyint_max = VIDEO_MAX_KF_H264;
                // param.rc.i_rc_method = X264_RC_ABR;
                //#// param.i_frame_reference = 1;

                param.b_vfr_input = 1; /* VFR input.  If 1, use timebase and timestamps for ratecontrol purposes.
                            * If 0, use fps only. */
                param.i_timebase_num = 1;       // 1 ms = timebase units = (1/1000)s
                param.i_timebase_den = 1000;   // 1 ms = timebase units = (1/1000)s
                param.b_repeat_headers = 1;
                param.b_annexb = 1;

                LOGGER_ERROR(log, "vc_reconfigure_encoder_h264:vb=%d", (int)(bit_rate / 1000));

                param.rc.f_rate_tolerance = VIDEO_F_RATE_TOLERANCE_H264;
                param.rc.i_vbv_buffer_size = (bit_rate / 1000) * VIDEO_BUF_FACTOR_H264;
                param.rc.i_vbv_max_bitrate = (bit_rate / 1000) * 1;

                if ((vc->video_rc_min_quantizer >= 0) &&
                        (vc->video_rc_min_quantizer < vc->video_rc_max_quantizer) &&
                        (vc->video_rc_min_quantizer < 51)) {
                    param.rc.i_qp_min = vc->video_rc_min_quantizer;
                }

                if ((vc->video_rc_max_quantizer >= 0) &&
                        (vc->video_rc_min_quantizer < vc->video_rc_max_quantizer) &&
                        (vc->video_rc_max_quantizer < 51)) {
                    param.rc.i_qp_max = vc->video_rc_max_quantizer;
                }

                // param.rc.i_bitrate = (bit_rate / 1000) * VIDEO_BITRATE_FACTOR_H264;
                vc->h264_enc_bitrate = bit_rate;

                param.rc.b_stat_read = 0;
                param.rc.b_stat_write = 0;
#if 0
                x264_param_apply_fastfirstpass(&param);
#endif

                /* Apply profile restrictions. */
                if (global_h264_enc_profile_high_enabled == 1) {
                    if (x264_param_apply_profile(&param,
                                                 "high") < 0) { // "baseline", "main", "high", "high10", "high422", "high444"
                        // goto fail;
                        LOGGER_WARNING(log, "h264: setting high encoder failed (2)");
                    } else {
                        LOGGER_WARNING(log, "h264: setting high encoder OK (2)");
                    }
                } else {
                    if (x264_param_apply_profile(&param,
                                                 "baseline") < 0) { // "baseline", "main", "high", "high10", "high422", "high444"
                        // goto fail;
                        LOGGER_WARNING(log, "h264: setting BASELINE encoder failed (2)");
                    } else {
                        LOGGER_WARNING(log, "h264: setting BASELINE encoder OK (2)");
                    }
                }

                LOGGER_ERROR(log, "H264: reconfigure encoder:001: w:%d h:%d w_new:%d h_new:%d BR:%d\n",
                             vc->h264_enc_width,
                             vc->h264_enc_height,
                             width,
                             height,
                             (int)bit_rate);

                // free old stuff ---------
                x264_encoder_close(vc->h264_encoder);
                x264_picture_clean(&(vc->h264_in_pic));
                // free old stuff ---------

                // alloc with new values -------
                if (x264_picture_alloc(&(vc->h264_in_pic), param.i_csp, param.i_width, param.i_height) < 0) {
                    // goto fail;
                }

                vc->h264_encoder = x264_encoder_open(&param);
                // alloc with new values -------

                vc->video_rc_max_quantizer_prev = vc->video_rc_max_quantizer;
                vc->video_rc_min_quantizer_prev = vc->video_rc_min_quantizer;

            }

#else

            // --- ffmpeg encoder ---
            avcodec_free_context(&vc->h264_encoder2);


            AVCodec *codec2 = NULL;
            vc->h264_encoder2 = NULL;

            // avcodec_register_all();

            codec2 = NULL;

#ifdef RAPI_HWACCEL_ENC
            codec2 = avcodec_find_encoder_by_name(H264_WANT_ENCODER_NAME);

            if (!codec2) {
                LOGGER_WARNING(log, "codec not found HW Accel H264 on encoder, trying software decoder ...");
                codec2 = avcodec_find_encoder_by_name("libx264");
            } else {
                LOGGER_ERROR(log, "FOUND: *HW Accel* H264 encoder: %s", H264_WANT_ENCODER_NAME);
            }

#else
            codec2 = avcodec_find_encoder_by_name("libx264");
#endif

            vc->h264_encoder2 = avcodec_alloc_context3(codec2);

            if (global_h264_enc_profile_high_enabled == 1) {
                av_opt_set(vc->h264_encoder2->priv_data, "profile", "high", 0);
                vc->h264_encoder2->profile               = FF_PROFILE_H264_HIGH; // FF_PROFILE_H264_HIGH;
                // av_opt_set(vc->h264_encoder2->priv_data, "level", "4.0", AV_OPT_SEARCH_CHILDREN);
                // vc->h264_encoder2->level = 40; // 4.0
                LOGGER_WARNING(log, "switching H264 encoder profile ... HIGH");

            } else {
                av_opt_set(vc->h264_encoder2->priv_data, "profile", "baseline", 0);
                vc->h264_encoder2->profile               = FF_PROFILE_H264_BASELINE; // FF_PROFILE_H264_HIGH;
                // av_opt_set(vc->h264_encoder2->priv_data, "level", "4.0", AV_OPT_SEARCH_CHILDREN);
                // vc->h264_encoder2->level = 40; // 4.0
                LOGGER_WARNING(log, "switching H264 encoder profile ... baseline");
            }

            av_opt_set(vc->h264_encoder2->priv_data, "annex_b", "1", 0);
            av_opt_set(vc->h264_encoder2->priv_data, "repeat_headers", "1", 0);
            av_opt_set(vc->h264_encoder2->priv_data, "tune", "zerolatency", 0);
            av_opt_set_int(vc->h264_encoder2->priv_data, "b", bit_rate, 0);
            av_opt_set_int(vc->h264_encoder2->priv_data, "bitrate", bit_rate, 0);
            // av_opt_set_int(vc->h264_encoder2->priv_data, "minrate", bit_rate, 0);
            // av_opt_set_int(vc->h264_encoder2->priv_data, "maxrate", (int)((float)bit_rate * H264_ENCODE_MAX_BITRATE_OVER_ALLOW), 0);

            av_opt_set_int(vc->h264_encoder2->priv_data, "cbr", true, 0);
            av_opt_set(vc->h264_encoder2->priv_data, "rc", "cbr", 0);
            av_opt_set_int(vc->h264_encoder2->priv_data, "delay", 0, 0);
            // av_opt_set_int(vc->h264_encoder2->priv_data, "rc-lookahead", 0, 0);
            av_opt_set(vc->h264_encoder2->priv_data, "preset", "llhp", 0);
            av_opt_set_int(vc->h264_encoder2->priv_data, "bf", 0, 0);
            av_opt_set_int(vc->h264_encoder2->priv_data, "qmin", 3, 0);
            av_opt_set_int(vc->h264_encoder2->priv_data, "qmax", 51, 0);
            av_opt_set(vc->h264_encoder2->priv_data, "forced-idr", "true", 0);
            av_opt_set_int(vc->h264_encoder2->priv_data, "zerolatency", 1, AV_OPT_SEARCH_CHILDREN);
            //y// av_opt_set_int(vc->h264_encoder2->priv_data, "refs", 1, 0);

            av_opt_set_int(vc->h264_encoder2->priv_data, "threads", X264_ENCODER_THREADS, 0);

            if (X264_ENCODER_SLICES > 0) {
                av_opt_set(vc->h264_encoder2->priv_data, "sliced_threads", "1", 0);
            } else {
                av_opt_set(vc->h264_encoder2->priv_data, "sliced_threads", "0", 0);
            }

            av_opt_set_int(vc->h264_encoder2->priv_data, "slice_count", X264_ENCODER_SLICES, 0);

            /* put sample parameters */
            vc->h264_encoder2->bit_rate = bit_rate;
            vc->h264_enc_bitrate = bit_rate;

            /* resolution must be a multiple of two */
            vc->h264_encoder2->width = width;
            vc->h264_encoder2->height = height;

            vc->h264_enc_width = vc->h264_encoder2->width;
            vc->h264_enc_height = vc->h264_encoder2->height;

            vc->h264_encoder2->gop_size = VIDEO_MAX_KF_H264;
            // vc->h264_encoder2->keyint_min = VIDEO_MAX_KF_H264;
            vc->h264_encoder2->max_b_frames = 0;
            vc->h264_encoder2->pix_fmt = AV_PIX_FMT_YUV420P;

            vc->h264_encoder2->time_base.num = 1;
            vc->h264_encoder2->time_base.den = 1000;

            // without these it won't work !! ---------------------
            vc->h264_encoder2->time_base = (AVRational) {
                40, 1000
            };
            vc->h264_encoder2->framerate = (AVRational) {
                1000, 40
            };
            // without these it won't work !! ---------------------

            /*
            av_opt_get(pCodecCtx->priv_data,"preset",0,(uint8_t **)&val_str);
                    printf("preset val: %s\n",val_str);
                    av_opt_get(pCodecCtx->priv_data,"tune",0,(uint8_t **)&val_str);
                    printf("tune val: %s\n",val_str);
                    av_opt_get(pCodecCtx->priv_data,"profile",0,(uint8_t **)&val_str);
                    printf("profile val: %s\n",val_str);
            av_free(val_str);
            */

            // vc->h264_encoder2->flags2 |= AV_CODEC_FLAG2_LOCAL_HEADER;
            // vc->h264_encoder2->flags2 |= AV_CODEC_FLAG2_FAST;

            AVDictionary *opts = NULL;

            if (avcodec_open2(vc->h264_encoder2, codec2, NULL) < 0) {
                LOGGER_ERROR(log, "could not open codec H264 on encoder");
            }

            av_dict_free(&opts);

            // --- ffmpeg encoder ---

#endif

        }
    }

    return 0;
}

void get_info_from_sps(const Messenger *m, VCSession *vc, const Logger *log,
                       const uint8_t data[], const uint32_t data_len)
{

    if (data_len > 7) {
        LOGGER_DEBUG(log, "SPS:len=%d bytes:%d %d %d %d %d %d %d %d", data_len, data[0], data[1], data[2], data[3], data[4],
                     data[5], data[6], data[7]);

        if (
            (data[0] == 0x00)
            &&
            (data[1] == 0x00)
            &&
            (data[2] == 0x00)
            &&
            (data[3] == 0x01)
            &&
            ((data[4] & 0x1F) == 7) // only the lower 5bits of the 4th byte denote the NAL type
            // 7 --> SPS
            // 8 --> PPS
            // (data[4] == 0x67)
        ) {
            // parse only every 5 seconds
            if ((vc->last_parsed_h264_sps_ts + 5000) < current_time_monotonic(m->mono_time)) {
                vc->last_parsed_h264_sps_ts = current_time_monotonic(m->mono_time);

                // we found a NAL unit containing the SPS
                uint8_t h264_profile = data[5];
                uint8_t h264_constraint_set0_flag = ((data[6] >> 3)  & 0x01);
                uint8_t h264_constraint_set3_flag = (data[6]  & 0x01);
                uint8_t h264_level = data[7];

                if ((h264_profile == 66) && (h264_constraint_set3_flag = 0)) {
                    LOGGER_DEBUG(log, "profile=%s level=%d", "baseline", h264_level);
                    vc->parsed_h264_sps_profile_i = h264_profile;
                } else if ((h264_profile == 66) && (h264_constraint_set3_flag = 1)) {
                    LOGGER_DEBUG(log, "profile=%s level=%d", "contrained baseline", h264_level);
                    vc->parsed_h264_sps_profile_i = h264_profile;
                } else if ((h264_profile == 77) && (h264_constraint_set0_flag = 0)) {
                    LOGGER_DEBUG(log, "profile=%s level=%d", "main", h264_level);
                    vc->parsed_h264_sps_profile_i = h264_profile;
                } else if ((h264_profile == 77) && (h264_constraint_set0_flag = 1)) {
                    LOGGER_DEBUG(log, "profile=%s level=%d", "extended", h264_level);
                    vc->parsed_h264_sps_profile_i = h264_profile;
                } else if (h264_profile == 100) {
                    LOGGER_DEBUG(log, "profile=%s level=%d", "high", h264_level);
                    vc->parsed_h264_sps_profile_i = h264_profile;
                } else if (h264_profile == 110) {
                    LOGGER_DEBUG(log, "profile=%s level=%d", "high10", h264_level);
                    vc->parsed_h264_sps_profile_i = h264_profile;
                } else if (h264_profile == 122) {
                    LOGGER_DEBUG(log, "profile=%s level=%d", "high422", h264_level);
                    vc->parsed_h264_sps_profile_i = h264_profile;
                } else if (h264_profile == 244) {
                    LOGGER_DEBUG(log, "profile=%s level=%d", "high444", h264_level);
                    vc->parsed_h264_sps_profile_i = h264_profile;
                } else {
                    LOGGER_DEBUG(log, "profile=%s level=%d", "unkwn", h264_level);
                    vc->parsed_h264_sps_profile_i = 0;
                }

                vc->parsed_h264_sps_level_i = h264_level;
            }
        }
    }
}

//int32_t global_first_video_frame_data = 0;
int32_t global_decoder_delay_counter = 0;

void decode_frame_h264(VCSession *vc, Messenger *m, uint8_t skip_video_flag, uint64_t *a_r_timestamp,
                       uint64_t *a_l_timestamp,
                       uint64_t *v_r_timestamp, uint64_t *v_l_timestamp,
                       const struct RTPHeader *header_v3,
                       struct RTPMessage *p, vpx_codec_err_t rc,
                       uint32_t full_data_len,
                       uint8_t *ret_value)
{

    LOGGER_DEBUG(vc->log, "decode_frame_h264:len=%d", full_data_len);

    if (p == NULL)
    {
        LOGGER_DEBUG(vc->log, "decode_frame_h264:NO data");
        return;
    }

    if (p->data == NULL)
    {
        LOGGER_DEBUG(vc->log, "decode_frame_h264:NO data p->data");
        return;
    }

    if (full_data_len < 1)
    {
        LOGGER_DEBUG(vc->log, "decode_frame_h264:not enough data");
        free(p);
        p = NULL;
        return;
    }

    if (vc->h264_decoder == NULL)
    {
        LOGGER_DEBUG(vc->log, "vc->h264_decoder:not ready");
        free(p);
        p = NULL;
        return;
    }


    if (vc->vcb_h264 != NULL)
    {
        // call callback function to give H264 buffer directly to the client
        vc->vcb_h264(vc->av, vc->friend_number, p->data, full_data_len, vc->vcb_h264_user_data);
        free(p);
        p = NULL;
        return;
    }

#if 0

    if ((p) && (p->data)) {
        global_first_video_frame_data++;

        if (global_first_video_frame_data == 250) {
            FILE *pFile = NULL;
            pFile = fopen("h264_frame.txt", "w");

            if (pFile) {
                fwrite(p->data, 1, full_data_len, pFile);

            }

            fclose(pFile);
            global_first_video_frame_data = 1;
        }
    }

#endif

    if ((p) && (p->data)) {
        get_info_from_sps(m, vc, vc->log, p->data, full_data_len);
    }

    /*
     For decoding, call avcodec_send_packet() to give the decoder raw
          compressed data in an AVPacket.


          For decoding, call avcodec_receive_frame(). On success, it will return
          an AVFrame containing uncompressed audio or video data.


     *   Repeat this call until it returns AVERROR(EAGAIN) or an error. The
     *   AVERROR(EAGAIN) return value means that new input data is required to
     *   return new output. In this case, continue with sending input. For each
     *   input frame/packet, the codec will typically return 1 output frame/packet,
     *   but it can also be 0 or more than 1.

     */

    AVPacket *compr_data = NULL;
    compr_data = av_packet_alloc();

    if (compr_data == NULL)
    {
        LOGGER_DEBUG(vc->log, "av_packet_alloc:ERROR");
        free(p);
        p = NULL;
        return;
    }

    uint64_t h_frame_record_timestamp = header_v3->frame_record_timestamp;

#if 0
    compr_data->pts = AV_NOPTS_VALUE;
    compr_data->dts = AV_NOPTS_VALUE;

    compr_data->duration = 0;
    compr_data->post = -1;
#endif

    // uint32_t start_time_ms = current_time_monotonic(m->mono_time);
    // HINT: dirty hack to add FF_INPUT_BUFFER_PADDING_SIZE bytes!! ----------
    uint8_t *tmp_buf = calloc(1, full_data_len + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(tmp_buf, p->data, full_data_len);
    // HINT: dirty hack to add FF_INPUT_BUFFER_PADDING_SIZE bytes!! ----------
    // uint32_t end_time_ms = current_time_monotonic(m->mono_time);
    // LOGGER_WARNING(vc->log, "decode_frame_h264:001: %d ms", (int)(end_time_ms - start_time_ms));

    compr_data->data = tmp_buf; // p->data;
    compr_data->size = (int)full_data_len; // hmm, "int" again

    if (header_v3->frame_record_timestamp > 0) {
        compr_data->dts = (int64_t)(header_v3->frame_record_timestamp) + 0;
        compr_data->pts = (int64_t)(header_v3->frame_record_timestamp) + 1;
        compr_data->duration = 0; // (int64_t)(header_v3->frame_record_timestamp) + 1; // 0;
    }

    /* ------------------------------------------------------- */
    /* ------------------------------------------------------- */
    /* HINT: this is the only part that takes all the time !!! */
    /* HINT: this is the only part that takes all the time !!! */
    /* HINT: this is the only part that takes all the time !!! */

    // uint32_t start_time_ms = current_time_monotonic(m->mono_time);

    /* ------------------------------------------------------- */
    /* ------------------------------------------------------- */
    // The input buffer, avpkt->data must be AV_INPUT_BUFFER_PADDING_SIZE larger than the actual
    // read bytes because some optimized bitstream readers read 32 or 64 bits at once and
    // could read over the end.
    /* ------------------------------------------------------- */
    /* ------------------------------------------------------- */

    int result_send_packet = avcodec_send_packet(vc->h264_decoder, compr_data);
    if (result_send_packet != 0)
    {
        LOGGER_DEBUG(vc->log, "avcodec_send_packet:ERROR=%d", result_send_packet);
        av_packet_free(&compr_data);
        free(tmp_buf);
        free(p);
        p = NULL;
        return;
    }
    // uint32_t end_time_ms = current_time_monotonic(m->mono_time);
    // if ((int)(end_time_ms - start_time_ms) > 4) {
    //    LOGGER_WARNING(vc->log, "decode_frame_h264:002: %d ms", (int)(end_time_ms - start_time_ms));
    //}

    /* HINT: this is the only part that takes all the time !!! */
    /* HINT: this is the only part that takes all the time !!! */
    /* HINT: this is the only part that takes all the time !!! */
    /* ------------------------------------------------------- */
    /* ------------------------------------------------------- */


    int ret_ = 0;

    while (ret_ >= 0) {

        // start_time_ms = current_time_monotonic(m->mono_time);
        AVFrame *frame = av_frame_alloc();
        if (frame == NULL)
        {
            // stop decoding
            break;
        }
        // end_time_ms = current_time_monotonic(m->mono_time);
        // LOGGER_WARNING(vc->log, "decode_frame_h264:003: %d ms", (int)(end_time_ms - start_time_ms));

        // start_time_ms = current_time_monotonic(m->mono_time);
        ret_ = avcodec_receive_frame(vc->h264_decoder, frame);
        // end_time_ms = current_time_monotonic(m->mono_time);
        // LOGGER_WARNING(vc->log, "decode_frame_h264:004: %d ms", (int)(end_time_ms - start_time_ms));

        // LOGGER_ERROR(vc->log, "H264:decoder:ret_=%d\n", (int)ret_);


        if (ret_ == AVERROR(EAGAIN) || ret_ == AVERROR_EOF) {
            // error
            av_frame_free(&frame);
            break;
        } else if (ret_ < 0) {
            // Error during decoding
            av_frame_free(&frame);
            break;
        } else if (ret_ == 0) {

            // LOGGER_ERROR(vc->log, "H264:decoder:fnum=%d\n", (int)vc->h264_decoder->frame_number);
            // LOGGER_ERROR(vc->log, "H264:decoder:linesize=%d\n", (int)frame->linesize[0]);
            // LOGGER_ERROR(vc->log, "H264:decoder:w=%d\n", (int)frame->width);
            // LOGGER_ERROR(vc->log, "H264:decoder:h=%d\n", (int)frame->height);


            /*
                pkt_pts
                PTS copied from the AVPacket that was decoded to produce this frame.

                pkt_dts
                DTS copied from the AVPacket that triggered returning this frame.
            */

            // calculate the real play delay (from toxcore-in to toxcore-out)
            // this seems to give incorrect values :-(
            if (header_v3->frame_record_timestamp > 0) {

                //vc->video_play_delay_real =
                //    (current_time_monotonic(m->mono_time) + vc->timestamp_difference_to_sender) -
                //    frame->pkt_pts;
                // use the calculated values instead
                //vc->video_play_delay_real = vc->video_play_delay;

                /*
                 * TODO: there is some memory issue in the log line. DO NOT ENABLE !! ---------
                                LOGGER_DEBUG(vc->log,
                                               "real play delay=%d header_v3->frame_record_timestamp=%d, mono=%d, vc->timestamp_difference_to_sender=%d frame->pkt_pts=%ld, frame->pkt_dts=%ld, frame->pts=%ld",
                                               (int)vc->video_play_delay_real,
                                               (int)header_v3->frame_record_timestamp,
                                               (int)current_time_monotonic(m->mono_time),
                                               (int)vc->timestamp_difference_to_sender,
                                               frame->pkt_pts,
                                               frame->pkt_dts,
                                               frame->pts
                                              );
                 *
                 * TODO: there is some memory issue in the log line. DO NOT ENABLE !! ---------
                 */



                /*


                MMAL H264 Decoder - problem ?
                =============================

                 fps    ms delay    ms between frames   frames cached
                ---------------------------------------------------------
                 10     450         100                 ~4.5
                 20     230         50                  ~4.6
                 25     190         40                  ~4.75

                */

                global_decoder_delay_counter++;

                if (global_decoder_delay_counter > 60) {
                    global_decoder_delay_counter = 0;
                    LOGGER_DEBUG(vc->log, "dec:delay=%ld",
                                 (long int)(h_frame_record_timestamp - frame->pkt_pts)
                                );
                }
            }

            // start_time_ms = current_time_monotonic(m->mono_time);
            if ((frame->data[0] != NULL) && (frame->data[1] != NULL) && (frame->data[2] != NULL))
            {

// -------- DEBUG:AUDIO/VIDEO DELAY/LATENCY --------
// -------- DEBUG:AUDIO/VIDEO DELAY/LATENCY --------
// -------- DEBUG:AUDIO/VIDEO DELAY/LATENCY --------
#if 0
                *v_r_timestamp = h_frame_record_timestamp;
                *v_l_timestamp = current_time_monotonic(m->mono_time);
#endif
// -------- DEBUG:AUDIO/VIDEO DELAY/LATENCY --------
// -------- DEBUG:AUDIO/VIDEO DELAY/LATENCY --------
// -------- DEBUG:AUDIO/VIDEO DELAY/LATENCY --------

                vc->vcb(vc->av, vc->friend_number, frame->width, frame->height,
                        (const uint8_t *)frame->data[0],
                        (const uint8_t *)frame->data[1],
                        (const uint8_t *)frame->data[2],
                        frame->linesize[0], frame->linesize[1],
                        frame->linesize[2], vc->vcb_user_data);
            }
            // end_time_ms = current_time_monotonic(m->mono_time);
            // LOGGER_WARNING(vc->log, "decode_frame_h264:005: %d ms", (int)(end_time_ms - start_time_ms));

        } else {
            // some other error
        }

        // start_time_ms = current_time_monotonic(m->mono_time);
        av_frame_free(&frame);
        // end_time_ms = current_time_monotonic(m->mono_time);
        // LOGGER_WARNING(vc->log, "decode_frame_h264:006: %d ms", (int)(end_time_ms - start_time_ms));
    }

    // start_time_ms = current_time_monotonic(m->mono_time);
    av_packet_free(&compr_data);
    // end_time_ms = current_time_monotonic(m->mono_time);
    // LOGGER_WARNING(vc->log, "decode_frame_h264:007: %d ms", (int)(end_time_ms - start_time_ms));

    // HINT: dirty hack to add FF_INPUT_BUFFER_PADDING_SIZE bytes!! ----------
    free(tmp_buf);
    // HINT: dirty hack to add FF_INPUT_BUFFER_PADDING_SIZE bytes!! ----------

    free(p);
}

int32_t global_encoder_delay_counter = 0;

uint32_t encode_frame_h264(ToxAV *av, uint32_t friend_number, uint16_t width, uint16_t height,
                           const uint8_t *y,
                           const uint8_t *u, const uint8_t *v, ToxAVCall *call,
                           uint64_t *video_frame_record_timestamp,
                           int vpx_encode_flags,
                           x264_nal_t **nal,
                           int *i_frame_size)
{

#ifdef X264_ENCODE_USED


    memcpy(call->video->h264_in_pic.img.plane[0], y, width * height);
    memcpy(call->video->h264_in_pic.img.plane[1], u, (width / 2) * (height / 2));
    memcpy(call->video->h264_in_pic.img.plane[2], v, (width / 2) * (height / 2));

    int i_nal;

    call->video->h264_in_pic.i_pts = (int64_t)(*video_frame_record_timestamp);

    if ((vpx_encode_flags & VPX_EFLAG_FORCE_KF) > 0) {
        call->video->h264_in_pic.i_type = X264_TYPE_IDR; // real full i-frame
        call->video->last_sent_keyframe_ts = current_time_monotonic(av->m->mono_time);
    } else {
        call->video->h264_in_pic.i_type = X264_TYPE_AUTO;
    }

    LOGGER_DEBUG(av->m->log, "X264 IN frame type=%d", (int)call->video->h264_in_pic.i_type);

    *i_frame_size = x264_encoder_encode(call->video->h264_encoder,
                                        nal,
                                        &i_nal,
                                        &(call->video->h264_in_pic),
                                        &(call->video->h264_out_pic));

    global_encoder_delay_counter++;

    if (global_encoder_delay_counter > 60) {
        global_encoder_delay_counter = 0;
        LOGGER_DEBUG(av->m->log, "enc:delay=%ld",
                     (long int)(call->video->h264_in_pic.i_pts - (int64_t)call->video->h264_out_pic.i_pts)
                    );
    }


    *video_frame_record_timestamp = (uint64_t)call->video->h264_out_pic.i_pts;




    if (IS_X264_TYPE_I(call->video->h264_out_pic.i_type)) {
        LOGGER_DEBUG(av->m->log, "X264 out frame type=%d", (int)call->video->h264_out_pic.i_type);
    }


    if (*i_frame_size < 0) {
        // some error
    } else if (*i_frame_size == 0) {
        // zero size output
    } else {
        // *nal->p_payload --> outbuf
        // *i_frame_size --> out size in bytes

        // -- WARN -- : this could crash !! ----
        // LOGGER_ERROR(av->m->log, "H264: i_frame_size=%d nal_buf=%p KF=%d\n",
        //             (int)*i_frame_size,
        //             (*nal)->p_payload,
        //             (int)call->video->h264_out_pic.b_keyframe
        //            );
        // -- WARN -- : this could crash !! ----

    }

    if (*nal == NULL) {
        //pthread_mutex_unlock(call->mutex_video);
        //goto END;
        return 1;
    }

    if ((*nal)->p_payload == NULL) {
        //pthread_mutex_unlock(call->mutex_video);
        //goto END;
        return 1;
    }

    return 0;

#else

    AVFrame *frame;
    int ret;
    uint32_t result = 1;

    frame = av_frame_alloc();

    frame->format = call->video->h264_encoder2->pix_fmt;
    frame->width  = width;
    frame->height = height;

    ret = av_frame_get_buffer(frame, 32);

    if (ret < 0) {
        LOGGER_ERROR(av->m->log, "av_frame_get_buffer:Could not allocate the video frame data");
    }

    /* make sure the frame data is writable */
    ret = av_frame_make_writable(frame);

    if (ret < 0) {
        LOGGER_ERROR(av->m->log, "av_frame_make_writable:ERROR");
    }

    frame->pts = (int64_t)(*video_frame_record_timestamp);


    // copy YUV frame data into buffers
    memcpy(frame->data[0], y, width * height);
    memcpy(frame->data[1], u, (width / 2) * (height / 2));
    memcpy(frame->data[2], v, (width / 2) * (height / 2));

    // encode the frame
    ret = avcodec_send_frame(call->video->h264_encoder2, frame);

    if (ret < 0) {
        LOGGER_ERROR(av->m->log, "Error sending a frame for encoding:ERROR");
    }

    ret = avcodec_receive_packet(call->video->h264_encoder2, call->video->h264_out_pic2);

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        *i_frame_size = 0;
    } else if (ret < 0) {
        *i_frame_size = 0;
        // fprintf(stderr, "Error during encoding\n");
    } else {

        // printf("Write packet %3"PRId64" (size=%5d)\n", call->video->h264_out_pic2->pts, call->video->h264_out_pic2->size);
        // fwrite(call->video->h264_out_pic2->data, 1, call->video->h264_out_pic2->size, outfile);

        global_encoder_delay_counter++;

        if (global_encoder_delay_counter > 60) {
            global_encoder_delay_counter = 0;
            LOGGER_DEBUG(av->m->log, "enc:delay=%ld",
                         (long int)(frame->pts - (int64_t)call->video->h264_out_pic2->pts)
                        );
        }


        *i_frame_size = call->video->h264_out_pic2->size;
        *video_frame_record_timestamp = (uint64_t)call->video->h264_out_pic2->pts;

        result = 0;
    }

    av_frame_free(&frame);

    return result;

#endif

}

uint32_t send_frames_h264(ToxAV *av, uint32_t friend_number, uint16_t width, uint16_t height,
                          const uint8_t *y,
                          const uint8_t *u, const uint8_t *v, ToxAVCall *call,
                          uint64_t *video_frame_record_timestamp,
                          int vpx_encode_flags,
                          x264_nal_t **nal,
                          int *i_frame_size,
                          TOXAV_ERR_SEND_FRAME *rc)
{

#ifdef X264_ENCODE_USED


    if (*i_frame_size > 0) {

        // use the record timestamp that was actually used for this frame
        *video_frame_record_timestamp = (uint64_t)call->video->h264_in_pic.i_pts; // TODO: --> is this wrong?
        const uint32_t frame_length_in_bytes = *i_frame_size;
        const int keyframe = (int)call->video->h264_out_pic.b_keyframe;

        // LOGGER_ERROR(av->m->log, "video packet record time[1]: %lu", (*video_frame_record_timestamp));

        int res = rtp_send_data
                  (
                      call->video_rtp,
                      (const uint8_t *)((*nal)->p_payload),
                      frame_length_in_bytes,
                      keyframe,
                      *video_frame_record_timestamp,
                      (int32_t)0,
                      TOXAV_ENCODER_CODEC_USED_H264,
                      call->video_bit_rate,
                      call->video->client_video_capture_delay_ms,
                      call->video->video_encoder_frame_orientation_angle,
                      av->m->log
                  );

        (*video_frame_record_timestamp)++;

        if (res < 0) {
            LOGGER_WARNING(av->m->log, "Could not send video frame: %s", strerror(errno));
            *rc = TOXAV_ERR_SEND_FRAME_RTP_FAILED;
            return 1;
        }

        return 0;
    } else {
        *rc = TOXAV_ERR_SEND_FRAME_RTP_FAILED;
        return 1;
    }

#else

    if (*i_frame_size > 0) {

        *video_frame_record_timestamp = (uint64_t)call->video->h264_out_pic2->pts;
        const uint32_t frame_length_in_bytes = *i_frame_size;
        const int keyframe = (int)1;

        // LOGGER_ERROR(av->m->log, "video packet record time[1]: %lu", (*video_frame_record_timestamp));
        *video_frame_record_timestamp = current_time_monotonic(av->m->mono_time);
        // LOGGER_ERROR(av->m->log, "video packet record time[2]: %lu", (*video_frame_record_timestamp));


        LOGGER_DEBUG(av->m->log, "call->video->video_encoder_frame_orientation_angle==%d",
                     call->video->video_encoder_frame_orientation_angle);

        int res = rtp_send_data
                  (
                      call->video_rtp,
                      (const uint8_t *)(call->video->h264_out_pic2->data),
                      frame_length_in_bytes,
                      keyframe,
                      *video_frame_record_timestamp,
                      (int32_t)0,
                      TOXAV_ENCODER_CODEC_USED_H264,
                      call->video_bit_rate,
                      call->video->client_video_capture_delay_ms,
                      call->video->video_encoder_frame_orientation_angle,
                      av->m->log
                  );

        (*video_frame_record_timestamp)++;


        av_packet_unref(call->video->h264_out_pic2);

        if (res < 0) {
            LOGGER_WARNING(av->m->log, "Could not send video frame: %s", strerror(errno));
            *rc = TOXAV_ERR_SEND_FRAME_RTP_FAILED;
            return 1;
        }

        return 0;

    } else {
        *rc = TOXAV_ERR_SEND_FRAME_RTP_FAILED;
        return 1;
    }

#endif

}

void vc_kill_h264(VCSession *vc)
{
    // encoder
#ifdef X264_ENCODE_USED

    if (vc->h264_encoder) {
        x264_encoder_close(vc->h264_encoder);
        x264_picture_clean(&(vc->h264_in_pic));
    }

#else
    // --- ffmpeg encoder ---
    av_packet_free(&(vc->h264_out_pic2));
    avcodec_free_context(&(vc->h264_encoder2));
    // --- ffmpeg encoder ---

#endif

    // decoder
    if (vc->h264_decoder->extradata) {
        av_free(vc->h264_decoder->extradata);
        vc->h264_decoder->extradata = NULL;
    }

    avcodec_free_context(&vc->h264_decoder);
}


