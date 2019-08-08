 Tox MessageV2
 =============

proposal to replace current text messages with filetransfers (which are basically multipart messages)

prerequisites:
--------------
- [x] filetransfers need to be "round robin" (now only 1 filetransfer is active until done or canceled or paused)
- [x] filetransfers of new types (TOX_FILE_KIND_MESSAGEV2_*) shall be autoaccepted in toxcore
- [ ] filetransfers of new types (TOX_FILE_KIND_MESSAGEV2_*) shall ignore PAUSE in toxcore
- [ ] filetransfers of new types (TOX_FILE_KIND_MESSAGEV2_*) shall be autocanceled after 5 minutes, if not accepted
- [ ] filetransfers of new types (TOX_FILE_KIND_MESSAGEV2_*) shall be autocanceled after 10 minutes, if not completed or canceled
- [ ] clients shall sort MessageV2 by "create ts" and "create ts ms"


new filetransfer type:
----------------------

TOX_FILE_KIND_MESSAGEV2_SEND
sending of a textmessage

filename shall be 'messagev2.txt'

## raw data:

```
what           |Length     | Contents
----------------------------------------------------------------
msg id         |32         | *uint8_t hash (what hash function?) to uniquely identify the message
create ts      |4          | uint32_t unixtimestamp in UTC of local clock (NTP time if poosible -> client?)
               |           | when the user typed the message
create ts ms   |2          | uint16_t unixtimestamp ms part
msg txt        |[0, 4096]  | Message as a UTF8 byte string
 4096 = TOX_MESSAGEV2_MAX_TEXT_LENGTH
```


new filetransfer type:
----------------------

TOX_FILE_KIND_MESSAGEV2_ANSWER
since its not certain for the sender that the filetransfer has really completed on the receiver.
this is the replacement for message receipts.

filename shall be 'messagev2ack.txt'

## raw data:

```
what        |Length   | Contents
----------------------------------------------------------------
msg id      |32       | *uint8_t hash (what hash function?) to uniquely identify the message
            |         | the answer is for
ts          |4        | uint32_t unixtimestamp in UTC of local clock (NTP time if poosible -> client?)
            |         | when the message was received
ts ms       |2        | uint16_t unixtimestamp ms part
```



new filetransfer type:
----------------------

TOX_FILE_KIND_MESSAGEV2_ALTER
correct or delete an already sent message

filename shall be 'messagev2alter.txt'

## raw data:

```
what        |Length     | Contents
----------------------------------------------------------------
msg id      |32         | *uint8_t hash (what hash function?) to uniquely identify the message
alter ts    |4          | uint32_t unixtimestamp in UTC of local clock (NTP time if poosible -> client?)
            |           | when the user typed the message
alter ts ms |2          | uint16_t unixtimestamp ms part
alter type  |1          | uint8_t values: 0 -> delete message, 1 -> change text
alter id    |32         | *uint8_t hash to identify the message to alter/delete
msg txt     |[0, 4096]  | Altered Message as a UTF8 byte string or 0 length on delete
 4096 = TOX_MESSAGEV2_MAX_TEXT_LENGTH
```


add helper functions for sending:
---------------------------------

```
uint32_t tox_messagev2_size(uint32_t text_length, uint32_t type)
  will return the length of the raw message data to send (for each type)
  type is of TOX_FILE_KIND_MESSAGEV2_SEND
             TOX_FILE_KIND_MESSAGEV2_ANSWER
             TOX_FILE_KIND_MESSAGEV2_ALTER

bool tox_messagev2_wrap(uint32_t text_length, uint32_t type, *uint8_t message_text, uint32_t ts_sec, uint16_t ts_ms, *uint8_t raw_message)
  if ts_sec and ts_ms are both "0" timestamp will be filled in my toxcore
  type is of TOX_FILE_KIND_MESSAGEV2_SEND
             TOX_FILE_KIND_MESSAGEV2_ANSWER
             TOX_FILE_KIND_MESSAGEV2_ALTER
  will fill raw_message with the raw message data to send as filetransfer
  raw_message must already be allocated to size of tox_messagev2_size() call
  text_length can be "0" (for DELETE or ACK)
  message_text can be "0" (for DELETE or ACK)
```

add helper functions for receiving:
-----------------------------------

```
bool tox_messagev2_get_message_id(*uint8_t raw_message, *uint8_t msg_id)
  will return the msg id
  msg_id has to be allocated to TOX_MESSAGEV2_MSGID_SIZE

uint32_t tox_messagev2_get_ts_sec(*uint8_t raw_message)
  will return the timestamp (seconds part)

uint16_t tox_messagev2_get_ts_ms(*uint8_t raw_message)
  will return the timestamp (milliseconds part)

bool tox_messagev2_get_message_text(*uint8_t raw_message, uint8_t *is_alter_msg, uint8_t *alter_type, *uint8_t message_text)
  will fill message_text with the message text to display
  message_text must already be allocated to TOX_MESSAGEV2_MAX_TEXT_LENGTH
  will set is_alter_msg: 0 -> not an "alter" message 1 -> is alter message
  will set alter_type: 0 -> delete message, 1 -> change text (if is_alter_msg == 0 then alter_type will be undefined)
```


### pros:
* does not break API or clients
* solves "message received out of sequence"
* adds multipart messages
* filetransfer are already fully implemented (no need to reinvent the wheel)
* prevent double message sending

### cons:
* no way to know if other node has Tox MessageV2 capability
* clients need to make changes to adopt


