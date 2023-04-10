

   40000 max bytes length for custom lossless NGC packets.

   37000 max bytes length for file and header, to leave some space for offline message syncing.


| what      | Length in bytes| Contents                                           |
|------     |--------        |------------------                                  |
| magic     |       6        |  0x667788113435                                    |
| version   |       1        |  0x01                                              |
| pkt id    |       1        |  0x11                                              |
| msg id    |      32        | *uint8_t  to uniquely identify the message         |
| create ts |       4        |  uint32_t unixtimestamp in UTC of local wall clock (in bigendian) |
| filename  |     255        |  len TOX_MAX_FILENAME_LENGTH                       |
|           |                |      data first, then pad with NULL bytes          |
| data      |[1, 36701]      |  bytes of file data, zero length files not allowed!|


header size: 299 bytes

data   size: 1 - 36701 bytes

