/* 
 *  Squeezelite - lightweight headless squeezeplay emulator for linux
 *
 *  (c) Adrian Smith 2012, triode1@btinternet.com
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

#include <FLAC/stream_decoder.h>

/*
 * Error if philippe44 patched libFLAC 1.4.3 ogg chaining support
 * found as the API is different in the 1.5+ release.
 */
#if defined(FLAC__OGG_CHAINING) && (FLAC_API_VERSION_CURRENT == 14)
#error "Upgrade to libFLAC 1.5+ for OggFlac chaining support"
#endif

#if BYTES_PER_FRAME == 4		
#define ALIGN8(n) 	(n << 8)		
#define ALIGN16(n) 	(n)
#define ALIGN24(n)	(n >> 8) 
#define ALIGN32(n)	(n >> 16)
#else
#define ALIGN8(n) 	(n << 24)		
#define ALIGN16(n) 	(n << 16)
#define ALIGN24(n)	(n << 8) 
#define ALIGN32(n)	(n)
#endif

struct flac {
	FLAC__StreamDecoder *decoder;
	u8_t container;
#if !LINKALL
	// FLAC symbols to be dynamically loaded
	const char **FLAC__StreamDecoderErrorStatusString;
	const char **FLAC__StreamDecoderStateString;
	FLAC__StreamDecoder * (* FLAC__stream_decoder_new)(void);
	FLAC__bool (* FLAC__stream_decoder_reset)(FLAC__StreamDecoder *decoder);
	void (* FLAC__stream_decoder_delete)(FLAC__StreamDecoder *decoder);
	FLAC__StreamDecoderInitStatus (* FLAC__stream_decoder_init_stream)(
		FLAC__StreamDecoder *decoder,
		FLAC__StreamDecoderReadCallback read_callback,
		FLAC__StreamDecoderSeekCallback seek_callback,
		FLAC__StreamDecoderTellCallback tell_callback,
		FLAC__StreamDecoderLengthCallback length_callback,
		FLAC__StreamDecoderEofCallback eof_callback,
		FLAC__StreamDecoderWriteCallback write_callback,
		FLAC__StreamDecoderMetadataCallback metadata_callback,
		FLAC__StreamDecoderErrorCallback error_callback,
		void *client_data
	);
	FLAC__StreamDecoderInitStatus (* FLAC__stream_decoder_init_ogg_stream)(
		FLAC__StreamDecoder *decoder,
		FLAC__StreamDecoderReadCallback read_callback,
		FLAC__StreamDecoderSeekCallback seek_callback,
		FLAC__StreamDecoderTellCallback tell_callback,
		FLAC__StreamDecoderLengthCallback length_callback,
		FLAC__StreamDecoderEofCallback eof_callback,
		FLAC__StreamDecoderWriteCallback write_callback,
		FLAC__StreamDecoderMetadataCallback metadata_callback,
		FLAC__StreamDecoderErrorCallback error_callback,
		void *client_data
	);
	FLAC__bool (* FLAC__stream_decoder_process_single)(FLAC__StreamDecoder *decoder);
	FLAC__StreamDecoderState (* FLAC__stream_decoder_get_state)(const FLAC__StreamDecoder *decoder);
	void (*FLAC__stream_decoder_set_metadata_respond)(FLAC__StreamDecoder* decoder, FLAC__MetadataType type);
#if FLAC_API_VERSION_CURRENT >= 14
	FLAC__bool (*FLAC__stream_decoder_set_decode_chained_stream)(FLAC__StreamDecoder* decoder, FLAC__bool allow);
	FLAC__bool (*FLAC__stream_decoder_finish_link)(FLAC__StreamDecoder* decoder);
#endif
#endif
};

static struct flac *f;

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
#define FLAC(h, fn, ...) (FLAC__ ## fn)(__VA_ARGS__)
#define FLAC_A(h, a)     (FLAC__ ## a)
#else
#define FLAC(h, fn, ...) (h)->FLAC__##fn(__VA_ARGS__)
#define FLAC_A(h, a)     (h)->FLAC__ ## a
#endif

static void metadata_cb(const FLAC__StreamDecoder* decoder, const FLAC__StreamMetadata* metadata, void* client_data) {
	switch (metadata->type) {
	case FLAC__METADATA_TYPE_STREAMINFO:
		LOG_INFO("stream parameters rate:%d, channels:%d, size:%d", metadata->data.stream_info.sample_rate, 
				 metadata->data.stream_info.channels, metadata->data.stream_info.bits_per_sample);
		break;
	case FLAC__METADATA_TYPE_VORBIS_COMMENT: {
		FLAC__StreamMetadata_VorbisComment_Entry* comment = metadata->data.vorbis_comment.comments;
		for (int i = 0; i < metadata->data.vorbis_comment.num_comments; i++, comment++) {
			LOG_INFO("stream metadata %*s", comment->length, comment->entry);
		}
	}
	default:
		break;
	}
}

