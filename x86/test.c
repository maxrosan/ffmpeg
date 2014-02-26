#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <math.h>

#include "cmdutils.h"

#include "libavutil/avstring.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/dict.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/avassert.h"
#include "libavutil/time.h"
#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavcodec/avfft.h"
#include "libswresample/swresample.h"

# include "libavfilter/avcodec.h"
# include "libavfilter/avfilter.h"
# include "libavfilter/buffersink.h"
# include "libavfilter/buffersrc.h"

#include <pthread.h>

#include <SDL.h>

#define LOG_TAG "RadioMMS"
#define LOG(MSG, ...) do { printf(MSG "\n", ## __VA_ARGS__ ); } while(0)

#define max(a, b) ((a > b)?a:b)
#define min(a, b) ((a > b)?b:a)

#define MAX_SIZE 4096

#define SDL_AUDIO_BUFFER_SIZE 2048

typedef struct SQueue {

	AVPacket packets[MAX_SIZE];
	int size;
	int tail, front;
	pthread_mutex_t lock;
	pthread_cond_t  cond;
} Queue;

typedef struct AudioParams {
    int sample_rate;
    int64_t channel_layout;
    int sample_fmt;
} AudioParams;

#define ERROR_MSG_SIZE 2048

typedef struct SFFPlayer {

	AVFormatContext *fmt_ctx;
	AVCodecContext  *audio_codec_ctx;
	AVCodec         *audio_codec;
	SwrContext      *swr_ctx;

	pthread_t       reading_thread;
	pthread_t       decoding_thread;

	Queue*          reading_queue;
	Queue*          decoding_queue;

	int             running;

	int audio_stream_index;

	AudioParams    *input_params;
	AudioParams    *output_params;

	struct SSDLPlayer *sdl;
	char error_msg[ERROR_MSG_SIZE];	

} FFPlayer;

#define TEST_FFERR(player, errnum) do { av_strerror(errnum, player->error_msg, ERROR_MSG_SIZE); LOG("ffmpeg error: %s", player->error_msg); } while(0)

#define SDL_QUEUE_MAX_SIZE (1024 * 1024)
#define SDL_BUFFER (100 * 1024)

const char program_name[] = "test";
const int program_birth_year = 2014;

typedef struct SSDLPlayer {
	SDL_AudioSpec spec;
	SDL_AudioSpec spec_needed;
	int running;
	
	uint8_t stream[SDL_QUEUE_MAX_SIZE];
	uint8_t buff[SDL_BUFFER];
	int stream_front;
	int stream_tail;
	int stream_size;

	pthread_mutex_t lock;
} SDLPlayer ;

static void log_callback(void* ptr, int level, const char* fmt, va_list vl) {
	vfprintf(stderr, fmt, vl);
	fprintf(stderr, "\n");
}

void show_help_default(const char *opt, const char *arg) {

}

SDLPlayer* SDLPlayer_init() {
	SDLPlayer *sdl = (SDLPlayer*) malloc(sizeof(SDLPlayer));
	sdl->running = 1;

	sdl->stream_tail = sdl->stream_front = sdl->stream_size = 0;

	SDL_Init(SDL_INIT_AUDIO);

	pthread_mutex_init(&sdl->lock, NULL);
	return sdl;
}

static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
	SDLPlayer *sdl = (SDLPlayer*) opaque;
	int lenBuffer;

	lenBuffer = SDLPlayer_remove_packet(sdl, sdl->buff, len);
	if (lenBuffer > 0) {
		SDL_MixAudio(stream, sdl->buff, lenBuffer, SDL_MIX_MAXVOLUME);
	}
}

int SDLPlayer_open(SDLPlayer *sdl, AudioParams *in, AudioParams *out) {

	SDL_AudioSpec want, have;

	memset(&want, 0, sizeof(SDL_AudioSpec));

	want.freq = in->sample_rate;
	want.format = AUDIO_S16SYS;
	want.channels = av_get_channel_layout_nb_channels(in->channel_layout);
	want.samples = SDL_AUDIO_BUFFER_SIZE;
	want.callback = sdl_audio_callback;  // you wrote this function elsewhere.
   	want.silence = 0;	
   	want.userdata = (void*) sdl;

	if (SDL_OpenAudio(&want, &have) < 0) {
    	LOG("Failed to open audio: %s\n", SDL_GetError());
	} else {

    	SDL_PauseAudio(0);  // start audio playing.
    	//SDL_Delay(5000);  // let the audio callback play some sound for 5 seconds.
    	//SDL_CloseAudio();

    	if (have.format != AUDIO_S16SYS) {
    		LOG("Format not supported by SDL");
    		return -1;
    	}
	}	

	out->channel_layout = av_get_default_channel_layout(have.channels);
	out->sample_rate = have.freq;
	out->sample_fmt = AV_SAMPLE_FMT_S16;

	return 0;
}

