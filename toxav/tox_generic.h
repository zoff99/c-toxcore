
// H264 settings -----------
#define x264_param_profile_str "high"
#define VIDEO_BITRATE_INITIAL_VALUE_H264 1500
#define VIDEO_BITRATE_MIN_AUTO_VALUE_H264 90
#define VIDEO_BITRATE_SCALAR_AUTO_VALUE_H264 1400
#define VIDEO_BITRATE_SCALAR_INC_BY_AUTO_VALUE_H264 180
#define VIDEO_BITRATE_SCALAR2_AUTO_VALUE_H264 5000
#define VIDEO_BITRATE_SCALAR2_INC_BY_AUTO_VALUE_H264 40
#define VIDEO_BITRATE_SCALAR3_AUTO_VALUE_H264 10000
#define VIDEO_BITRATE_MAX_AUTO_VALUE_H264 12000
#define VIDEO_BITRATE_AUTO_INC_THRESHOLD 2.0
#define VIDEO_BITRATE_AUTO_DEC_THRESHOLD 8.0
// increase video bitrate by 6%
#define VIDEO_BITRATE_AUTO_INC_TO 1.06
#define VIDEO_BITRATE_AUTO_DEC_FACTOR 0.5
#define VIDEO_MAX_KF_H264 50
#define VIDEO_BUF_FACTOR_H264 1
#define VIDEO_F_RATE_TOLERANCE_H264 1.2
#define VIDEO_BITRATE_FACTOR_H264 0.7
// H264 settings -----------


typedef struct ToxAVCall_s {
    ToxAV *av;

    pthread_mutex_t mutex_audio[1];
    PAIR(RTPSession *, ACSession *) audio;

    pthread_mutex_t mutex_video[1];
    PAIR(RTPSession *, VCSession *) video;

    BWController *bwc;

    uint8_t skip_video_flag;

    bool active;
    MSICall *msi_call;
    uint32_t friend_number;

    uint32_t audio_bit_rate; /* Sending audio bit rate */
    uint32_t video_bit_rate; /* Sending video bit rate */
    uint32_t video_bit_rate_last_last_changed; // only for callback info

    uint64_t last_incoming_video_frame_rtimestamp;
    uint64_t last_incoming_video_frame_ltimestamp;

    uint64_t last_incoming_audio_frame_rtimestamp;
    uint64_t last_incoming_audio_frame_ltimestamp;

    uint64_t reference_rtimestamp;
    uint64_t reference_ltimestamp;
    int64_t reference_diff_timestamp;
    uint8_t reference_diff_timestamp_set;

    /** Required for monitoring changes in states */
    uint8_t previous_self_capabilities;

    pthread_mutex_t mutex[1];

    struct ToxAVCall_s *prev;
    struct ToxAVCall_s *next;
} ToxAVCall;


struct ToxAV {
    Messenger *m;
    MSISession *msi;

    /* Two-way storage: first is array of calls and second is list of calls with head and tail */
    ToxAVCall **calls;
    uint32_t calls_tail;
    uint32_t calls_head;
    pthread_mutex_t mutex[1];

    PAIR(toxav_call_cb *, void *) ccb; /* Call callback */
    PAIR(toxav_call_comm_cb *, void *) call_comm_cb; /* Call_comm callback */
    PAIR(toxav_call_state_cb *, void *) scb; /* Call state callback */
    PAIR(toxav_audio_receive_frame_cb *, void *) acb; /* Audio frame receive callback */
    PAIR(toxav_video_receive_frame_cb *, void *) vcb; /* Video frame receive callback */
    PAIR(toxav_bit_rate_status_cb *, void *) bcb; /* Bit rate control callback */

    /** Decode time measures */
    int32_t dmssc; /** Measure count */
    int32_t dmsst; /** Last cycle total */
    int32_t dmssa; /** Average decoding time in ms */

    uint32_t interval; /** Calculated interval */
};

