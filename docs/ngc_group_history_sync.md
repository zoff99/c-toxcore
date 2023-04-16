 
# Spec for NGC group history sync

## Send history sync request

* trigger: `tox_group_peer_join_cb()`

each time a peer joins we ask that peer to send all non-private group chat messages
from 130 minutes ago up to now.

to make sure that peer is actually really online (tox has a bit of an issue there), wait `n` seconds before sending `ngch_request`

`n` = 9 + random(0 .. 7)

* `ngch_request` packet data:

| what      | Length in bytes| Contents         |
|-----------|----------------|------------------|
| magic     |       6        |  0x667788113435  |
| version   |       1        |  0x01            |
| pkt id    |       1        |  0x01            |
| sync delta|       1        |  how many minutes back from now() to get messages <br> allowed values from 5 to 130 minutes (both inclusive)         |

send the data to the peer with:
`tox_group_send_custom_private_packet()` and specifiy `lossless` paramter as `true`

## Respond to history sync request

* trigger: `tox_group_custom_private_packet_cb()` and it's the `ngch_request` packet

the tox client needs to get **authorization** from the user that it's ok to
share group chat history with the requesting peer

then we send all non-private group chats messages from 130 minutes ago up to now
sorted by oldest (first) to newest (last)
each message is sent as a seperate **lossless** custom packet

wait `n` milliseconds seconds before sending the next message.

`n` = 300 + random(0 .. 300)

* `ngch_syncmsg` packet data:

#### for text messages:

MAX_GC_CUSTOM_PACKET_SIZE 40000<br>
header_size = 6 + 1 + 1 + 4 + 32 + 4 + 25 = 73<br>
max sync message text bytes = MAX_GC_CUSTOM_PACKET_SIZE - header_size = 39927<br>

| what        | Length in bytes| Contents                                                                      |
|-------------|----------------|-------------------------------------------------------------------------------|
| magic       |       6        |  0x667788113435                                                               |
| version     |       1        |  0x01                                                                         |
| pkt id      |       1        |  0x02 <-- text                                                                |
| msg id      |       4        |  4 bytes message id for this group message                                    |
| sender      |      32        |  32 bytes pubkey of the original sender in the ngc group                      |
| timestamp   |       4        |  uint32_t unixtimestamp in UTC of local wall clock (in bigendian) when the message was originally sent   |
| name        |      25        |  sender name 25 bytes (cut off if longer, or right padded with 0x0 bytes)     |
| message     | [1, 39927]     |  message text, zero length message not allowed!                               | 

send the data to the peer with:
`tox_group_send_custom_private_packet()` and specifiy `lossless` paramter as `true`

#### for files:

MAX_GC_CUSTOM_PACKET_SIZE 40000<br>
header_size = 6 + 1 + 1 + 32 + 32 + 4 + 25 = 356<br>
max sync message file bytes = MAX_GC_CUSTOM_PACKET_SIZE - header_size = 39644<br>
max group file bytes is 36701<br>

| what        | Length in bytes| Contents                                                                      |
|-------------|----------------|-------------------------------------------------------------------------------|
| magic       |       6        |  0x667788113435                                                               |
| version     |       1        |  0x01                                                                         |
| pkt id      |       1        |  0x03 <-- file                                                                |
| msg id      |      32        |  *uint8_t to uniquely identify the message (the original value that is saved in history) |
| sender      |      32        |  32 bytes pubkey of the original sender in the ngc group                      |
| timestamp   |       4        |  uint32_t unixtimestamp in UTC of local wall clock (in bigendian) when the message was originally sent   |
| name        |      25        |  sender name 25 bytes (cut off if longer, or right padded with 0x0 bytes)     |
| filename    |     255        |  len TOX_MAX_FILENAME_LENGTH   data first, then pad with NULL bytes           |
| data        | [1, 36701]     |  bytes of file data, zero length files not allowed!                           |


send the data to the peer with:
`tox_group_send_custom_private_packet()` and specifiy `lossless` paramter as `true`

## Receive history sync data

* trigger: `tox_group_custom_private_packet_cb()` and it's the `ngch_syncmsg` packet

the tox client needs to sort these messages into the group chat view
since the dates can be in the past and newer messages can already be present in the current uservisible view.

* duplicate message check:

if a message with that `msg id` for this group chat and this `sender` within the last 300 days already exists,
then assume it is a duplicate and keep the current message and discard the incoming sync message.

* cutoff UTF-8

since `name` could have been cut at the maximum byte limit, the tox client needs to check for cut off UTF-8 and handle this accordingly.