int SDLPlayer_put_packet(SDLPlayer *sdl, uint8_t *data, int len) {
	int i;
	int result = 0;

	pthread_mutex_lock(&sdl->lock);

	if ((SDL_QUEUE_MAX_SIZE - sdl->stream_size) > 0) {

		if ((SDL_QUEUE_MAX_SIZE - sdl->stream_size) < len) {
			len = (SDL_QUEUE_MAX_SIZE - sdl->stream_size);
		}

		int total = min(SDL_QUEUE_MAX_SIZE - sdl->stream_front, len);
		memcpy(sdl->stream + sdl->stream_front, data, total);
		if (len > total) {
			memcpy(sdl->stream, data + total, len - total);
		}
		sdl->stream_front = (sdl->stream_front + len) % SDL_QUEUE_MAX_SIZE;
		sdl->stream_size += len;
		result = 1;

	}

	pthread_mutex_unlock(&sdl->lock);

	return result;
}

int SDLPlayer_remove_packet(SDLPlayer *sdl, uint8_t *data, int len) {

	int result = 0;
	pthread_mutex_lock(&sdl->lock);

	if (sdl->stream_size > 0) {

		if (sdl->stream_size < len) {
			len = sdl->stream_size;
		}

		int total = min(SDL_QUEUE_MAX_SIZE - sdl->stream_tail, len);
		memcpy(data, sdl->stream + sdl->stream_tail, total);

		if (len > total) {
			memcpy(data + total, sdl->stream, len - total);
		}

		sdl->stream_tail = (sdl->stream_tail + len) % SDL_QUEUE_MAX_SIZE;
		sdl->stream_size -= len;

		result = len;
	}

	pthread_mutex_unlock(&sdl->lock);

	return result;
}

Queue* queue_init() {
	Queue *queue = (Queue*) malloc(sizeof(Queue));
	queue->size = 0;
	queue->front = queue->tail = 0;
	
	pthread_mutex_init(&queue->lock, NULL);
	pthread_cond_init(&queue->cond, NULL);

	return queue;
}

int queue_put(Queue *q, AVPacket packet) {

	int result = 1;

	pthread_mutex_lock(&q->lock);

	if (q->size < MAX_SIZE) {
		q->packets[q->front] = packet;
		q->front = (q->front + 1) % MAX_SIZE;
		q->size++;
	} else {
		result = 0;
	}

	if (q->size == 1) {
		pthread_cond_broadcast(&q->cond);
	}

	pthread_mutex_unlock(&q->lock);

	return result;
}

int queue_remove(Queue *q, AVPacket *item) {

	struct timespec ts;
	int result = 0;

	pthread_mutex_lock(&q->lock);

	if (q->size == 0) {
		clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 10;
        pthread_cond_timedwait(&q->cond, &q->lock, &ts);
	}

	if (q->size > 0) {
		*item = q->packets[q->tail];
		q->tail = (q->tail + 1) % MAX_SIZE;
		q->size--;
		result = 1;
	}

	pthread_mutex_unlock(&q->lock);

	return result;
}

FFPlayer* FFPlayer_init() {

	FFPlayer *player;

	player = (FFPlayer*) malloc(sizeof(FFPlayer));

	memset(player, 0, sizeof(FFPlayer));

	if (player == NULL) {

		LOG("Failed to alloc memory");

		return NULL;
	}

	av_log_set_callback(log_callback);

	player->fmt_ctx = avformat_alloc_context();

	av_register_all();
	avcodec_register_all();
	avformat_network_init();
	avfilter_register_all();

	player->reading_queue = queue_init();
	player->decoding_queue = queue_init();

	player->sdl = NULL;

	player->input_params  = (AudioParams*) malloc(sizeof(AudioParams));
	player->output_params = (AudioParams*) malloc(sizeof(AudioParams));

	return player;
}

int FFPlayer_open(FFPlayer *player, char *url) {

	int result, i, audio_stream_index = -1;
	player->audio_codec_ctx = NULL;
	AVCodecContext* codecCtx;

	assert(player != NULL);
	assert(url != NULL);

	result = avformat_open_input(&player->fmt_ctx, url, NULL, NULL);

	if (result != 0) {
		LOG("Failed to open %s", url);
		return result;
	}

	result = avformat_find_stream_info(player->fmt_ctx, NULL);

	if (result != 0) {
		LOG("Failed to find information about stream");
		return result;
	}

	for (i = 0; i < player->fmt_ctx->nb_streams; i++) {
		codecCtx = player->fmt_ctx->streams[i]->codec;
		if (codecCtx != NULL && codecCtx->codec_type == AVMEDIA_TYPE_AUDIO && codecCtx->sample_rate != 0 && codecCtx->channels != 0) {
			player->audio_codec_ctx = codecCtx;
			player->audio_stream_index = i;
			i = player->fmt_ctx->nb_streams + 1;
		}
	}

	if (player->audio_codec_ctx == NULL) {
		LOG("Audio codec wasn't found");
		return -1;
	}

	player->audio_codec = avcodec_find_decoder(codecCtx->codec_id);

	if (player->audio_codec == NULL) {
		LOG("Failed to find codec");
		return -1;
	}

	result = avcodec_open2(codecCtx, player->audio_codec, NULL);

	if (result < 0) {
		LOG("Failed to open codec");
		return -1;
	}

	player->input_params->channel_layout = player->audio_codec_ctx->channel_layout;
	player->input_params->sample_rate = player->audio_codec_ctx->sample_rate;
	player->input_params->sample_fmt = player->audio_codec_ctx->sample_fmt;

	return result;
}

