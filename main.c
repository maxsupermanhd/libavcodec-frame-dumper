#include <stdio.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <png.h>
#include <time.h>

int64_t videoBeginTime = 0;
uint64_t videoNumberOfFrames = 0;
const char* outPrefix;
uint64_t saveFromFrame;

static void getRGBfromFrame(AVFrame* frame, int* or, int* og, int* ob, int x, int y);
static void logging(const char *fmt, ...);
static int decodePacket(struct SwsContext* swsCtx, AVPacket *pPacket, AVCodecContext *pCodecContext, AVFrame *pFrame, AVFrame *pFrameConverted);
typedef struct {
	uint8_t red;
	uint8_t green;
	uint8_t blue;
} pixel_t;
typedef struct {
	pixel_t *pixels;
	size_t width;
	size_t height;
} bitmap_t;
static int save_png_to_file(bitmap_t *bitmap, const char *path);

int main(int argc, char **argv) {
	if (argc < 4) {
		logging("usage %s videoPath videoBeginTime outPath", argv[0]);
		return 1;
	}
	videoBeginTime = atoi(argv[2]);
	outPrefix = argv[3];
	saveFromFrame = atoi(argv[4]);

	AVFormatContext *formatCtx = avformat_alloc_context();
	if (!formatCtx) {
		logging("ERROR could not allocate memory for Format Context");
		return -1;
	}

	formatCtx->flags |= AVFMT_FLAG_GENPTS;

	logging("Opening %s", argv[1]);
	if (avformat_open_input(&formatCtx, argv[1], NULL, NULL) != 0) {
		logging("ERROR could not open the file");
		return -1;
	}
	logging("format %s, duration %lld us, bit_rate %lld", formatCtx->iformat->name, formatCtx->duration, formatCtx->bit_rate);

	if (avformat_find_stream_info(formatCtx, NULL) < 0) {
		logging("ERROR could not get the stream info");
		return -1;
	}

	const AVCodec *codec = NULL;
	AVCodecParameters *codecParams = NULL;
	int videoStreamIndex = -1;

	for (int i = 0; i < formatCtx->nb_streams; i++) {
		codecParams = formatCtx->streams[i]->codecpar;
		const AVCodec *foundCodec = avcodec_find_decoder(codecParams->codec_id);
		if (foundCodec == NULL) {
			logging("ERROR unsupported codec!");
			continue;
		}
		if (codecParams->codec_type == AVMEDIA_TYPE_VIDEO) {
			if (videoStreamIndex == -1) {
				videoStreamIndex = i;
				codec = foundCodec;
				logging("SELECTED Video Codec %d resolution %d x %d", i, codecParams->width, codecParams->height);
			} else {
				logging("Video Codec %d resolution %d x %d", i, codecParams->width, codecParams->height);
			}
		}
	}

	if (videoStreamIndex == -1) {
		logging("File %s does not contain a video stream!", argv[1]);
		return -1;
	}
	AVStream *video = formatCtx->streams[videoStreamIndex];

	int tS = (video->nb_frames/60)%60;
	int tM = (video->nb_frames/60/60)%60;
	int tH = (video->nb_frames/60/60/60);
	videoNumberOfFrames = video->nb_frames;
	logging("Video length: %d frames (%02dh%02dm%02ds)", video->nb_frames, tH, tM, tS);

	if (video->codecpar->format != AV_PIX_FMT_YUV420P) {
		logging("Wrong video pixel format: %d", video->codecpar->format);
		return -1;
	}

	AVCodecContext *codecCtx = avcodec_alloc_context3(codec);
	if (!codecCtx) {
		logging("failed to allocated memory for AVCodecContext");
		return -1;
	}

	codecCtx->thread_count = 16;
	codecCtx->thread_type = FF_THREAD_FRAME;

	if (avcodec_parameters_to_context(codecCtx, codecParams) < 0) {
		logging("failed to copy codec params to codec context");
		return -1;
	}

	if (avcodec_open2(codecCtx, codec, NULL) < 0) {
		logging("failed to open codec through avcodec_open2");
		return -1;
	}

	AVFrame *pFrame = av_frame_alloc();
	if (!pFrame) {
		logging("failed to allocate memory for AVFrame");
		return -1;
	}
	AVFrame *pFrameConverted = av_frame_alloc();
	if (!pFrameConverted) {
		logging("failed to allocate memory for AVFrame");
		return -1;
	}
	pFrameConverted->width = video->codecpar->width;
	pFrameConverted->height = video->codecpar->height;
	pFrameConverted->format = AV_PIX_FMT_RGB24;
	if (av_frame_get_buffer(pFrameConverted, 0)) {
		logging("failed to allocate frame buffer");
		return -1;
	}
	AVPacket *packet = av_packet_alloc();
	if (!packet) {
		logging("failed to allocate memory for AVPacket");
		return -1;
	}

	struct SwsContext* swsCtx = sws_getContext(
		video->codecpar->width, video->codecpar->height, AV_PIX_FMT_YUV420P,
		video->codecpar->width, video->codecpar->height, AV_PIX_FMT_RGB24,
		0, 0, 0, 0);

	int response = 0;

	while (av_read_frame(formatCtx, packet) >= 0) {
		if (packet->stream_index == videoStreamIndex) {
			response = decodePacket(swsCtx, packet, codecCtx, pFrame, pFrameConverted);
			if (response < 0)
				break;
		}
		av_packet_unref(packet);
	}

	logging("Done");
	return 0;
}

