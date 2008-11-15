/*
 * Realmedia RTSP protocol (RDT) support.
 * Copyright (c) 2007 Ronald S. Bultje
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file rdt.c
 * @brief Realmedia RTSP protocol (RDT) support
 * @author Ronald S. Bultje <rbultje@ronald.bitfreak.net>
 */

#include "avformat.h"
#include "libavutil/avstring.h"
#include "rtp_internal.h"
#include "rdt.h"
#include "libavutil/base64.h"
#include "libavutil/md5.h"
#include "rm.h"
#include "internal.h"

struct RDTDemuxContext {
    AVFormatContext *ic;
    AVStream *st;
    void *dynamic_protocol_context;
    DynamicPayloadPacketHandlerProc parse_packet;
    uint32_t prev_set_id, prev_timestamp;
};

RDTDemuxContext *
ff_rdt_parse_open(AVFormatContext *ic, AVStream *st,
                  void *priv_data, RTPDynamicProtocolHandler *handler)
{
    RDTDemuxContext *s = av_mallocz(sizeof(RDTDemuxContext));
    if (!s)
        return NULL;

    s->ic = ic;
    s->st = st;
    s->prev_set_id    = -1;
    s->prev_timestamp = -1;
    s->parse_packet = handler->parse_packet;
    s->dynamic_protocol_context = priv_data;

    return s;
}

void
ff_rdt_parse_close(RDTDemuxContext *s)
{
    av_free(s);
}

struct PayloadContext {
    AVFormatContext *rmctx;
    uint8_t *mlti_data;
    unsigned int mlti_data_size;
    char buffer[RTP_MAX_PACKET_LENGTH + FF_INPUT_BUFFER_PADDING_SIZE];
};

void
ff_rdt_calc_response_and_checksum(char response[41], char chksum[9],
                                  const char *challenge)
{
    int ch_len = strlen (challenge), i;
    unsigned char zres[16],
        buf[64] = { 0xa1, 0xe9, 0x14, 0x9d, 0x0e, 0x6b, 0x3b, 0x59 };
#define XOR_TABLE_SIZE 37
    const unsigned char xor_table[XOR_TABLE_SIZE] = {
        0x05, 0x18, 0x74, 0xd0, 0x0d, 0x09, 0x02, 0x53,
        0xc0, 0x01, 0x05, 0x05, 0x67, 0x03, 0x19, 0x70,
        0x08, 0x27, 0x66, 0x10, 0x10, 0x72, 0x08, 0x09,
        0x63, 0x11, 0x03, 0x71, 0x08, 0x08, 0x70, 0x02,
        0x10, 0x57, 0x05, 0x18, 0x54 };

    /* some (length) checks */
    if (ch_len == 40) /* what a hack... */
        ch_len = 32;
    else if (ch_len > 56)
        ch_len = 56;
    memcpy(buf + 8, challenge, ch_len);

    /* xor challenge bytewise with xor_table */
    for (i = 0; i < XOR_TABLE_SIZE; i++)
        buf[8 + i] ^= xor_table[i];

    av_md5_sum(zres, buf, 64);
    ff_data_to_hex(response, zres, 16);
    for (i=0;i<32;i++) response[i] = tolower(response[i]);

    /* add tail */
    strcpy (response + 32, "01d0a8e3");

    /* calculate checksum */
    for (i = 0; i < 8; i++)
        chksum[i] = response[i * 4];
    chksum[8] = 0;
}

static int
rdt_load_mdpr (PayloadContext *rdt, AVStream *st, int rule_nr)
{
    ByteIOContext *pb;
    int size;
    uint32_t tag;

    /**
     * Layout of the MLTI chunk:
     * 4:MLTI
     * 2:<number of streams>
     * Then for each stream ([number_of_streams] times):
     *     2:<mdpr index>
     * 2:<number of mdpr chunks>
     * Then for each mdpr chunk ([number_of_mdpr_chunks] times):
     *     4:<size>
     *     [size]:<data>
     * we skip MDPR chunks until we reach the one of the stream
     * we're interested in, and forward that ([size]+[data]) to
     * the RM demuxer to parse the stream-specific header data.
     */
    if (!rdt->mlti_data)
        return -1;
    url_open_buf(&pb, rdt->mlti_data, rdt->mlti_data_size, URL_RDONLY);
    tag = get_le32(pb);
    if (tag == MKTAG('M', 'L', 'T', 'I')) {
        int num, chunk_nr;

        /* read index of MDPR chunk numbers */
        num = get_be16(pb);
        if (rule_nr < 0 || rule_nr >= num)
            return -1;
        url_fskip(pb, rule_nr * 2);
        chunk_nr = get_be16(pb);
        url_fskip(pb, (num - 1 - rule_nr) * 2);

        /* read MDPR chunks */
        num = get_be16(pb);
        if (chunk_nr >= num)
            return -1;
        while (chunk_nr--)
            url_fskip(pb, get_be32(pb));
        size = get_be32(pb);
    } else {
        size = rdt->mlti_data_size;
        url_fseek(pb, 0, SEEK_SET);
    }
    rdt->rmctx->pb = pb;
    if (ff_rm_read_mdpr_codecdata(rdt->rmctx, st, size) < 0)
        return -1;

    url_close_buf(pb);
    return 0;
}