void FFPlayer_setAudioOutputParams(FFPlayer *player, int sample_rate, int64_t channel_layout, int sample_fmt) {

	assert(player != NULL);

	player->output_params->sample_rate = sample_rate;
	player->output_params->channel_layout = channel_layout;
	player->output_params->sample_fmt = sample_fmt;
}

int FFPlayer_setResampler(FFPlayer *player) {

	assert(player != NULL);

	if (player->swr_ctx != NULL) {
		swr_free(&player->swr_ctx);
	}

	player->swr_ctx = swr_alloc();

    if (player->input_params->channel_layout == 0) {
    	player->input_params->channel_layout = AV_CH_LAYOUT_STEREO;
    }

	LOG("in_channel_layout = %lu", player->input_params->channel_layout);
    LOG("in_sample_rate = %d", player->input_params->sample_rate);
    LOG("in_sample_fmt = %d", player->input_params->sample_fmt);

	LOG("out_channel_layout = %lu", player->output_params->channel_layout);
    LOG("out_sample_rate = %d", player->output_params->sample_rate);
    LOG("out_sample_fmt = %d", player->output_params->sample_fmt);    

    av_opt_set_int(player->swr_ctx, "in_channel_layout", player->input_params->channel_layout, 0);
    av_opt_set_int(player->swr_ctx, "in_sample_rate", player->input_params->sample_rate, 0);
    av_opt_set_sample_fmt(player->swr_ctx, "in_sample_fmt", player->input_params->sample_fmt, 0);	

    av_opt_set_int(player->swr_ctx, "out_channel_layout", player->output_params->channel_layout, 0);
    av_opt_set_int(player->swr_ctx, "out_sample_rate", player->output_params->sample_rate, 0);
    av_opt_set_sample_fmt(player->swr_ctx, "out_sample_fmt", player->output_params->sample_fmt, 0);

    if (swr_init(player->swr_ctx) < 0) {
    	LOG("Failed to init resampler");
    	return -1;
    }

    return 0;
}

void* _reading_thread(void *arg) {

	FFPlayer *player = (FFPlayer*) arg;
	int status;
	AVPacket pkt, copy;

	av_init_packet(&pkt);

	LOG("Reading thread initialized");

	while (player->running) {

		if ((status = av_read_frame(player->fmt_ctx, &pkt)) != 0) {
			TEST_FFERR(player, status);
		} else {
			status = av_copy_packet(&copy, &pkt);

			if (status == 0) {
				if (copy.size > 0) {
					LOG("PUT 0x%x[%d]", copy.data, copy.size);
					queue_put(player->reading_queue, copy);
				}				
			}

		}
	}

	return NULL;
}

