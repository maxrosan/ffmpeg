#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <math.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <pthread.h>

#include <SDL.h>

#define LOG_TAG "RadioMMS"
#define LOG(MSG, ...) do { printf(MSG "\n", ## __VA_ARGS__ ); } while(0)

#define max(a, b) ((a > b)?a:b)
#define min(a, b) ((a > b)?b:a)

#define MAX_SIZE 1024

typedef struct SQueue {

	AVPacket packets[MAX_SIZE];
	int size;
	int tail, front;
	pthread_mutex_t lock;
	pthread_cond_t  cond;
} Queue;

typedef struct SFFPlayer {
	AVFormatContext *fmt_ctx;
	AVCodecContext  *codec_ctx;
	AVFrame         *decoded_frame;
	AVPacket        avpkt;
	
	pthread_t       read_thread;
	pthread_t       decode_thread;

	Queue*          read_queue;
	Queue*          decode_queue;

	int             running;
} FFPlayer;

#define SDL_QUEUE_MAX_SIZE (1024 * 100)
#define SDL_BUFFER (100 * 1024)

typedef struct SSDLPlayer {
	SDL_AudioSpec spec;
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

SDLPlayer* SDLPlayer_init() {
	SDLPlayer *sdl = (SDLPlayer*) malloc(sizeof(SDLPlayer));
	sdl->running = 1;

	sdl->stream_tail = sdl->stream_front = sdl->stream_size = 0;

	SDL_Init(SDL_INIT_AUDIO);

	pthread_mutex_init(&sdl->lock, NULL);
	return sdl;
}

int SDLPlayer_put_packet(SDLPlayer *sdl, uint8_t *data, int len) {
	int i;
	int result = 0;

	pthread_mutex_lock(&sdl->lock);

	LOG("put");

	LOG("arg = %d", len);
	LOG("stream front = %d", sdl->stream_front);
	LOG("stream tail = %d", sdl->stream_tail);
	LOG("stream size = %d", sdl->stream_size);	
	LOG("------");

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

		LOG("stream front = %d", sdl->stream_front);
		LOG("stream tail = %d", sdl->stream_tail);
		LOG("stream size = %d", sdl->stream_size);
	}

	pthread_mutex_unlock(&sdl->lock);

	return result;
}

int SDLPlayer_remove_packet(SDLPlayer *sdl, uint8_t *data, int len) {

	int result = 0;
	pthread_mutex_lock(&sdl->lock);

	LOG("remove");

	LOG("arg = %d", len);
	LOG("stream front = %d", sdl->stream_front);
	LOG("stream tail = %d", sdl->stream_tail);
	LOG("stream size = %d", sdl->stream_size);
	LOG("------");

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

		LOG("stream front = %d", sdl->stream_front);
		LOG("stream tail = %d", sdl->stream_tail);
		LOG("stream size = %d", sdl->stream_size);

		result = len;
	}

	pthread_mutex_unlock(&sdl->lock);

	return result;
}

void SDLPlayer_fill_audio(void *udata, Uint8 *stream, int len) {

	SDLPlayer *sdl = (SDLPlayer*) udata;

	len = min(SDL_BUFFER, len);
	int len_data = SDLPlayer_remove_packet(sdl, sdl->buff, len);

	if (len_data > 0) {
		SDL_MixAudio(stream, sdl->buff, len, SDL_MIX_MAXVOLUME);
	}

}

void SDLPlayer_set_parameters(SDLPlayer *sdl, FFPlayer *ff) {
    /* Set the audio format */
    sdl->spec.freq = ff->codec_ctx->sample_rate;
    sdl->spec.format = AUDIO_S16SYS;
    sdl->spec.channels = ff->codec_ctx->channels;    /* 1 = mono, 2 = stereo */
    sdl->spec.samples = ff->codec_ctx->frame_size;  /* Good low-latency value for callback */
    sdl->spec.callback = SDLPlayer_fill_audio;
    sdl->spec.userdata = (void*) sdl;	
    sdl->spec.silence = 0;

	LOG("freq = %d", sdl->spec.freq);
	LOG("channels = %d", sdl->spec.channels);
	LOG("samples = %d", sdl->spec.samples);
}

int SDLPlayer_open(SDLPlayer *sdl) {
	return SDL_OpenAudio(&sdl->spec, NULL);
}

void SDLPlayer_run(SDLPlayer *sdl, FFPlayer *ff) {
	
	AVPacket pkt;

	SDL_PauseAudio(0);

	while (sdl->running) {
		if (queue_remove(ff->decode_queue, &pkt)) {
			SDLPlayer_put_packet(sdl, pkt.data, pkt.size);
			av_free_packet(&pkt);
		}	
	}
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

	player = malloc(sizeof(FFPlayer));

	if (player == NULL) {

		LOG("Failed to alloc memory");

		return NULL;
	}

	av_log_set_callback(log_callback);

	player->fmt_ctx = avformat_alloc_context();

	av_register_all();
	avcodec_register_all();
	avformat_network_init();

	player->read_queue = queue_init();
	player->decode_queue = queue_init();

	return player;
}