/**
 * Actual data handling.
 */

int
ff_rdt_parse_header(const uint8_t *buf, int len,
                    int *set_id, int *seq_no, int *stream_id,
                    int *is_keyframe, uint32_t *timestamp)
{
    int consumed = 10;

    if (len > 0 && (buf[0] < 0x40 || buf[0] > 0x42)) {
        buf += 9;
        len -= 9;
        consumed += 9;
    }
    if (len < 10)
        return -1;
    /**
     * Layout of the header (in bits):
     * 1:  len_included
     *     Flag indicating whether this header includes a length field;
     *     this can be used to concatenate multiple RDT packets in a
     *     single UDP/TCP data frame and is used to precede RDT data
     *     by stream status packets
     * 1:  need_reliable
     *     Flag indicating whether this header includes a "reliable
     *     sequence number"; these are apparently sequence numbers of
     *     data packets alone. For data packets, this flag is always
     *     set, according to the Real documentation [1]
     * 5:  set_id
     *     ID of a set of streams of identical content, possibly with
     *     different codecs or bitrates
     * 1:  is_reliable
     *     Flag set for certain streams deemed less tolerable for packet
     *     loss
     * 16: seq_no
     *     Packet sequence number; if >=0xFF00, this is a non-data packet
     *     containing stream status info, the second byte indicates the
     *     type of status packet (see wireshark docs / source code [2])
     * if (len_included) {
     *     16: packet_len
     * } else {
     *     packet_len = remainder of UDP/TCP frame
     * }
     * 1:  is_back_to_back
     *     Back-to-Back flag; used for timing, set for one in every 10
     *     packets, according to the Real documentation [1]
     * 1:  is_slow_data
     *     Slow-data flag; currently unused, according to Real docs [1]
     * 5:  stream_id
     *     ID of the stream within this particular set of streams
     * 1:  is_no_keyframe
     *     Non-keyframe flag (unset if packet belongs to a keyframe)
     * 32: timestamp (PTS)
     * if (set_id == 0x1F) {
     *     16: set_id (extended set-of-streams ID; see set_id)
     * }
     * if (need_reliable) {
     *     16: reliable_seq_no
     *         Reliable sequence number (see need_reliable)
     * }
     * if (stream_id == 0x3F) {
     *     16: stream_id (extended stream ID; see stream_id)
     * }
     * [1] https://protocol.helixcommunity.org/files/2005/devdocs/RDT_Feature_Level_20.txt
     * [2] http://www.wireshark.org/docs/dfref/r/rdt.html and
     *     http://anonsvn.wireshark.org/viewvc/trunk/epan/dissectors/packet-rdt.c
     */
    if (set_id)    *set_id    = (buf[0]>>1) & 0x1f;
    if (seq_no)    *seq_no    = AV_RB16(buf+1);
    if (timestamp) *timestamp = AV_RB32(buf+4);
    if (stream_id) *stream_id = (buf[3]>>1) & 0x1f;
    if (is_keyframe) *is_keyframe = !(buf[3] & 0x1);

    return consumed;
}

/**< return 0 on packet, no more left, 1 on packet, 1 on partial packet... */
static int
rdt_parse_packet (PayloadContext *rdt, AVStream *st,
                  AVPacket *pkt, uint32_t *timestamp,
                  const uint8_t *buf, int len, int flags)
{
    int seq = 1, res;
    ByteIOContext *pb = rdt->rmctx->pb;
    RMContext *rm = rdt->rmctx->priv_data;

    if (rm->audio_pkt_cnt == 0) {
        int pos;

        url_open_buf (&pb, buf, len, URL_RDONLY);
        flags = (flags & PKT_FLAG_KEY) ? 2 : 0;
        rdt->rmctx->pb = pb;
        res = ff_rm_parse_packet (rdt->rmctx, st, len, pkt,
                                  &seq, &flags, timestamp);
        pos = url_ftell(pb);
        url_close_buf (pb);
        if (res < 0)
            return res;
        if (rm->audio_pkt_cnt > 0 &&
            st->codec->codec_id == CODEC_ID_AAC) {
            memcpy (rdt->buffer, buf + pos, len - pos);
            url_open_buf (&pb, rdt->buffer, len - pos, URL_RDONLY);
            rdt->rmctx->pb = pb;
        }
    } else {
        ff_rm_retrieve_cache (rdt->rmctx, st, pkt);
        if (rm->audio_pkt_cnt == 0 &&
            st->codec->codec_id == CODEC_ID_AAC)
            url_close_buf (pb);
    }
    pkt->stream_index = st->index;
    pkt->pts = *timestamp;

    return rm->audio_pkt_cnt > 0;
}