inline static int _convert(FFPlayer *player, AVFrame *dec_frame, AVPacket *pktReturn) {

	int64_t src_ch_layout = player->input_params->channel_layout, 
		dst_ch_layout = player->output_params->channel_layout;
	int src_rate = player->input_params->sample_rate, dst_rate = player->output_params->sample_rate;
	uint8_t **src_data = NULL, **dst_data = NULL;
	int src_nb_channels = 0, dst_nb_channels = 0;
	int src_linesize, dst_linesize;
	int src_nb_samples = dec_frame->nb_samples, dst_nb_samples, max_dst_nb_samples;
	enum AVSampleFormat src_sample_fmt = player->input_params->sample_fmt, 
		dst_sample_fmt = player->output_params->sample_fmt;
	int dst_bufsize;

	int ret = 0;

	src_nb_channels = av_get_channel_layout_nb_channels(player->input_params->channel_layout);

	src_data = dec_frame->extended_data;

	max_dst_nb_samples = dst_nb_samples =
		av_rescale_rnd(src_nb_samples, dst_rate, src_rate, AV_ROUND_UP);        					

	/* buffer is going to be directly written to a rawaudio file, no alignment */
	dst_nb_channels = av_get_channel_layout_nb_channels(dst_ch_layout);

	ret = av_samples_alloc_array_and_samples(&dst_data, &dst_linesize, dst_nb_channels,
	                         dst_nb_samples, dst_sample_fmt, 0);

	if (ret < 0) {
		LOG("Failed to alloc");
		goto error;
	}


	dst_nb_samples = av_rescale_rnd(swr_get_delay(player->swr_ctx, src_rate) +
	                src_nb_samples, dst_rate, src_rate, AV_ROUND_UP);

	if (dst_nb_samples > max_dst_nb_samples) {
		
		av_free(dst_data[0]);
		
		ret = av_samples_alloc(dst_data, &dst_linesize, dst_nb_channels,
	           dst_nb_samples, dst_sample_fmt, 1);

		if (ret < 0) {
			LOG("Failed to allocate");
			goto error;
		}

		max_dst_nb_samples = dst_nb_samples;
	}

	ret = swr_convert(player->swr_ctx, dst_data, dst_nb_samples, (const uint8_t **)src_data, src_nb_samples);

	if (ret < 0) {
		LOG("Failed to convert");
		goto error;
	}

	dst_bufsize = av_samples_get_buffer_size(&dst_linesize, dst_nb_channels,
                                                 ret, dst_sample_fmt, 1);	

	pktReturn->data = dst_data[0];
	pktReturn->size = dst_bufsize;

	LOG("Converted");

error:
	return ret;
}

void* _decoding_thread(void *arg) {

	FFPlayer *player = (FFPlayer*) arg;
	AVPacket pkt, pktConverted;
	AVFrame *dec_frame = av_frame_alloc();
	int got_frame, decoded, len;
	int64_t dec_channel_layout;

	LOG("Decoding thread initialized");

	while (player->running) {

		if (queue_remove(player->reading_queue, &pkt)) {

			LOG("REMOVE 0x%x [%d]", pkt.data, pkt.size);

			if (pkt.size > 0) {

				while (pkt.size > 0) {
					
					len = avcodec_decode_audio4(player->audio_codec_ctx, dec_frame, &got_frame, &pkt);

					if (len < 0) {
						TEST_FFERR(player, len);
					}

            		dec_channel_layout =
                		(dec_frame->channel_layout && av_frame_get_channels(dec_frame) == av_get_channel_layout_nb_channels(dec_frame->channel_layout)) ?
                		dec_frame->channel_layout : av_get_default_channel_layout(av_frame_get_channels(dec_frame));

                	if (got_frame) {

            			if (dec_frame->format        != player->input_params->sample_fmt ||
                			dec_channel_layout       != player->input_params->channel_layout ||
                			dec_frame->sample_rate   != player->input_params->sample_rate) {

            				player->input_params->sample_fmt = dec_frame->format;
            				player->input_params->channel_layout = dec_channel_layout;
            				player->input_params->sample_rate = dec_frame->sample_rate;

            				FFPlayer_setResampler(player);

            				LOG("Adjust resampler");
            			}

            			if (_convert(player, dec_frame, &pktConverted) < 0) {
            				LOG("Failed to convert");
            			}

            			LOG("pktConverted [0x%x][%d]", pktConverted.data, pktConverted.size);

            			queue_put(player->decoding_queue, pktConverted);

            		}

					if (len > 0 && got_frame) {
						decoded = FFMIN(len, pkt.size);
						pkt.data += decoded;
						pkt.size -= decoded;
					} else {
						pkt.size = 0;
					}
				}

			}

			av_free_packet(&pkt);
		}

	}

	return NULL;
}

int FFPlayer_initThreads(FFPlayer *player) {

	player->running = 1;

	pthread_create(&player->reading_thread, NULL, _reading_thread, (void*) player);	
	pthread_create(&player->decoding_thread, NULL, _decoding_thread, (void*) player);
}

void FFPlayer_consumeDecodedPackets(FFPlayer *player, SDLPlayer *sdl) {

	AVPacket pkt;

	while (player->running) {
		if (queue_remove(player->decoding_queue, &pkt)) {

			SDLPlayer_put_packet(sdl, pkt.data, pkt.size);
			LOG("SDL PUT [%x][%d]", pkt.data, pkt.size);

			free(pkt.data);
		}
	}

}

int main(int argc, char **argv) {

	int status;

	FFPlayer* player = FFPlayer_init();
	SDLPlayer *sdl   = SDLPlayer_init();

	if (FFPlayer_open(player, argv[1]) != 0) {
		LOG("Failed to init player");
		return EXIT_FAILURE;
	}

	if (SDLPlayer_open(sdl, player->input_params, player->output_params) != 0) {
		LOG("Failed to open SDL audio");
		return EXIT_FAILURE;
	}

	FFPlayer_setResampler(player);
	FFPlayer_initThreads(player);

	FFPlayer_consumeDecodedPackets(player, sdl);

	return EXIT_SUCCESS;
}