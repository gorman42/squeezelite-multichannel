/* 
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *      Ralph Irving 2015-2026, ralph_irving@hotmail.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "squeezelite.h"

#if FFMPEG

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#define READ_SIZE  4096 * 4   // this is large enough to ensure ffmpeg always gets new data when decode is called
#define WRITE_SIZE 256 * 1024 // FIXME - make smaller, but still to absorb max wma output
#define SPDIF_WRITE_SIZE (64 * 1024)

// FIXME - do we need to align these params as per ffmpeg on i386? 
#define attribute_align_arg

#define HAVE_CH_LAYOUT (LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100))

struct ff_s {
	// state for ffmpeg decoder
	bool wma;
	bool bitstream_mode;
	char bitstream_format;
	char out_bitstream_format;
	u8_t bitstream_channels;
	u8_t wma_mmsh;
	u8_t wma_playstream;
	u8_t wma_metadatastream;
	u8_t *readbuf;
	u8_t *writebuf;
	bool end_of_stream;
	bool stream_completed;
	bool header_written;
	bool trailer_written;
	int64_t spdif_pts;
	const AVInputFormat *input_format;
	AVFormatContext *formatC;
	AVFormatContext *out_formatC;
	AVCodecContext *codecC;
	AVCodecParameters *codecP;
	AVFrame *frame;
	AVPacket *avpkt;
	unsigned mmsh_bytes_left;
	unsigned mmsh_bytes_pad;
	unsigned mmsh_packet_len;
#if !LINKALL
	// ffmpeg symbols to be dynamically loaded from libavcodec
	unsigned (* avcodec_version)(void);
	AVCodec * (* avcodec_find_decoder)(int);
	int attribute_align_arg (* avcodec_open2)(AVCodecContext *, const AVCodec *, AVDictionary **);
	AVFrame * (* av_frame_alloc)(void);
	void (* av_frame_free)(AVFrame **);
	int attribute_align_arg (* avcodec_send_packet)(AVCodecContext *, const AVPacket *);
	int attribute_align_arg (* avcodec_receive_frame)(AVCodecContext *, AVFrame *);
	AVCodecContext * (* avcodec_alloc_context3)(const AVCodec *);
	void (* avcodec_free_context)(AVCodecContext **);
	int (* avcodec_parameters_to_context)(AVCodecContext *, const AVCodecParameters *);
	int (* avcodec_parameters_copy)(AVCodecParameters *, const AVCodecParameters *);
	// ffmpeg symbols to be dynamically loaded from libavformat
	unsigned (* avformat_version)(void);
	AVFormatContext * (* avformat_alloc_context)(void);
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(59,0,0)
	int (* avformat_alloc_output_context2)(AVFormatContext **, const AVOutputFormat *, const char *, const char *);
#else
	int (* avformat_alloc_output_context2)(AVFormatContext **, AVOutputFormat *, const char *, const char *);
#endif
	AVStream * (* avformat_new_stream)(AVFormatContext *, const AVCodec *);
	int (* avformat_write_header)(AVFormatContext *, AVDictionary **);
	int (* av_interleaved_write_frame)(AVFormatContext *, AVPacket *);
	int (* av_write_trailer)(AVFormatContext *);
	void (* avformat_free_context)(AVFormatContext *);
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(59,0,0)
	int (* avformat_open_input)(AVFormatContext **, const char *, const AVInputFormat *, AVDictionary **);
#else
	int (* avformat_open_input)(AVFormatContext **, const char *, AVInputFormat *, AVDictionary **);
#endif
	int (* avformat_find_stream_info)(AVFormatContext *, AVDictionary **);
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(61,0,0)
	AVIOContext * (* avio_alloc_context)(unsigned char *, int, int, void *,
		int (*read_packet)(void *, uint8_t *, int), int (*write_packet)(void *, const uint8_t *, int), int64_t (*seek)(void *, int64_t, int));
#else
	AVIOContext * (* avio_alloc_context)(unsigned char *, int, int, void *,
		int (*read_packet)(void *, uint8_t *, int), int (*write_packet)(void *, uint8_t *, int), int64_t (*seek)(void *, int64_t, int));
#endif
	void (* avio_flush)(AVIOContext *);
	AVPacket * (* av_packet_alloc)(void);
	int (* av_new_packet)(AVPacket *, int);
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58,133,100)
	void (* av_init_packet)(AVPacket *);
#endif
	void (* av_packet_unref)(AVPacket *);
	int (* av_read_frame)(AVFormatContext *, AVPacket *);
	AVInputFormat * (* av_find_input_format)(const char *);
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58,9,100)
	void (* av_register_all)(void);
#endif
	// ffmpeg symbols to be dynamically loaded from libavutil
	unsigned (* avutil_version)(void);
	void (* av_log_set_callback)(void (*)(void*, int, const char*, va_list));
	void (* av_log_set_level)(int);
	int  (* av_strerror)(int, char *, size_t);
	void * (* av_malloc)(size_t);
	void (* av_freep)(void *);
	int (* av_dict_set)(AVDictionary **, const char *, const char *, int);
	void (* av_dict_free)(AVDictionary **);
#endif
};

static struct ff_s *ff;

extern log_level loglevel;

extern struct buffer *streambuf;
extern struct buffer *outputbuf;
extern struct streamstate stream;
extern struct outputstate output;
extern struct decodestate decode;
extern struct processstate process;

#define LOCK_S   mutex_lock(streambuf->mutex)
#define UNLOCK_S mutex_unlock(streambuf->mutex)
#define LOCK_O   mutex_lock(outputbuf->mutex)
#define UNLOCK_O mutex_unlock(outputbuf->mutex)
#if PROCESS
#define LOCK_O_direct   if (decode.direct) mutex_lock(outputbuf->mutex)
#define UNLOCK_O_direct if (decode.direct) mutex_unlock(outputbuf->mutex)
#define IF_DIRECT(x)    if (decode.direct) { x }
#define IF_PROCESS(x)   if (!decode.direct) { x }
#else
#define LOCK_O_direct   mutex_lock(outputbuf->mutex)
#define UNLOCK_O_direct mutex_unlock(outputbuf->mutex)
#define IF_DIRECT(x)    { x }
#define IF_PROCESS(x)
#endif

#if LINKALL
#define AV(h, fn, ...)       (av_ ## fn)(__VA_ARGS__)
#define AVIO(h, fn, ...)     (avio_ ## fn)(__VA_ARGS__)
#define AVCODEC(h, fn, ...)  (avcodec_ ## fn)(__VA_ARGS__)
#define AVFORMAT(h, fn, ...) (avformat_ ## fn)(__VA_ARGS__)
#else
#define AV(h, fn, ...)       (h)->av_##fn(__VA_ARGS__)
#define AVIO(h, fn, ...)     (h)->avio_##fn(__VA_ARGS__)
#define AVCODEC(h, fn, ...)  (h)->avcodec_##fn(__VA_ARGS__)
#define AVFORMAT(h, fn, ...) (h)->avformat_##fn(__VA_ARGS__)
#endif


// our own version of useful error function not included in earlier ffmpeg versions
static char *av__err2str(int errnum) {
	static char buf[64];
	AV(ff, strerror, errnum, buf, 64); 
	return buf;
}

// parser to extract asf data packet length from asf header
const u8_t header_guid[16] = { 0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C };
const u8_t file_props_guid[16] = { 0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11, 0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65 };

static int _parse_packlen(void) {
	int bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
	u8_t *ptr = streambuf->readp;
	int remain = 1;

	while (bytes >= 24 && remain > 0) {
		u32_t len = *(ptr+16) | *(ptr+17) << 8 | *(ptr+18) << 16 | *(ptr+19) << 24; // assume msb 32 bits are 0
		if (!memcmp(ptr, header_guid, 16) && bytes >= 30) {
			ptr    += 30;
			bytes  -= 30;
			remain = len - 30;
			continue;
		}
		if (!memcmp(ptr, file_props_guid, 16) && len == 104) {
			u32_t packlen = *(ptr+92) | *(ptr+93) << 8 | *(ptr+94) << 16 | *(ptr+95) << 24;
			LOG_INFO("asf packet len: %u", packlen);
			return packlen;
		}
		ptr    += len;
		bytes  -= len;
		remain -= len;
	}

	LOG_WARN("could not parse packet length");
	return 0;
}

static int _read_data(void *opaque, u8_t *buffer, int buf_size) {
	unsigned int bytes;
	unsigned wait_ms = 0;

	while (1) {
		LOCK_S;
		bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
		if (bytes > 0) {
			ff->end_of_stream = false;
			break;
		}
		if (stream.state <= DISCONNECT) {
			ff->end_of_stream = true;
			UNLOCK_S;
			return AVERROR_EOF;
		}
		UNLOCK_S;

		if (decode.state != DECODE_RUNNING || wait_ms >= 2000) {
			return AVERROR_EXIT;
		}
		usleep(2000);
		wait_ms += 2;
	}

	bytes = min(bytes, buf_size);

	// for chunked wma extract asf header and data frames from framing structure
	// pad asf data frames to size of packet extracted from asf header
	if (ff->wma_mmsh) {
		unsigned chunk_type = 0, chunk_len = 0;
		
		if (ff->mmsh_bytes_left) {
			// bytes remaining from previous frame
			if (bytes >= ff->mmsh_bytes_left) {
				bytes = ff->mmsh_bytes_left;
				ff->mmsh_bytes_left = 0;
			} else {
				ff->mmsh_bytes_left -= bytes;
			}
		} else if (ff->mmsh_bytes_pad) {
			// add padding for previous frame
			bytes = min(ff->mmsh_bytes_pad, buf_size);
			memset(buffer, 0, bytes);
			ff->mmsh_bytes_pad -= bytes;
			UNLOCK_S;
			return bytes;
		} else if (bytes >= 12) {
			// new chunk header
			chunk_type = (*(streambuf->readp) & 0x7f) | *(streambuf->readp + 1) << 8;
			chunk_len = *(streambuf->readp + 2) | *(streambuf->readp + 3) << 8;
			_buf_inc_readp(streambuf, 12);
			bytes -= 12;
		} else if (_buf_used(streambuf) >= 12) {
			// new chunk header split over end of streambuf, read in two
			u8_t header[12];
			memcpy(header, streambuf->readp, bytes);
			_buf_inc_readp(streambuf, bytes);
			memcpy(header + bytes, streambuf->readp, 12 - bytes);
			_buf_inc_readp(streambuf, 12 - bytes);
			chunk_type = (header[0] & 0x7f) | header[1] << 8;
			chunk_len  = header[2] | header[3] << 8;
			bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
			bytes = min(bytes, buf_size);
		} else {
			// should not get here...
			LOG_ERROR("chunk parser stalled bytes: %u %u", bytes, _buf_used(streambuf));
			UNLOCK_S;
			return 0;
		}
		
		if (chunk_type && chunk_len) {
			if (chunk_type == 0x4824) {
				// asf header - parse packet length
				ff->mmsh_packet_len = _parse_packlen();
				ff->mmsh_bytes_pad = 0;
			} else if (chunk_type == 0x4424 && ff->mmsh_packet_len) {
				// asf data packet - add padding
				ff->mmsh_bytes_pad = ff->mmsh_packet_len - chunk_len + 8;
			} else {
				LOG_INFO("unknown chunk: %04x", chunk_type);
				// other packet - no padding
				ff->mmsh_bytes_pad = 0;
			}
	
			if (chunk_len - 8 <= bytes) {
				bytes = chunk_len - 8;
				ff->mmsh_bytes_left = 0;
			} else {
				ff->mmsh_bytes_left = chunk_len - 8 - bytes;
			}
		}

	}

	memcpy(buffer, streambuf->readp, bytes);

	_buf_inc_readp(streambuf, bytes);

	if (ff->mmsh_bytes_pad && bytes + ff->mmsh_bytes_pad < buf_size) {
		memset(buffer + bytes, 0, ff->mmsh_bytes_pad);
		bytes += ff->mmsh_bytes_pad;
		ff->mmsh_bytes_pad = 0;
	}

	UNLOCK_S;

	return bytes;
}

#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(61,0,0)
static int _write_spdif_data(void *opaque, const u8_t *buffer, int buf_size) {
#else
static int _write_spdif_data(void *opaque, u8_t *buffer, int buf_size) {
#endif
	int remain = buf_size;
	const u8_t *src = buffer;
	unsigned frame_bytes = output_frame_bytes(ff->bitstream_channels, true);

	while (remain > 0) {
		unsigned space, chunk;

		LOCK_O;
		space = _buf_space(outputbuf);
		space = (space / frame_bytes) * frame_bytes;
		if (space > 0) {
			chunk = min((unsigned)remain, space);
			chunk = min(chunk, _buf_cont_write(outputbuf));
			memcpy(outputbuf->writep, src, chunk);
			_buf_inc_writep(outputbuf, chunk);
			src += chunk;
			remain -= (int)chunk;
			UNLOCK_O;
			continue;
		}
		if (output.state <= OUTPUT_STOPPED) {
			UNLOCK_O;
			return AVERROR_EXIT;
		}
		UNLOCK_O;

		if (decode.state != DECODE_RUNNING) {
			return AVERROR_EXIT;
		}
		usleep(2000);
	}

	return buf_size;
}

static decode_state ff_decode(void) {
	int r;
	s32_t *optr = NULL;

	if (decode.new_stream) {

		AVIOContext *avio;
		AVStream *av_stream;
		const AVCodec *codec;
		int o;
		int audio_stream = -1;

		ff->mmsh_bytes_left = ff->mmsh_bytes_pad = ff->mmsh_packet_len = 0;

		if (!(ff->bitstream_mode && ff->bitstream_format == 'T')) {
			if (!ff->readbuf) {
				ff->readbuf = AV(ff, malloc, READ_SIZE +  AV_INPUT_BUFFER_PADDING_SIZE);
			}

			avio = AVIO(ff, alloc_context, ff->readbuf, READ_SIZE, 0, NULL, _read_data, NULL, NULL);
			avio->seekable = 0;

			ff->formatC = AVFORMAT(ff, alloc_context);
			if (ff->formatC == NULL) {
				LOG_ERROR("null context");
				return DECODE_ERROR;
			}

			ff->formatC->pb = avio;
			ff->formatC->flags |= AVFMT_FLAG_CUSTOM_IO;
			if (!ff->bitstream_mode) {
				ff->formatC->flags |= AVFMT_FLAG_NOPARSE;
			}

#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(59,0,0)
			o = AVFORMAT(ff, open_input, &ff->formatC, "", (AVInputFormat *)ff->input_format, NULL);
#else
			o = AVFORMAT(ff, open_input, &ff->formatC, "", ff->input_format, NULL);
#endif
			if (o < 0) {
				LOG_WARN("avformat_open_input: %d %s", o, av__err2str(o));
				return DECODE_ERROR;
			}

			LOG_INFO("format: name:%s lname:%s", ff->formatC->iformat->name, ff->formatC->iformat->long_name);

			if (ff->bitstream_mode) {
				ff->formatC->probesize = 16384;
				ff->formatC->max_analyze_duration = 100000;
			}
		
			o = AVFORMAT(ff, find_stream_info, ff->formatC, NULL);
			if (o < 0) {
				LOG_WARN("avformat_find_stream_info: %d %s", o, av__err2str(o));
				return DECODE_ERROR;
			}
			
			if (ff->wma && ff->wma_playstream < ff->formatC->nb_streams) {
				if (ff->formatC->streams[ff->wma_playstream]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
					LOG_INFO("using wma stream sent from server: %i", ff->wma_playstream);
					audio_stream = ff->wma_playstream;
				}
			}

			if (audio_stream == -1) {
				unsigned int i;
				for (i = 0; i < ff->formatC->nb_streams; ++i) {
					if (ff->formatC->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
						audio_stream = i;
						LOG_INFO("found stream: %i", i);
						break;
					}
				}
			}

			if (audio_stream == -1) {
				LOG_WARN("no audio stream found");
				return DECODE_ERROR;
			}

			av_stream = ff->formatC->streams[audio_stream];
			ff->codecP = av_stream->codecpar;
		} else {
			ff->codecP = NULL;
		}

		ff->avpkt = AV(ff, packet_alloc);
		if (ff->avpkt == NULL) {
			LOG_ERROR("can't allocate avpkt");
			return DECODE_ERROR;
		}

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58,133,100)
		AV(ff, init_packet, ff->avpkt); // on older ffmpeg, this still is needed to e.g. set ->pos to -1
#endif

		if (ff->bitstream_mode) {
			if (!ff->out_formatC) {
				AVStream *out_stream;
				AVDictionary *opts = NULL;
				AVIOContext *out_avio;

				if (!ff->writebuf) {
					ff->writebuf = AV(ff, malloc, SPDIF_WRITE_SIZE);
				}
				out_avio = AVIO(ff, alloc_context, ff->writebuf, SPDIF_WRITE_SIZE, 1, NULL, NULL, _write_spdif_data, NULL);
				if (!out_avio) {
					LOG_ERROR("can't allocate spdif avio");
					return DECODE_ERROR;
				}
				out_avio->seekable = 0;

				o = AVFORMAT(ff, alloc_output_context2, &ff->out_formatC, NULL, "spdif", NULL);
				if (o < 0 || !ff->out_formatC) {
					LOG_ERROR("avformat_alloc_output_context2 (spdif): %d %s", o, av__err2str(o));
					AV(ff, freep, &out_avio->buffer);
					ff->writebuf = NULL;
					AV(ff, freep, &out_avio);
					return DECODE_ERROR;
				}

				ff->out_formatC->pb = out_avio;
				ff->out_formatC->flags |= AVFMT_FLAG_CUSTOM_IO;

				out_stream = AVFORMAT(ff, new_stream, ff->out_formatC, NULL);
				if (!out_stream) {
					LOG_ERROR("can't allocate spdif output stream");
					return DECODE_ERROR;
				}

				if (ff->codecP) {
					if (AVCODEC(ff, parameters_copy, out_stream->codecpar, ff->codecP) < 0) {
						LOG_ERROR("can't copy codecpar to spdif stream");
						return DECODE_ERROR;
					}
				} else {
					int thd_rate = 48000;
					LOCK_S;
					if (_buf_used(streambuf) >= 12) {
						u8_t ms[12];
						size_t c = _buf_cont_read(streambuf);
						for (size_t k = 0; k < 12; k++) {
							ms[k] = (k < c) ? streambuf->readp[k] : streambuf->buf[k - c];
						}
						if (ms[4] == 0xF8 && ms[5] == 0x72 && ms[6] == 0x6F && ms[7] == 0xBA) {
							u8_t rb = (ms[8] >> 4) & 0x0F;
							thd_rate = ((rb & 8) ? 44100 : 48000) << (rb & 7);
						}
					}
					UNLOCK_S;
					out_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
					out_stream->codecpar->codec_id = AV_CODEC_ID_TRUEHD;
					out_stream->codecpar->sample_rate = thd_rate;
				}
				out_stream->codecpar->codec_tag = 0;

				if (ff->bitstream_format == 'H') {
					AV(ff, dict_set, &opts, "dtshd_rate", "768000", 0);
				}

				o = AVFORMAT(ff, write_header, ff->out_formatC, &opts);
				AV(ff, dict_free, &opts);
				if (o < 0) {
					LOG_ERROR("avformat_write_header (spdif): %d %s", o, av__err2str(o));
					return DECODE_ERROR;
				}
				ff->out_bitstream_format = ff->bitstream_format;
				ff->header_written = true;
				ff->trailer_written = false;
				ff->spdif_pts = 0;
			} else {
				LOG_INFO("reusing persistent spdif muxer for gapless bitstream (fmt=%c)", ff->bitstream_format);
			}

			LOCK_O;
			LOG_INFO("setting bitstream track_start (fmt=%c, ch=%u, rate=192000)", ff->bitstream_format, ff->bitstream_channels);
			output.next_sample_rate = 192000;
			output.next_channels = ff->bitstream_channels;
			output.next_is_bitstream = true;
			output.next_replay_gain = FIXED_ONE;
			IF_DSD( output.next_fmt = PCM; )
			output.track_start = outputbuf->writep;
			output.fade = FADE_INACTIVE;
#if PROCESS
			decode.direct = true;
			decode.process = false;
#endif
			decode.new_stream = false;
			UNLOCK_O;
		} else {
			ff->codecC = AVCODEC (ff, alloc_context3, NULL);
			if ( ff->codecC == NULL ) {
				LOG_ERROR("can't allocate avctx");
				return DECODE_ERROR;
			}

			if ( (AVCODEC(ff, parameters_to_context, ff->codecC, ff->codecP) ) < 0) {
				AVCODEC(ff, free_context, &ff->codecC);

				LOG_ERROR("can't initialize avctx");
				return DECODE_ERROR;
			}

			codec = AVCODEC(ff, find_decoder, ff->codecP->codec_id);

			AVCODEC(ff, open2, ff->codecC, codec, NULL);

			ff->frame = AV(ff, frame_alloc);

			LOCK_O;
			LOG_INFO("setting track_start");
			output.next_sample_rate = decode_newstream(ff->codecP->sample_rate, output.supported_rates);
			IF_DSD(	output.next_fmt = PCM; )
			output.track_start = outputbuf->writep;
			if (output.fade_mode) _checkfade(true);
			decode.new_stream = false;
			UNLOCK_O;
		}
	}

	if (ff->bitstream_mode && ff->bitstream_format == 'T') {
		unsigned wait_ms = 0;
		size_t avail, au_words, au_bytes, cont, copied;
		u8_t hdr[2];

		while (1) {
			LOCK_S;
			avail = _buf_used(streambuf);
			if (avail >= 4) {
				break;
			}
			if (stream.state <= DISCONNECT) {
				UNLOCK_S;
				ff->end_of_stream = true;
				ff->stream_completed = true;
				LOG_INFO("decode complete");
				return DECODE_COMPLETE;
			}
			UNLOCK_S;
			if (decode.state != DECODE_RUNNING || wait_ms >= 2000) {
				return DECODE_STOPPED;
			}
			usleep(2000);
			wait_ms += 2;
		}

		cont = _buf_cont_read(streambuf);
		hdr[0] = streambuf->readp[0];
		hdr[1] = (cont >= 2) ? streambuf->readp[1] : streambuf->buf[0];
		au_words = ((hdr[0] & 0x0F) << 8) | hdr[1];
		au_bytes = au_words * 2;
		if (au_bytes < 10 || au_bytes > 4096) {
			UNLOCK_S;
			LOG_ERROR("invalid TrueHD access unit size: %zu", au_bytes);
			return DECODE_ERROR;
		}

		while (avail < au_bytes) {
			if (stream.state <= DISCONNECT) {
				UNLOCK_S;
				ff->end_of_stream = true;
				ff->stream_completed = true;
				LOG_INFO("decode complete");
				return DECODE_COMPLETE;
			}
			UNLOCK_S;
			if (decode.state != DECODE_RUNNING || wait_ms >= 2000) {
				return DECODE_STOPPED;
			}
			usleep(2000);
			wait_ms += 2;
			LOCK_S;
			avail = _buf_used(streambuf);
		}

		if (AV(ff, new_packet, ff->avpkt, (int)au_bytes) < 0) {
			UNLOCK_S;
			LOG_ERROR("can't allocate TrueHD packet (%zu bytes)", au_bytes);
			return DECODE_ERROR;
		}
		copied = 0;
		while (copied < au_bytes) {
			size_t chunk = min(au_bytes - copied, _buf_cont_read(streambuf));
			memcpy(ff->avpkt->data + copied, streambuf->readp, chunk);
			_buf_inc_readp(streambuf, chunk);
			copied += chunk;
		}
		ff->end_of_stream = false;
		UNLOCK_S;
	} else if ((r = AV(ff, read_frame, ff->formatC, ff->avpkt)) < 0) {
		if (r == AVERROR_EOF) {
			ff->stream_completed = true;
			LOG_INFO("decode complete");
			return DECODE_COMPLETE;
		} else if (r == AVERROR_EXIT || decode.state != DECODE_RUNNING) {
			return DECODE_STOPPED;
		} else {
			LOG_ERROR("av_read_frame error: %i %s", r, av__err2str(r));
		}
		return DECODE_RUNNING;
	}

	if (ff->bitstream_mode) {
		if (ff->bitstream_format == 'T' && ff->spdif_pts == 0) {
			// Fresh spdifenc muxer requires its first TrueHD packet to contain a Major Sync header (0xF8726FBA)
			if (ff->avpkt->size < 22 ||
			    ff->avpkt->data[4] != 0xF8 || ff->avpkt->data[5] != 0x72 ||
			    ff->avpkt->data[6] != 0x6F || ff->avpkt->data[7] != 0xBA) {
				AV(ff, packet_unref, ff->avpkt);
				return DECODE_RUNNING;
			}
		}
		int64_t pkt_dur = ff->avpkt->duration > 0 ? ff->avpkt->duration : 40;
		ff->avpkt->stream_index = 0;
		ff->avpkt->pos = -1;
		ff->avpkt->pts = ff->spdif_pts;
		ff->avpkt->dts = ff->spdif_pts;
		ff->spdif_pts += pkt_dur;
		r = AV(ff, interleaved_write_frame, ff->out_formatC, ff->avpkt);
		AVIO(ff, flush, ff->out_formatC->pb);
		AV(ff, packet_unref, ff->avpkt);
		if (r < 0) {
			if (decode.state == DECODE_STOPPED) {
				return DECODE_STOPPED;
			}
			LOG_ERROR("spdif mux error: %i %s", r, av__err2str(r));
			return DECODE_ERROR;
		}
		return DECODE_RUNNING;
	}

	IF_PROCESS(
		optr = (s32_t *)process.inbuf;
		process.in_frames = 0;
	);

	if ((r = AVCODEC(ff, send_packet, ff->codecC, ff->avpkt)) < 0) {
		AV(ff, packet_unref, ff->avpkt);

		if (r == AVERROR_EOF) {
			LOG_DEBUG("av_send_packet reports eof");
			return DECODE_COMPLETE;
		} else {
			LOG_ERROR("av_send_packet error: %i %s", r, av__err2str(r));

			// EAGAIN is treated as an unrecoverable error here since all pending output frames are processed below
			if (r == AVERROR(EAGAIN) || r == AVERROR(EINVAL) || r == AVERROR(ENOMEM))
				return DECODE_ERROR;

			// retain existing behavior (continue decoding) for legitimate decoding errors
			return DECODE_RUNNING;
		}
	} else {
		// a single input packet can lead to multiple output frames - process all of them
		while ((r = AVCODEC(ff, receive_frame, ff->codecC, ff->frame)) >= 0) {
			s16_t *iptr16 = (s16_t *)ff->frame->data[0];
			s32_t *iptr32 = (s32_t *)ff->frame->data[0];
			s16_t *iptr16l = (s16_t *)ff->frame->data[0];
			s16_t *iptr16r = (s16_t *)ff->frame->data[1];
			s32_t *iptr32l = (s32_t *)ff->frame->data[0];
			s32_t *iptr32r = (s32_t *)ff->frame->data[1];
			float *iptrfl = (float *)ff->frame->data[0];
			float *iptrfr = (float *)ff->frame->data[1];

			frames_t frames = ff->frame->nb_samples;
#if HAVE_CH_LAYOUT
			LOG_SDEBUG("got audio channels: %u samples: %u format: %u", ff->codecC->ch_layout.nb_channels, ff->frame->nb_samples,
					   ff->codecC->sample_fmt);
#else
			LOG_SDEBUG("got audio channels: %u samples: %u format: %u", ff->codecC->channels, ff->frame->nb_samples,
					   ff->codecC->sample_fmt);
#endif
			
			LOCK_O_direct;

			while (frames > 0) {
				frames_t count;
				frames_t f;
				
				IF_DIRECT(
					optr = (s32_t *)outputbuf->writep;
					f = min(_buf_space(outputbuf), _buf_cont_write(outputbuf)) / BYTES_PER_FRAME;
					f = min(f, frames);
				);

				IF_PROCESS(
					if (process.in_frames + frames > process.max_in_frames) {
						LOG_WARN("exceeded process buffer size - dropping frames");
						break;
					}
					f = frames;	   
				);

				count = f;
				
#if HAVE_CH_LAYOUT
				if (ff->codecC->ch_layout.nb_channels == 2) {
#else
				if (ff->codecC->channels == 2) {
#endif
					if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S16) {
						while (count--) {
							*optr++ = *iptr16++ << 16;
							*optr++ = *iptr16++ << 16;
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S32) {
						while (count--) {
							*optr++ = *iptr32++;
							*optr++ = *iptr32++;
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S16P) {
						while (count--) {
							*optr++ = *iptr16l++ << 16;
							*optr++ = *iptr16r++ << 16;
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S32P) {
						while (count--) {
							*optr++ = *iptr32l++;
							*optr++ = *iptr32r++;
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_FLTP) {
						while (count--) {
							double scaledl = *iptrfl++ * 0x7fffffff;
							double scaledr = *iptrfr++ * 0x7fffffff;
							if (scaledl > 2147483647.0) scaledl = 2147483647.0;
							if (scaledl < -2147483648.0) scaledl = -2147483648.0;
							if (scaledr > 2147483647.0) scaledr = 2147483647.0;
							if (scaledr < -2147483648.0) scaledr = -2147483648.0;
							*optr++ = (s32_t)scaledl;
							*optr++ = (s32_t)scaledr;
						}
					} else {
						LOG_WARN("unsupported sample format: %u", ff->codecC->sample_fmt);
					}
#if HAVE_CH_LAYOUT
				} else if (ff->codecC->ch_layout.nb_channels == 1) {
#else
				} else if (ff->codecC->channels == 1) {
#endif
					if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S16) {
						while (count--) {
							*optr++ = *iptr16   << 16;
							*optr++ = *iptr16++ << 16;
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S32) {
						while (count--) {
							*optr++ = *iptr32;
							*optr++ = *iptr32++;						
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S16P) {
						while (count--) {
							*optr++ = *iptr16l << 16;
							*optr++ = *iptr16l++ << 16;
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S32P) {
						while (count--) {
							*optr++ = *iptr32l;
							*optr++ = *iptr32l++;
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_FLTP) {
						while (count--) {
							double scaled = *iptrfl++ * 0x7fffffff;
							if (scaled > 2147483647.0) scaled = 2147483647.0;
							if (scaled < -2147483648.0) scaled = -2147483648.0;
							*optr++ = (s32_t)scaled;
							*optr++ = (s32_t)scaled;
						}
					} else {
						LOG_WARN("unsupported sample format: %u", ff->codecC->sample_fmt);
					}
				} else {
					LOG_WARN("unsupported number of channels");
				}
				
				frames -= f;
				
				IF_DIRECT(
					_buf_inc_writep(outputbuf, f * BYTES_PER_FRAME);
				);

				IF_PROCESS(
					process.in_frames += f;
				);
			}
			
			UNLOCK_O_direct;
		}
	}

	AV(ff, packet_unref, ff->avpkt);

	if (r == AVERROR(EAGAIN)) {
		// EAGAIN is expected if there are no more pending frames
	} else if (r == AVERROR_EOF) {
		LOG_DEBUG("av_receive_frame reports eof");
		return DECODE_COMPLETE;
	} else {
		LOG_ERROR("av_receive_frame error: %i %s", r, av__err2str(r));

		if (r == AVERROR(EINVAL))
			return DECODE_ERROR;

		// retain existing behavior (continue decoding) for legitimate decoding errors
	}

	return DECODE_RUNNING;
}

static void _free_ff_spdif(void) {
	if (ff->out_formatC) {
		if (ff->header_written && !ff->trailer_written && ff->out_formatC->pb) {
			ff->trailer_written = true;
			AV(ff, write_trailer, ff->out_formatC);
			AVIO(ff, flush, ff->out_formatC->pb);
		}
		if (ff->out_formatC->pb) {
			AV(ff, freep, &ff->out_formatC->pb->buffer);
			ff->writebuf = NULL;
			AV(ff, freep, &ff->out_formatC->pb);
		}
		AVFORMAT(ff, free_context, ff->out_formatC);
		ff->out_formatC = NULL;
	}
	ff->out_bitstream_format = 0;
	ff->stream_completed = false;
	ff->header_written = false;
	ff->trailer_written = false;
	ff->spdif_pts = 0;
}

static void _free_ff_input(void) {
	if (ff->codecC) {
		AVCODEC(ff, free_context, &ff->codecC);
		ff->codecC = NULL;
	}

	if (ff->formatC) {
		if (ff->formatC->pb) {
			// per ffmpeg docs, the buffer originally pointed to by ff->readbuf may be dynamically freed and reallocated behind the scenes, so this is the one that must be freed
			// otherwise, a double free can occur (seen by valgrind), resulting in e.g. SIGILL
			AV(ff, freep, &ff->formatC->pb->buffer);
			ff->readbuf = NULL;
			AV(ff, freep, &ff->formatC->pb);
		}

		AVFORMAT(ff, free_context, ff->formatC);
		ff->formatC = NULL;
	}

	if (ff->frame) {
		AV(ff, frame_free, &ff->frame);
		ff->frame = NULL;
	}

	if (ff->avpkt) {
		AV(ff, packet_unref, ff->avpkt);
		AV(ff, freep, &ff->avpkt);
		ff->avpkt = NULL;
	}
}

static void _free_ff_data(void) {
	_free_ff_spdif();
	_free_ff_input();
}

static void _prepare_bitstream_open(char fmt) {
	bool natural_eof = ff->stream_completed;
	ff->stream_completed = false;
	_free_ff_input();
	LOCK_O;
	bool keep_spdif = (ff->out_formatC != NULL && ff->out_bitstream_format == fmt &&
	                   output.is_bitstream && output.state > OUTPUT_STOPPED &&
	                   (fmt != 'T' || natural_eof));
	UNLOCK_O;
	if (!keep_spdif) {
		_free_ff_spdif();
	}
}

static void ff_open_wma(u8_t size, u8_t rate, u8_t chan, u8_t endianness) {
	_free_ff_data();

	ff->input_format = AV(ff, find_input_format, "asf");
	if (ff->input_format == NULL) {
		LOG_ERROR("asf format not supported by ffmpeg library");
	}

	ff->wma = true;
	ff->bitstream_mode = false;
	ff->wma_mmsh = size - '0';
	ff->wma_playstream = rate - 1;
	ff->wma_metadatastream = chan != '?' ? chan : 0;

	LOG_INFO("open wma chunking: %u playstream: %u metadatastream: %u", ff->wma_mmsh, ff->wma_playstream, ff->wma_metadatastream);
}

static void ff_open_alac(u8_t size, u8_t rate, u8_t chan, u8_t endianness) {
	_free_ff_data();

	ff->input_format = AV(ff, find_input_format, "mp4");
	if (ff->input_format == NULL) {
		LOG_ERROR("mp4 format not supported by ffmpeg library");
	}

	ff->wma = false;
	ff->bitstream_mode = false;
	ff->wma_mmsh = 0;

	LOG_INFO("open alac");
}

static void ff_open_ec3(u8_t size, u8_t rate, u8_t chan, u8_t endianness) {
	_prepare_bitstream_open('E');
	ff->input_format = AV(ff, find_input_format, "eac3");
	if (ff->input_format == NULL) {
		LOG_ERROR("eac3 format not supported by ffmpeg library");
	}
	ff->wma = false;
	ff->wma_mmsh = 0;
	ff->bitstream_mode = true;
	ff->bitstream_format = 'E';
	ff->bitstream_channels = 2;
	LOG_INFO("open eac3 bitstream passthrough");
}

static void ff_open_thd(u8_t size, u8_t rate, u8_t chan, u8_t endianness) {
	_prepare_bitstream_open('T');
	ff->input_format = AV(ff, find_input_format, "truehd");
	if (ff->input_format == NULL) {
		LOG_ERROR("truehd format not supported by ffmpeg library");
	}
	ff->wma = false;
	ff->wma_mmsh = 0;
	ff->bitstream_mode = true;
	ff->bitstream_format = 'T';
	ff->bitstream_channels = 8;
	LOG_INFO("open truehd bitstream passthrough (HBR 8ch)");
}

static void ff_open_dtshd(u8_t size, u8_t rate, u8_t chan, u8_t endianness) {
	_prepare_bitstream_open('H');
	ff->input_format = AV(ff, find_input_format, "dts");
	if (ff->input_format == NULL) {
		LOG_ERROR("dts format not supported by ffmpeg library");
	}
	ff->wma = false;
	ff->wma_mmsh = 0;
	ff->bitstream_mode = true;
	ff->bitstream_format = 'H';
	ff->bitstream_channels = 8;
	LOG_INFO("open dts-hd bitstream passthrough (HBR 8ch, dtshd_rate=768000)");
}

static void ff_close(void) {
	_free_ff_data();
}

static bool load_ff() {
#if !LINKALL
	void *handle_codec = NULL, *handle_format = NULL, *handle_util = NULL;
	char name[30];
	char *err;

	// we try to load the ffmpeg library version which matches the header file we are compiled with as structs differ between versions

	sprintf(name, LIBAVCODEC, LIBAVCODEC_VERSION_MAJOR);
	handle_codec = dlopen(name, RTLD_NOW);
	if (!handle_codec) {
		LOG_INFO("dlerror: %s", dlerror());
		return false;
	}

	sprintf(name, LIBAVFORMAT, LIBAVFORMAT_VERSION_MAJOR);
	handle_format = dlopen(name, RTLD_NOW);
	if (!handle_format) {
		LOG_INFO("dlerror: %s", dlerror());
		return false;
	}

	sprintf(name, LIBAVUTIL, LIBAVUTIL_VERSION_MAJOR);
	handle_util = dlopen(name, RTLD_NOW);
	if (!handle_util) {
		LOG_INFO("dlerror: %s", dlerror());
		return false;
	}

	ff->avcodec_version = dlsym(handle_codec, "avcodec_version");
	ff->avcodec_find_decoder = dlsym(handle_codec, "avcodec_find_decoder");
	ff->avcodec_open2 = dlsym(handle_codec, "avcodec_open2");
	ff->av_frame_alloc = dlsym(handle_codec, "av_frame_alloc");
	ff->av_frame_free = dlsym(handle_codec, "av_frame_free");
	ff->avcodec_send_packet = dlsym(handle_codec, "avcodec_send_packet");
	ff->avcodec_receive_frame = dlsym(handle_codec, "avcodec_receive_frame");
	ff->avcodec_alloc_context3 = dlsym(handle_format, "avcodec_alloc_context3");
	ff->avcodec_free_context = dlsym(handle_format, "avcodec_free_context");
	ff->avcodec_parameters_to_context = dlsym(handle_format, "avcodec_parameters_to_context");
	ff->avcodec_parameters_copy = dlsym(handle_codec, "avcodec_parameters_copy");
	ff->av_packet_alloc = dlsym(handle_codec, "av_packet_alloc");
	ff->av_new_packet = dlsym(handle_codec, "av_new_packet");
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58,133,100)
	ff->av_init_packet = dlsym(handle_codec, "av_init_packet");
#endif
	ff->av_packet_unref = dlsym(handle_codec, "av_packet_unref");

	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);		
		return false;
	}
	
	LOG_INFO("loaded "LIBAVCODEC" (%u.%u.%u)", LIBAVCODEC_VERSION_MAJOR, ff->avcodec_version() >> 16, (ff->avcodec_version() >> 8) & 0xff, ff->avcodec_version() & 0xff);

	ff->avformat_version = dlsym(handle_format, "avformat_version");
	ff->avformat_alloc_context = dlsym(handle_format, "avformat_alloc_context");
	ff->avformat_alloc_output_context2 = dlsym(handle_format, "avformat_alloc_output_context2");
	ff->avformat_new_stream = dlsym(handle_format, "avformat_new_stream");
	ff->avformat_write_header = dlsym(handle_format, "avformat_write_header");
	ff->av_interleaved_write_frame = dlsym(handle_format, "av_interleaved_write_frame");
	ff->av_write_trailer = dlsym(handle_format, "av_write_trailer");
	ff->avformat_free_context = dlsym(handle_format, "avformat_free_context");
	ff->avformat_open_input = dlsym(handle_format, "avformat_open_input");
	ff->avformat_find_stream_info = dlsym(handle_format, "avformat_find_stream_info");
	ff->avio_alloc_context = dlsym(handle_format, "avio_alloc_context");
	ff->avio_flush = dlsym(handle_format, "avio_flush");
	ff->av_read_frame = dlsym(handle_format, "av_read_frame");
	ff->av_find_input_format= dlsym(handle_format, "av_find_input_format");
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58,9,100)
	ff->av_register_all = dlsym(handle_format, "av_register_all");
#endif

	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);		
		return false;
	}

	LOG_INFO("loaded "LIBAVFORMAT" (%u.%u.%u)", LIBAVFORMAT_VERSION_MAJOR, ff->avformat_version() >> 16, (ff->avformat_version() >> 8) & 0xff, ff->avformat_version() & 0xff);

	ff->avutil_version = dlsym(handle_util, "avutil_version");
	ff->av_log_set_callback = dlsym(handle_util, "av_log_set_callback");
	ff->av_log_set_level = dlsym(handle_util, "av_log_set_level");
	ff->av_strerror = dlsym(handle_util, "av_strerror");
	ff->av_malloc = dlsym(handle_util, "av_malloc");
	ff->av_freep = dlsym(handle_util, "av_freep");
	ff->av_dict_set = dlsym(handle_util, "av_dict_set");
	ff->av_dict_free = dlsym(handle_util, "av_dict_free");

	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);		
		return false;
	}

	LOG_INFO("loaded "LIBAVUTIL" (%u.%u.%u)", LIBAVUTIL_VERSION_MAJOR, ff->avutil_version() >> 16, (ff->avutil_version() >> 8) & 0xff, ff->avutil_version() & 0xff);

#endif

	return true;
}

static int ff_log_level = 0;

void av_err_callback(void *avcl, int level, const char *fmt, va_list vl) {
	if (level > ff_log_level) return;
	fprintf(stderr, "%s ffmpeg: ", logtime());
	vfprintf(stderr, fmt, vl);
	fflush(stderr);
}

static bool registered = false;

struct codec *register_ff(const char *codec) {
	if (!registered) {

		ff = malloc(sizeof(struct ff_s));
		if (!ff) {
			return NULL;
		}

		memset(ff, 0, sizeof(struct ff_s));

		if (!load_ff()) {
			return NULL;
		}

		switch (loglevel) {
		case lERROR:
			ff_log_level = AV_LOG_ERROR; break;
		case lWARN:
			ff_log_level = AV_LOG_WARNING; break;
		case lINFO:
			ff_log_level = AV_LOG_INFO; break;
		case lDEBUG:
			ff_log_level = AV_LOG_VERBOSE; break;
		default: break;
		}

		AV(ff, log_set_callback, av_err_callback);

#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58,9,100)
		AV(ff, register_all);
#endif

		registered = true;
	}

	if (!strcmp(codec, "wma")) {

		static struct codec ret = { 
			'w',         // id
			"wma,wmap,wmal", // types
			READ_SIZE,   // min read
			WRITE_SIZE,  // min space
			ff_open_wma, // open
			ff_close,    // close
			ff_decode,   // decode
		};
		
		LOG_INFO("using ffmpeg to decode wma,wmap,wmal");
		return &ret;
	}

	if (!strcmp(codec, "alc")) {

		static struct codec ret = { 
			'l',         // id
			"alc",       // types
			READ_SIZE,   // min read
			WRITE_SIZE,  // min space
			ff_open_alac,// open
			ff_close,    // close
			ff_decode,   // decode
		};
		
		LOG_INFO("using ffmpeg to decode alc");
		return &ret;
	}

	if (!strcmp(codec, "ec3")) {
		static struct codec ret = {
			'E',         // id
			"ec3",       // types
			READ_SIZE,   // min read
			SPDIF_WRITE_SIZE, // min space
			ff_open_ec3, // open
			ff_close,    // close
			ff_decode,   // decode
		};
		LOG_INFO("using ffmpeg spdif muxer for ec3 bitstream");
		return &ret;
	}

	if (!strcmp(codec, "thd")) {
		static struct codec ret = {
			'T',         // id
			"thd",       // types
			READ_SIZE,   // min read
			SPDIF_WRITE_SIZE, // min space
			ff_open_thd, // open
			ff_close,    // close
			ff_decode,   // decode
		};
		LOG_INFO("using ffmpeg spdif muxer for thd bitstream");
		return &ret;
	}

	if (!strcmp(codec, "dtshd")) {
		static struct codec ret = {
			'H',         // id
			"dtshd",     // types
			READ_SIZE,   // min read
			SPDIF_WRITE_SIZE, // min space
			ff_open_dtshd, // open
			ff_close,    // close
			ff_decode,   // decode
		};
		LOG_INFO("using ffmpeg spdif muxer for dtshd bitstream");
		return &ret;
	}

	return NULL;
}

#endif