uint64_t lastReport = 0;
uint64_t lastFrame = 0;
uint8_t lastPixR = 0;
uint8_t lastPixG = 0;
uint8_t lastPixB = 0;

static int decodePacket(struct SwsContext* swsCtx, AVPacket *packet, AVCodecContext *codecCtx, AVFrame *frame, AVFrame *frameConverted) {
	int response = avcodec_send_packet(codecCtx, packet);
	if (response < 0) {
		logging("Error while sending a packet to the decoder: %s", av_err2str(response));
		return response;
	}

	while (response >= 0) {
		response = avcodec_receive_frame(codecCtx, frame);
		if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
			break;
		} else if (response < 0) {
			logging("Error while receiving a frame from the decoder: %s", av_err2str(response));
			return response;
		}

		uint64_t currTime = time(NULL);
		if (currTime - lastReport >= 1) {
			int tS = (codecCtx->frame_num/60)%60;
			int tM = (codecCtx->frame_num/60/60)%60;
			int tH = (codecCtx->frame_num/60/60/60);
			int fps = codecCtx->frame_num-lastFrame;
			int eS = (((videoNumberOfFrames-codecCtx->frame_num)/fps))%60;
			int eM = (((videoNumberOfFrames-codecCtx->frame_num)/fps)/60)%60;
			int eH = (((videoNumberOfFrames-codecCtx->frame_num)/fps)/60/60);
			logging("Frame % 8d (%02dh%02dm%02ds) FPS % 5d (ETA %02dh%02dm%02ds)", codecCtx->frame_num, tH, tM, tS, fps, eH, eM, eS);
			lastFrame = codecCtx->frame_num;
			lastReport = currTime;
		}

		int r, g, b;
		getRGBfromFrame(frame, &r, &g, &b, 1856, 799);

		if (codecCtx->frame_num > saveFromFrame) {
			if (r < 25 && g < 25 && b < 25 && lastPixR < 130 && lastPixG > 180 && lastPixB < 160) {
				const uint8_t* srcSlice[] = { frame->data[0], frame->data[1], frame->data[2] };
				const int srcStride[] = { frame->linesize[0], frame->linesize[1], frame->linesize[2] };
				uint8_t* dst[] = { frameConverted->data[0], frameConverted->data[1], frameConverted->data[2] };
				int dstStride[] = { frameConverted->linesize[0], frameConverted->linesize[1], frameConverted->linesize[2] };

				sws_scale(swsCtx, srcSlice, srcStride, 0, frame->height, dst, dstStride);

				char frame_filename[1024];
				int tS = (codecCtx->frame_num/60)%60;
				int tM = (codecCtx->frame_num/60/60)%60;
				int tH = (codecCtx->frame_num/60/60/60);
				logging("Saving frame % 8d (%02dh%02dm%02ds)", codecCtx->frame_num, tH, tM, tS);
				snprintf(frame_filename, sizeof(frame_filename), "%s/%ld.png", outPrefix, videoBeginTime + codecCtx->frame_num/60);
				bitmap_t btm = {
					.pixels = (pixel_t*)frameConverted->data[0],
					.width = frame->width,
					.height = frame->height,
				};
				save_png_to_file(&btm, frame_filename);
			}
		}

		lastPixR = r;
		lastPixG = g;
		lastPixB = b;
	}
	return 0;
}