int
ff_rdt_parse_packet(RDTDemuxContext *s, AVPacket *pkt,
                    const uint8_t *buf, int len)
{
    int seq_no, flags = 0, stream_id, set_id, is_keyframe;
    uint32_t timestamp;
    int rv= 0;

    if (!s->parse_packet)
        return -1;

    if (!buf) {
        /* return the next packets, if any */
        timestamp= 0; ///< Should not be used if buf is NULL, but should be set to the timestamp of the packet returned....
        rv= s->parse_packet(s->dynamic_protocol_context,
                            s->st, pkt, &timestamp, NULL, 0, flags);
        return rv;
    }

    if (len < 12)
        return -1;
    rv = ff_rdt_parse_header(buf, len, &set_id, &seq_no, &stream_id, &is_keyframe, &timestamp);
    if (rv < 0)
        return rv;
    if (is_keyframe && (set_id != s->prev_set_id || timestamp != s->prev_timestamp)) {
        flags |= PKT_FLAG_KEY;
        s->prev_set_id    = set_id;
        s->prev_timestamp = timestamp;
    }
    buf += rv;
    len -= rv;

    rv = s->parse_packet(s->dynamic_protocol_context,
                         s->st, pkt, &timestamp, buf, len, flags);

    return rv;
}

void
ff_rdt_subscribe_rule (char *cmd, int size,
                       int stream_nr, int rule_nr)
{
    av_strlcatf(cmd, size, "stream=%d;rule=%d,stream=%d;rule=%d",
                stream_nr, rule_nr * 2, stream_nr, rule_nr * 2 + 1);
}

void
ff_rdt_subscribe_rule2 (RDTDemuxContext *s, char *cmd, int size,
                        int stream_nr, int rule_nr)
{
    PayloadContext *rdt = s->dynamic_protocol_context;

    rdt_load_mdpr(rdt, s->st, rule_nr * 2);
}

static unsigned char *
rdt_parse_b64buf (unsigned int *target_len, const char *p)
{
    unsigned char *target;
    int len = strlen(p);
    if (*p == '\"') {
        p++;
        len -= 2; /* skip embracing " at start/end */
    }
    *target_len = len * 3 / 4;
    target = av_mallocz(*target_len + FF_INPUT_BUFFER_PADDING_SIZE);
    av_base64_decode(target, p, *target_len);
    return target;
}

static int
rdt_parse_sdp_line (AVStream *stream, PayloadContext *rdt, const char *line)
{
    const char *p = line;

    if (av_strstart(p, "OpaqueData:buffer;", &p)) {
        rdt->mlti_data = rdt_parse_b64buf(&rdt->mlti_data_size, p);
    } else if (av_strstart(p, "StartTime:integer;", &p))
        stream->first_dts = atoi(p);

    return 0;
}

static PayloadContext *
rdt_new_extradata (void)
{
    PayloadContext *rdt = av_mallocz(sizeof(PayloadContext));

    av_open_input_stream(&rdt->rmctx, NULL, "", &rdt_demuxer, NULL);

    return rdt;
}

static void
rdt_free_extradata (PayloadContext *rdt)
{
    if (rdt->rmctx)
        av_close_input_stream(rdt->rmctx);
    av_freep(&rdt->mlti_data);
    av_free(rdt);
}

#define RDT_HANDLER(n, s, t) \
static RTPDynamicProtocolHandler ff_rdt_ ## n ## _handler = { \
    s, \
    t, \
    CODEC_ID_NONE, \
    rdt_parse_sdp_line, \
    rdt_new_extradata, \
    rdt_free_extradata, \
    rdt_parse_packet \
};

RDT_HANDLER(live_video, "x-pn-multirate-realvideo-live", CODEC_TYPE_VIDEO);
RDT_HANDLER(live_audio, "x-pn-multirate-realaudio-live", CODEC_TYPE_AUDIO);
RDT_HANDLER(video,      "x-pn-realvideo",                CODEC_TYPE_VIDEO);
RDT_HANDLER(audio,      "x-pn-realaudio",                CODEC_TYPE_AUDIO);

void av_register_rdt_dynamic_payload_handlers(void)
{
    ff_register_dynamic_payload_handler(&ff_rdt_video_handler);
    ff_register_dynamic_payload_handler(&ff_rdt_audio_handler);
    ff_register_dynamic_payload_handler(&ff_rdt_live_video_handler);
    ff_register_dynamic_payload_handler(&ff_rdt_live_audio_handler);
}
