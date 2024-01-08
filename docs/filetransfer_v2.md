
# Spec for Filetransfer version 2

## toxcore changes for ftv2

there is a new Filestatus `FILECONTROL_FINISHED` added, which is used only by toxcore internally.
this is used to know when the receiver actually has finished the filetransfer and is not missing any chunks.

a new Filekind `FILEKIND_FTV2` / `TOX_FILE_KIND_FTV2` is added, used by a toxclient to know that an ftv2 is used.

a new Capability flag `TOX_CAPABILITY_FTV2` is added, to know if clients support ftv2.
if the receiver supports ftv2, then ftv2 should be used when starting filetransfers.

## sending files with ftv2

here we describe only the changes from basic filetransfers, anything else stays the same and is documented in tox.h

start a filetransfer with `tox_file_send` but use `TOX_FILE_KIND_FTV2` as `kind`

now when toxcore requests a filechunk with `tox_file_chunk_request_cb` you need to send the data
and the `FILE_ID_LENGTH` bytes file id on every chuck with `tox_file_send_chunk`

call `tox_file_send_chunk` with `length` parameter set to the requested chunk length plus `FILE_ID_LENGTH`
and prepend the `FILE_ID_LENGTH` bytes file_id at the start of `data` chunk buffer

here is a pseudo code example:

```
tox_callback_file_chunk_request_cb_method(friend_number, file_number, position, length)
{
    if (this is a TOX_FILE_KIND_FTV2)
    {
        uint_8 *file_chunk = ByteBuffer.allocateDirect(length + TOX_FILE_ID_LENGTH)
        file_chunk.put(file_id_hash_bytes)
        file_chunk.put(file_chunk_bytes)
        tox_file_send_chunk(friend_number, file_number, position, file_chunk,
                            length + TOX_FILE_ID_LENGTH)
    }
}
```

## receiving files with ftv2

here we describe only the changes from basic filetransfers, anything else stays the same and is documented in tox.h


toxcore gives file data via `tox_file_recv_chunk_cb` callback, but now `data` and `length` will also contain
`FILE_ID_LENGTH` bytes file_id at the start of `data` chunk buffer.
only if the filetransfer is complete it will not include the file_id and send `data` as NULL and `length` as 0 as usual.


here is a pseudo code example:

```
tox_callback_file_recv_chunk_cb_method(friend_number, file_number, position, data, length)
{
    if (this is a TOX_FILE_KIND_FTV2)
    {
        if (length == 0)
        {
            // FTv2 is finished, data fully received
        }
        else
        {
            fileStream fos = fileopen(filename)
            fos.seek(position)
            fos.write(data, offset:(TOX_FILE_ID_LENGTH), bytes:(length - TOX_FILE_ID_LENGTH))
            fos.close()
        }
    }
}
```