static void getRGBfromFrame(AVFrame* frame, int* or, int* og, int* ob, int x, int y) {
	uint8_t *yPlane = frame->data[0];
	uint8_t *uPlane = frame->data[1];
	uint8_t *vPlane = frame->data[2];
	int yIndex = y * frame->linesize[0] + x;
	int uIndex = (y / 2) * frame->linesize[1] + (x / 2);
	int vIndex = (y / 2) * frame->linesize[2] + (x / 2);
	double yValue = yPlane[yIndex];
	double uValue = uPlane[uIndex];
	double vValue = vPlane[vIndex];
	yValue -= 16;
	uValue -= 128;
	vValue -= 128;
	double r = 1.164 * yValue                  + 1.596 * vValue;
	double g = 1.164 * yValue - 0.392 * uValue - 0.813 * vValue;
	double b = 1.164 * yValue + 2.017 * uValue;
	// int r = yValue + 1.402 * (vValue - 128);
	// int g = yValue - 0.344136 * (uValue - 128) - 0.714136 * (vValue - 128);
	// int b = yValue + 1.772 * (uValue - 128);
	if (r < 0) {
		r = 0;
	}
	if (r > 255) {
		r = 255;
	}
	if (g < 0) {
		g = 0;
	}
	if (g > 255) {
		g = 255;
	}
	if (b < 0) {
		b = 0;
	}
	if (b > 255) {
		b = 255;
	}
	*or = round(r);
	*og = round(g);
	*ob = round(b);
}

static pixel_t * pixel_at (bitmap_t * bitmap, int x, int y) {
	return bitmap->pixels + bitmap->width * y + x;
}

static int save_png_to_file(bitmap_t *bitmap, const char *path) {
	FILE * fp;
	png_structp png_ptr = NULL;
	png_infop info_ptr = NULL;
	size_t x, y;
	png_byte ** row_pointers = NULL;
	/* "status" contains the return value of this function. At first
	it is set to a value which means 'failure'. When the routine
	has finished its work, it is set to a value which means
	'success'. */
	int status = -1;
	/* The following number is set by trial and error only. I cannot
	see where it it is documented in the libpng manual.
	*/
	int pixel_size = 3;
	int depth = 8;
	
	fp = fopen (path, "wb");
	if (! fp) {
		goto fopen_failed;
	}

	png_ptr = png_create_write_struct (PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (png_ptr == NULL) {
		goto png_create_write_struct_failed;
	}
	
	info_ptr = png_create_info_struct (png_ptr);
	if (info_ptr == NULL) {
		goto png_create_info_struct_failed;
	}
	
	/* Set up error handling. */

	if (setjmp (png_jmpbuf (png_ptr))) {
		goto png_failure;
	}
	
	/* Set image attributes. */

	png_set_IHDR (png_ptr,
				info_ptr,
				bitmap->width,
				bitmap->height,
				depth,
				PNG_COLOR_TYPE_RGB,
				PNG_INTERLACE_NONE,
				PNG_COMPRESSION_TYPE_DEFAULT,
				PNG_FILTER_TYPE_DEFAULT);
	
	/* Initialize rows of PNG. */

	row_pointers = png_malloc (png_ptr, bitmap->height * sizeof (png_byte *));
	for (y = 0; y < bitmap->height; y++) {
		png_byte *row = 
			png_malloc (png_ptr, sizeof (uint8_t) * bitmap->width * pixel_size);
		row_pointers[y] = row;
		for (x = 0; x < bitmap->width; x++) {
			pixel_t * pixel = pixel_at (bitmap, x, y);
			*row++ = pixel->red;
			*row++ = pixel->green;
			*row++ = pixel->blue;
		}
	}
	
	/* Write the image data to "fp". */

	png_init_io (png_ptr, fp);
	png_set_rows (png_ptr, info_ptr, row_pointers);
	png_write_png (png_ptr, info_ptr, PNG_TRANSFORM_IDENTITY, NULL);

	/* The routine has successfully written the file, so we set
	"status" to a value which indicates success. */

	status = 0;
	
	for (y = 0; y < bitmap->height; y++) {
		png_free (png_ptr, row_pointers[y]);
	}
	png_free (png_ptr, row_pointers);
	
png_failure:
png_create_info_struct_failed:
	png_destroy_write_struct (&png_ptr, &info_ptr);
png_create_write_struct_failed:
	fclose (fp);
fopen_failed:
	return status;
}

static void logging(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fprintf(stderr, "\n");
}