int FFPlayer_openURLAndFindCodec(FFPlayer *player, char *url) {

	int status, audio_stream_index;
	AVCodec *codec;

	assert(player != NULL && url != NULL);

	status = avformat_open_input(&player->fmt_ctx, url, NULL, NULL);

	if (status != 0) {
		LOG("Cannot start streaming");
		return -1;
	}

	status = avformat_find_stream_info(player->fmt_ctx, NULL);

	if (status < 0) {
		LOG("Failed to find stream");
		return -1;
	}

	audio_stream_index = av_find_best_stream(player->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);

	if (audio_stream_index < 0) {
		LOG("Failed to find best stream");
		return -1;
	}

	player->codec_ctx = player->fmt_ctx->streams[audio_stream_index]->codec;

	status = avcodec_open2(player->codec_ctx, codec, NULL);
	if (status < 0) {
		LOG("Failed to open codec");
		return status;
	}

	return 0;
}

void *_FFPlayer_readRun(void *arg);
void *_FFPlayer_decoderRun(void *arg);

int FFPlayer_prepareDecoding(FFPlayer *player) {

	assert(player != NULL);

	player->decoded_frame = avcodec_alloc_frame();

	if (player->decoded_frame == NULL) {
		LOG("init: failed to decode packet");
		return -1;
	}

	av_init_packet(&player->avpkt);

	player->running = 1;
	pthread_create(&player->read_thread, NULL, _FFPlayer_readRun, (void*) player);
	pthread_create(&player->decode_thread, NULL, _FFPlayer_decoderRun, (void*) player);

	return 0;
}

int FFPlayer_readPacketAndPutItIntoQueue(FFPlayer *player) {

	AVPacket copy;
	int status;

	if (av_read_frame(player->fmt_ctx, &player->avpkt) != 0) {
		LOG("Failed to read packet");
		return -1;
	}	

	status = av_copy_packet(&copy, &player->avpkt);

	if (status < 0) {
		LOG("Failed to copy packet");
	} else {
		LOG("Putting queue [%d]", copy.size);
		if (copy.size > 0) {
			queue_put(player->read_queue, copy);
		}
	}

	return status;
}

void *_FFPlayer_readRun(void *arg) {

	int status, frame_size = 0;
	FFPlayer *player;
	uint8_t* frame = NULL;

	player = (FFPlayer*) arg;

	while (player->running) {
		//status = FFPlayer_readPacket(player, &frame, &frame_size);
		status = FFPlayer_readPacketAndPutItIntoQueue(player);
	}
}

void *_FFPlayer_decoderRun(void *arg) {

	FFPlayer *player;
	AVPacket pkt;
	AVPacket tmp_packet;
	int len, got_frame;

	player = (FFPlayer*) arg;

	while (player->running) {

		if (queue_remove(player->read_queue, &pkt)) {
			LOG("packet.size = %d", pkt.size);

			if (pkt.size > 0) {

				avcodec_get_frame_defaults(player->decoded_frame);

				len = avcodec_decode_audio4(player->codec_ctx, player->decoded_frame, &got_frame, &pkt);

				if (got_frame) {
					int data_size = av_samples_get_buffer_size(NULL, player->codec_ctx->channels,
						player->decoded_frame->nb_samples, player->codec_ctx->sample_fmt, 1);				

					tmp_packet.data = malloc(data_size);
					tmp_packet.size = data_size;

					memcpy(tmp_packet.data, player->decoded_frame->data[0], data_size);

					queue_put(player->decode_queue, tmp_packet);

					av_free_packet(&pkt);
				}

			}
		}

	}
}

/*int FFPlayer_readPacket(FFPlayer *player, uint8_t **frame_decoded, int *frame_size) {

	int got_frame, len;

	*frame_size = 0;

	if (player->decoded_frame == NULL) {
		LOG("Failed to alloc");
		return -1;
	}

	if (av_read_frame(player->fmt_ctx, &player->avpkt) != 0) {
		LOG("Failed to read packet");
		return -1;
	}


	avcodec_get_frame_defaults(player->decoded_frame);

	len = avcodec_decode_audio4(player->codec_ctx, player->decoded_frame, &got_frame, &player->avpkt);

	if (len < 0) {
		LOG("Failed to decode packet");
		return -1;
	}


	if (got_frame) {

		int data_size = av_samples_get_buffer_size(NULL, player->codec_ctx->channels,
				player->decoded_frame->nb_samples, player->codec_ctx->sample_fmt, 1);

		if (*frame_size < data_size && *frame_decoded != NULL) {
			free(*frame_decoded);
			*frame_decoded = NULL;
		}

		if (*frame_decoded == NULL) {
			*frame_decoded = malloc(data_size);
		}

		if (*frame_decoded != NULL) {
			*frame_size = data_size;
			memcpy(*frame_decoded, player->decoded_frame->data[0], data_size);
		} else {
			LOG("Failed to alloc memory for decoding frame");
			got_frame = 0;
		}
	}

	return got_frame;
}*/

void FFPlayer_stop(FFPlayer *player) {

	LOG("stooping");

	player->running = 0;
	
	pthread_join(player->read_thread, NULL);
	pthread_join(player->decode_thread, NULL);
}

int main(int argc, char **argv) {

	int status;

	FFPlayer* player = FFPlayer_init();
	SDLPlayer *sdl   = SDLPlayer_init();

	status = FFPlayer_openURLAndFindCodec(player, argv[0]);

	if (status < 0) {
		LOG("Failed to open streaming");
		return EXIT_FAILURE;
	}

 	status = FFPlayer_prepareDecoding(player);

	if (status < 0) {
		LOG("Failed to prepare decoding");
		return EXIT_FAILURE;
	}

	SDLPlayer_set_parameters(sdl, player);

	if (SDLPlayer_open(sdl) < 0) {
		LOG("Failed to open audio");
	} else {
		SDLPlayer_run(sdl, player);
	}

	FFPlayer_stop(player);

	return EXIT_SUCCESS;
}