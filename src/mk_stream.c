/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Monkey HTTP Server
 *  ==================
 *  Copyright 2001-2015 Monkey Software LLC <eduardo@monkey.io>
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <monkey/mk_list.h>
#include <monkey/mk_socket.h>
#include <monkey/mk_memory.h>
#include <monkey/mk_stream.h>
#include <monkey/mk_server.h>

/* Create a new stream instance */
mk_stream_t *mk_stream_new(int type, mk_channel_t *channel,
                           void *buffer, size_t size, void *data,
                           void (*cb_finished) (mk_stream_t *),
                           void (*cb_bytes_consumed) (mk_stream_t *, long),
                           void (*cb_exception) (mk_stream_t *, int))
{
    mk_stream_t *stream;

    stream = mk_mem_malloc(sizeof(mk_stream_t));
    mk_stream_set(stream, type, channel,
                  buffer, size,
                  data,
                  cb_finished,
                  cb_bytes_consumed,
                  cb_exception);

    return stream;
}


/* Create a new channel */
mk_channel_t *mk_channel_new(int type, int fd)
{
    mk_channel_t *channel;

    channel = mk_mem_malloc(sizeof(mk_channel_t));
    channel->type = type;
    channel->fd   = fd;

    mk_list_init(&channel->streams);

    return channel;
}

static inline size_t channel_write_stream_file(mk_channel_t *channel,
                                               mk_stream_t *stream)
{
    long int bytes = 0;

    MK_TRACE("[CH %i] STREAM_FILE %i, bytes=%lu",
             channel->fd, stream->fd, stream->bytes_total);

    /* Direct write */
    bytes = mk_socket_send_file(channel->fd,
                                stream->fd,
                                &stream->bytes_offset,
                                stream->bytes_total
                                );
    MK_TRACE("[CH=%d] [FD=%i] WRITE STREAM FILE: %lu bytes",
             channel->fd, stream->fd, bytes);

    return bytes;
}

int mk_channel_write(mk_channel_t *channel)
{
    size_t bytes = -1;
    struct mk_iov *iov;
    mk_ptr_t *ptr;
    mk_stream_t *stream;

    if (mk_list_is_empty(&channel->streams) == 0) {
        MK_TRACE("[CH %i] CHANNEL_EMPTY", channel->fd);
        return MK_CHANNEL_EMPTY;
    }

    /* Get the input source */
    stream = mk_list_entry_first(&channel->streams, mk_stream_t, _head);

    /*
     * Based on the Stream type we consume on that way, not all inputs
     * requires to read from buffer, e.g: Static File, Pipes.
     */
    if (channel->type == MK_CHANNEL_SOCKET) {
        if (stream->type == MK_STREAM_FILE) {
            bytes = channel_write_stream_file(channel, stream);
        }
        else if (stream->type == MK_STREAM_IOV) {
            MK_TRACE("[CH %i] STREAM_IOV, wrote %lu bytes",
                     channel->fd, stream->bytes_total);

            iov   = stream->buffer;
            bytes = mk_socket_sendv(channel->fd, iov);

            if (bytes > 0) {
                /* Perform the adjustment on mk_iov */
                mk_iov_consume(iov, bytes);
            }
        }
        else if (stream->type == MK_STREAM_PTR) {
            MK_TRACE("[CH %i] STREAM_PTR, bytes=%lu",
                     channel->fd, stream->bytes_total);

            ptr = stream->buffer;
            printf("data='%s'\n", ptr->data);
            bytes = mk_socket_send(channel->fd, ptr->data, ptr->len);
            if (bytes > 0) {
                /* FIXME OFFSET */
            }
        }

        if (bytes > 0) {
            mk_stream_bytes_consumed(stream, bytes);

            /* notification callback, optional */
            if (stream->cb_bytes_consumed) {
                stream->cb_bytes_consumed(stream, bytes);
            }

            if (stream->bytes_total == 0) {
                MK_TRACE("Stream done, unlinking");

                if (stream->cb_finished) {
                    stream->cb_finished(stream);
                }

                if (stream->preserve == MK_FALSE) {
                    mk_stream_unlink(stream);
                }
            }

            if (mk_list_is_empty(&channel->streams) == 0) {
                MK_TRACE("[CH %i] CHANNEL_DONE", channel->fd);
                return MK_CHANNEL_DONE;
            }

            MK_TRACE("[CH %i] CHANNEL_FLUSH", channel->fd);
            return MK_CHANNEL_FLUSH;
        }
        else if (bytes <= 0) {
            if (stream->cb_exception) {
                stream->cb_exception(stream, errno);
            }
            return MK_CHANNEL_ERROR;
        }
    }

    return MK_CHANNEL_UNKNOWN;
}