static FLAC__StreamDecoderReadStatus read_cb(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *want, void *client_data) {
	size_t bytes;
	bool end;

	LOCK_S;
	bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
	bytes = min(bytes, *want);
	end = (stream.state <= DISCONNECT && bytes == 0);

	memcpy(buffer, streambuf->readp, bytes);
	_buf_inc_readp(streambuf, bytes);
	UNLOCK_S;

	*want = bytes;

	// if there's nothing in the stream buffer, libFLAC will continuously call this function as quickly as possible. slow it down.
	if (!bytes && !end)
		usleep(1000);

	return end ? FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM : FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

// FLAC (SMPTE/ITU-R) to ALSA CEA-861 speaker swizzle lookup table
// Index is ALSA output channel (0..7); value is FLAC buffer[] index, or -1 for silence fill
static const signed char flac_to_cea861_map[9][MAX_CHANNELS] = {
	{ -1, -1, -1, -1, -1, -1, -1, -1 }, // 0: invalid
	{  0,  0, -1, -1, -1, -1, -1, -1 }, // 1: 1.0 Mono -> L, R
	{  0,  1, -1, -1, -1, -1, -1, -1 }, // 2: 2.0 Stereo -> FL, FR
	{  0,  1, -1,  2, -1, -1, -1, -1 }, // 3: 3.0 (FL,FR,FC) -> FL, FR, Silence, FC, Silence, Silence
	{  0,  1, -1, -1,  2,  3, -1, -1 }, // 4: 4.0 Quad (FL,FR,BL,BR) -> FL, FR, Silence, Silence, BL, BR
	{  0,  1, -1,  2,  3,  4, -1, -1 }, // 5: 5.0 (FL,FR,FC,BL,BR) -> FL, FR, Silence, FC, BL, BR
	{  0,  1,  3,  2,  4,  5, -1, -1 }, // 6: 5.1 (FL,FR,FC,LFE,BL,BR) -> FL, FR, LFE, FC, BL, BR
	{  0,  1,  3,  2,  5,  6,  4, -1 }, // 7: 6.1 (FL,FR,FC,LFE,BC,SL,SR) -> FL, FR, LFE, FC, SL, SR, BC, Silence
	{  0,  1,  3,  2,  4,  5,  6,  7 }, // 8: 7.1 (FL,FR,FC,LFE,BL,BR,SL,SR) -> FL, FR, LFE, FC, BL, BR, SL, SR
};

static inline u8_t normalize_hdmi_channels(unsigned channels) {
	if (channels <= 2) return 2;
	if (channels <= 6) return 6;
	return 8;
}

static FLAC__StreamDecoderWriteStatus write_cb(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
											   const FLAC__int32 *const buffer[], void *client_data) {

	size_t frames = frame->header.blocksize;
	size_t offset = 0;
	unsigned bits_per_sample = frame->header.bits_per_sample;
	unsigned channels = frame->header.channels;
	u8_t out_channels;
	unsigned frame_bytes;

	if (channels == 0 || channels > MAX_CHANNELS) {
		LOG_ERROR("unsupported flac channel count: %u", channels);
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	}

	out_channels = normalize_hdmi_channels(channels);
	frame_bytes = output_frame_bytes(out_channels, false);

	FLAC__int32 *lptr = (FLAC__int32 *)buffer[0];
	FLAC__int32 *rptr = (FLAC__int32 *)buffer[channels > 1 ? 1 : 0];
	
	if (decode.new_stream) {
		LOCK_O;
		LOG_INFO("setting track_start (in_ch=%u, out_ch=%u)", channels, out_channels);
		output.track_start = outputbuf->writep;
		output.next_channels = out_channels;
		output.next_is_bitstream = false;
		decode.new_stream = false;

#if DSD
#if SL_LITTLE_ENDIAN
#define MARKER_OFFSET 2
#else
#define MARKER_OFFSET 1
#endif		
		if (channels <= 2 && bits_per_sample == 24 && is_stream_dop(((u8_t *)lptr) + MARKER_OFFSET, ((u8_t *)rptr) + MARKER_OFFSET, 4, frames)) {
			LOG_INFO("file contains DOP");
			if (output.dsdfmt == DOP_S24_LE || output.dsdfmt == DOP_S24_3LE)
				output.next_fmt = output.dsdfmt;
			else
				output.next_fmt = DOP;
			output.next_sample_rate = frame->header.sample_rate;
			output.fade = FADE_INACTIVE;
		} else {
			output.next_sample_rate = decode_newstream(frame->header.sample_rate, output.supported_rates);
			output.next_fmt = PCM;
			if (output.fade_mode) _checkfade(true);
		}
#else
		output.next_sample_rate = decode_newstream(frame->header.sample_rate, output.supported_rates);
		if (output.fade_mode) _checkfade(true);
#endif
#if PROCESS
		if (channels > 2) {
			decode.direct = true;
			decode.process = false;
		}
#endif

		UNLOCK_O;
	}

	LOCK_O_direct;

	while (frames > 0) {
		frames_t f;
		frames_t count;
		ISAMPLE_T *optr;

		IF_DIRECT( 
			optr = (ISAMPLE_T *)outputbuf->writep; 
			f = min(_buf_space(outputbuf), _buf_cont_write(outputbuf)) / frame_bytes;
		);
		IF_PROCESS(
			optr = (ISAMPLE_T *)process.inbuf;
			f = process.max_in_frames;
		);

		f = min(f, frames);

		count = f;

		if (channels <= 2) {
			if (bits_per_sample == 8) {
				while (count--) {
					*optr++ = ALIGN8(*lptr++);
					*optr++ = ALIGN8(*rptr++);
				}
			} else if (bits_per_sample == 16) {
				while (count--) {
					*optr++ = ALIGN16(*lptr++);
					*optr++ = ALIGN16(*rptr++);
				}
			} else if (bits_per_sample == 20) {
				while (count--) {
					*optr++ = ALIGN24(*lptr++ << 4);
					*optr++ = ALIGN24(*rptr++ << 4);
				}
			} else if (bits_per_sample == 24) {
				while (count--) {
					*optr++ = ALIGN24(*lptr++);
					*optr++ = ALIGN24(*rptr++);
				}
			} else if (bits_per_sample == 32) {
				while (count--) {
					*optr++ = ALIGN32(*lptr++);
					*optr++ = ALIGN32(*rptr++);
				}
			} else {
				LOG_ERROR("unsupported bits per sample: %u", bits_per_sample);
			}
		} else {
			const signed char *map = flac_to_cea861_map[channels];
			frames_t i;
			u8_t ch;

			if (bits_per_sample == 8) {
				for (i = 0; i < count; ++i) {
					size_t idx = offset + i;
					for (ch = 0; ch < out_channels; ++ch) {
						signed char src_ch = map[ch];
						*optr++ = (src_ch >= 0) ? ALIGN8(buffer[src_ch][idx]) : 0;
					}
				}
			} else if (bits_per_sample == 16) {
				for (i = 0; i < count; ++i) {
					size_t idx = offset + i;
					for (ch = 0; ch < out_channels; ++ch) {
						signed char src_ch = map[ch];
						*optr++ = (src_ch >= 0) ? ALIGN16(buffer[src_ch][idx]) : 0;
					}
				}
			} else if (bits_per_sample == 20) {
				for (i = 0; i < count; ++i) {
					size_t idx = offset + i;
					for (ch = 0; ch < out_channels; ++ch) {
						signed char src_ch = map[ch];
						*optr++ = (src_ch >= 0) ? ALIGN24(buffer[src_ch][idx] << 4) : 0;
					}
				}
			} else if (bits_per_sample == 24) {
				for (i = 0; i < count; ++i) {
					size_t idx = offset + i;
					for (ch = 0; ch < out_channels; ++ch) {
						signed char src_ch = map[ch];
						*optr++ = (src_ch >= 0) ? ALIGN24(buffer[src_ch][idx]) : 0;
					}
				}
			} else if (bits_per_sample == 32) {
				for (i = 0; i < count; ++i) {
					size_t idx = offset + i;
					for (ch = 0; ch < out_channels; ++ch) {
						signed char src_ch = map[ch];
						*optr++ = (src_ch >= 0) ? ALIGN32(buffer[src_ch][idx]) : 0;
					}
				}
			} else {
				LOG_ERROR("unsupported bits per sample: %u", bits_per_sample);
			}
		}

		frames -= f;
		offset += f;

		IF_DIRECT(
			_buf_inc_writep(outputbuf, f * frame_bytes);
		);
		IF_PROCESS(
			process.in_frames = f;
			if (frames) LOG_ERROR("unhandled case");
		);
	}

	UNLOCK_O_direct;

	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void error_cb(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data) {
	LOG_INFO("flac error: %s", FLAC_A(f, StreamDecoderErrorStatusString)[status]);
}

static void flac_close(void) {
	FLAC(f, stream_decoder_delete, f->decoder);
	f->decoder = NULL;
}

static void flac_open(u8_t sample_size, u8_t sample_rate, u8_t channels, u8_t endianness) {
	if ( f->decoder && f->container != sample_size ) {
		flac_close();
	}

	f->container = sample_size;

	if (f->decoder) {
		FLAC(f, stream_decoder_reset, f->decoder);
	} else {
		f->decoder = FLAC(f, stream_decoder_new);
	}
	
	if ( f->container == 'o' ) {
		LOG_INFO("ogg/flac container - using init_ogg_stream");

#if FLAC_API_VERSION_CURRENT >= 14
#if LINKALL
		FLAC__stream_decoder_set_decode_chained_stream(f->decoder, true);
#else
		FLAC(f, stream_decoder_set_decode_chained_stream, f->decoder, true);
#endif
		LOG_INFO("using chained stream decoding");
#else
		#pragma message ("OggFlac library does not support chaining") 
#endif
		FLAC(f, stream_decoder_set_metadata_respond, f->decoder, FLAC__METADATA_TYPE_VORBIS_COMMENT);
		FLAC(f, stream_decoder_init_ogg_stream, f->decoder, &read_cb, NULL, NULL, NULL, NULL, &write_cb, &metadata_cb, &error_cb, NULL);
	} else {
		FLAC(f, stream_decoder_init_stream, f->decoder, &read_cb, NULL, NULL, NULL, NULL, &write_cb, NULL, &error_cb, NULL);
	}
}

static decode_state flac_decode(void) {
	bool ok = FLAC(f, stream_decoder_process_single, f->decoder);
	FLAC__StreamDecoderState state = FLAC(f, stream_decoder_get_state, f->decoder);
	
	if (!ok && state != FLAC__STREAM_DECODER_END_OF_STREAM) {
		LOG_INFO("flac error: %s", FLAC_A(f, StreamDecoderStateString)[state]);
	};
	
#if FLAC_API_VERSION_CURRENT >= 14
	if (state == FLAC__STREAM_DECODER_END_OF_LINK) {
		if (!FLAC(f, stream_decoder_finish_link, f->decoder)) {
			LOG_INFO("flac error: could not finish chained link");
			return DECODE_ERROR;
		}

		return DECODE_RUNNING;
	}
#endif

	if (state == FLAC__STREAM_DECODER_END_OF_STREAM) {
		return DECODE_COMPLETE;
	} else if (state > FLAC__STREAM_DECODER_END_OF_STREAM) {
		return DECODE_ERROR;
	} else {
		return DECODE_RUNNING;
	}
}

static bool load_flac() {
#if !LINKALL
	void *handle = NULL;
	char name[30];
	char *err;

	sprintf(name, LIBFLAC, FLAC_API_VERSION_CURRENT - FLAC_API_VERSION_AGE);

        handle = dlopen(name, RTLD_NOW);

	if (!handle) {
		LOG_INFO("dlerror: %s", dlerror());
		return false;
	}

	f->FLAC__StreamDecoderErrorStatusString = dlsym(handle, "FLAC__StreamDecoderErrorStatusString");
	f->FLAC__StreamDecoderStateString = dlsym(handle, "FLAC__StreamDecoderStateString");
	f->FLAC__stream_decoder_new = dlsym(handle, "FLAC__stream_decoder_new");
	f->FLAC__stream_decoder_reset = dlsym(handle, "FLAC__stream_decoder_reset");
	f->FLAC__stream_decoder_delete = dlsym(handle, "FLAC__stream_decoder_delete");
	f->FLAC__stream_decoder_init_stream = dlsym(handle, "FLAC__stream_decoder_init_stream");
	f->FLAC__stream_decoder_init_ogg_stream = dlsym(handle, "FLAC__stream_decoder_init_ogg_stream");
	f->FLAC__stream_decoder_process_single = dlsym(handle, "FLAC__stream_decoder_process_single");
	f->FLAC__stream_decoder_get_state = dlsym(handle, "FLAC__stream_decoder_get_state");
	f->FLAC__stream_decoder_set_metadata_respond = dlsym(handle, "FLAC__stream_decoder_set_metadata_respond");
#if FLAC_API_VERSION_CURRENT >= 14
	f->FLAC__stream_decoder_set_decode_chained_stream = dlsym(handle, "FLAC__stream_decoder_set_decode_chained_stream");
	f->FLAC__stream_decoder_finish_link = dlsym(handle, "FLAC__stream_decoder_finish_link");
#endif

	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);		
		return false;
	}

	LOG_INFO("loaded %s", name);
#elif FLAC_API_VERSION_CURRENT < 14
	LOG_INFO("OggFlac chaining disabled");
#endif

	return true;
}

struct codec *register_flac(void) {
	static struct codec ret = { 
		'f',          // id
		"ogf,flc",    // types
		16384,        // min read
		204800,       // min space
		flac_open,    // open
		flac_close,   // close
		flac_decode,  // decode
	};

	f = malloc(sizeof(struct flac));
	if (!f) {
		return NULL;
	}

	f->decoder = NULL;

	if (!load_flac()) {
		return NULL;
	}

	LOG_INFO("using flac to decode ogf,flc");
	return &ret;
}
