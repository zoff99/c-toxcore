
# Spec for NGC Video

video size MUST be exactly 480x640 pixels, portait format, suited for cellphone cameras.
video sending 1 frame per 250 milliseconds (~4 fps) (approximately), but NOT more often.

## Packet Data

   40000 max bytes length for custom lossless NGC packets.

   37000 max bytes length for video frames and header, to leave some space for $whatever here be dragons later




| what         | Length in bytes| Contents                                           |
|------        |--------        |------------------                                  |
| magic        |       6        |  0x667788113435                                    |
| version      |       1        |  0x01                                              |
| pkt id       |       1        |  0x21                                              |
| video width  |       1        |  uint8_t always 224 (8bit of 480)                  |
| video height |       1        |  uint8_t always 128 (8bit of 640)                  |
| video codec  |       1        |  uint8_t always 1  (1 -> H264)                     |
| data         |[1, 36989]      |  *uint8_t  bytes of file video, zero not allowed!  |


header size: 11 bytes

data   size: 1 - 36989 bytes

## Send Video to NGC Groups

do this globally, so you can only send/receive video for 1 NGC group at a time!

void* toxav_ngc_video_init(const uint16_t v_bitrate, const uint16_t max_quantizer);
bool toxav_ngc_video_encode(void *vngc, const uint16_t vbitrate, const uint16_t width, const uint16_t height,
                            const uint8_t *y, const uint8_t *u, const uint8_t *v,
                            uint8_t *encoded_frame_bytes, uint32_t *encoded_frame_size_bytes);
bool toxav_ngc_video_decode(void *vngc, uint8_t *encoded_frame_bytes, uint32_t encoded_frame_size_bytes,
                            uint16_t width, uint16_t height,
                            uint8_t *y, uint8_t *u, uint8_t *v,
                            int32_t *ystride, int32_t *ustride, int32_t *vstride);
void toxav_ngc_video_kill(void *vngc);


use `tox_group_send_custom_packet()` and `lossless` set to `true` to send video data
and `tox_group_custom_private_packet_cb()` to receive video date.






