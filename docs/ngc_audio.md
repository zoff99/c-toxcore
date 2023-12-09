
# Spec for NGC Audio

audio MUST be PCM (signed) int16_t, MONO, @48kHz and framesize 120ms, audio bitrate MUST be 12000 bits/second

## Packet Data

   40000 max bytes length for custom lossless NGC packets.

   1372 (MAX_GC_PACKET_CHUNK_SIZE) max bytes length for audio frames and header,
   but most audio frames will never be near that size anyway.




| what          | Length in bytes| Contents                                           |
|------         |--------        |------------------                                  |
| magic         |       6        |  0x667788113435                                    |
| version       |       1        |  0x01                                              |
| pkt id        |       1        |  0x31                                              |
| audio channels|       1        |  uint8_t always 1 (for MONO)                       |
| sampling freq |       1        |  uint8_t always 48 (for 48kHz)                     |
| data          |[1, 1362]       |  *uint8_t  bytes, zero not allowed!                |


header size: 10 bytes

data   size: 1 - 1362 bytes

## Send Audio to NGC Groups

do this globally, so you can only send/receive audio for 1 NGC group at a time!

```c
void* toxav_ngc_audio_init(const int32_t bit_rate, const int32_t sampling_rate, const int32_t channel_count);
void toxav_ngc_audio_kill(void *angc);
bool toxav_ngc_audio_encode(void *angc, const int16_t *pcm, const int32_t sample_count_per_frame,
                        uint8_t *encoded_frame_bytes, uint32_t *encoded_frame_size_bytes);
int32_t toxav_ngc_audio_decode(void *angc, const uint8_t *encoded_frame_bytes,
                        uint32_t encoded_frame_size_bytes,
                        int16_t *pcm_decoded);
```

use `tox_group_send_custom_packet()` and `lossless` set to `false` to send audio data
and `tox_group_custom_private_packet_cb()` to receive audio data.






