#pragma once
#include "fmt/mp4.h"
#include "sock_send.h"

/* Transmit-or-skip the fMP4 fragment currently staged in the global mux
   (caller has already run mp4_set_slice for this frame) to one client on `fd`.
   Patches the staged moof with this client's state via mp4_set_state (which
   advances st->sequence_number and st->base_media_decode_time by the fragment's
   duration REGARDLESS of the send outcome), wraps moof+mdat in HTTP chunked
   framing, and hands it to sock_send_frame_or_skip.
   Returns SOCK_SEND_SENT / SOCK_SEND_SKIPPED / SOCK_SEND_DEAD. */
enum SockSendResult fmp4_send_fragment(int fd, struct Mp4State *st);
