 Tox MessageV3
 =============

proposal for a simple addon to 1-on-1 text messages in toxcore

prerequisites:
--------------
- [x] patch toxcore for msgV3 [patch](https://github.com/zoff99/toxcore_custom_diffs/blob/master/0003_zoff_tc___message_v3_addon.diff)
- [x] the maximum bytes for the text message has shortended to TOX_MSGV3_MAX_MESSAGE_LENGTH (== 1334) bytes


sending a 1-on-1 text message:
------------------------------


call `tox_messagev3_get_new_message_id()` to get a new random 32byte msgV3 hash
use that hash for this messages from now on.

the contruct a new msgV3 buffer like this:

```
what           |Length     | Contents
----------------------------------------------------------------
msg txt        |[1, 1334]  | Message as a UTF8 byte string (usually no NULL byte at the end)
               |           |   zero length text for messages are not allowed in toxcore
msgV3 guard    |2          | 2 NULL bytes to sperate the text from the metadata after it
msg id         |32         | *uint8_t msgV3 hash to uniquely identify the message
create ts      |4          | uint32_t unixtimestamp in UTC of local clock when the message was typed
               |           |   if the timestamp is not used then set to 4 NULL bytes    
 1334 = TOX_MSGV3_MAX_MESSAGE_LENGTH
```

now just normally send that buffer with `tox_friend_send_message()` where message is your buffer
and length is the length in bytes of the msgV3 buffer (including the metadata!)

always send msgV3 format, even when it is not clear if the other client does have msgV3 capability.
this is to avoid handshake and complicated negotiation. also msgV3 does not interfere with non msgV3 tox clients.
*(the exception is aTox, the aTox developer does not wish to fix this)*


receiving a 1-on-1 text message:
--------------------------------

to detect if the sender "speaks" msgV3 a tox client should wait for the first message from another client.
if that client sends msgV3 metadata with a text message, that client is assumed to "speak" msgV3.
that capability should be saved. only if a text message without msgV3 metadata is received, the msgV3
for that client shall be removed, and saved that it is so.
if a message with msgV3 metadata is received again, see above.

on receicing a text message via the `tox_friend_message_cb()` callback, first check (see above) for msgV3 capbility and store that fact
permanently.

if that message buffer from the callback contains msgV3 metadata (this is determined by the msgV3 guard and the number of bytes that follow)
parse the metadata and store it with the message text.


sending a high level ACK for a msgV3 message:
---------------------------------------------

when you want to send a high level ACK for a particular message just call `tox_friend_send_message()` with this buffer:

```
what           |Length     | Contents
----------------------------------------------------------------
msg txt        |[1]        | shall by fixed to '_' char (without NULL termination)
msgV3 guard    |2          | 2 NULL bytes to sperate the text from the metadata after it
msg id         |32         | *uint8_t msgV3 hash to uniquely identify the message
create ts      |4          | uint32_t unixtimestamp in UTC of local clock when the message was ACK'ed
               |           |   if the timestamp is not used then set to 4 NULL bytes    
```

and
Tox_Message_Type is TOX_MESSAGE_TYPE_HIGH_LEVEL_ACK (== 3)
and
length is the length in bytes of the msgV3 buffer (including the metadata!)


receiving a high level ACK for a msgV3 message:
-----------------------------------------------

on receicing a text message via the `tox_friend_message_cb()` callback where Tox_Message_Type == TOX_MESSAGE_TYPE_HIGH_LEVEL_ACK (== 3),
parse the msgV3 metadata from the message buffer and get the msgV3 hash.
now you can do whatever your tox client needs to do to ACK that message.





