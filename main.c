#include <stdio.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <png.h>
#include <time.h>

static void getRGBfromFrame(AVFrame* frame, int* or, int* og, int* ob, int x, int y);
static void logging(const char *fmt, ...);
static int decode_packet(struct SwsContext* swsCtx, AVPacket *pPacket, AVCodecContext *pCodecContext, AVFrame *pFrame, AVFrame *pFrameConverted);
static void save_frame(unsigned char *buf, int wrap, int xsize, int ysize, char *filename);
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
	if (argc < 5) {
		logging("usage %s videoPath videoBeginTime outPath hwaccel_device", argv[0]);
		return 1;
	}
	int64_t beginTime = atoi(argv[2]);
	logging("begin time %ld", beginTime);

	enum AVHWDeviceType hwaccel_type;
	hwaccel_type = av_hwdevice_find_type_by_name(argv[4]);
	if (hwaccel_type == AV_HWDEVICE_TYPE_NONE) {
		logging("Device type %s is not supported.", argv[4]);
		logging("Available device types:");
		while((hwaccel_type = av_hwdevice_iterate_types(hwaccel_type)) != AV_HWDEVICE_TYPE_NONE)
			logging(" %s", av_hwdevice_get_type_name(hwaccel_type));
		return -1;
	}

	logging("Allocating context");
	AVFormatContext *pFormatContext = avformat_alloc_context();
	if (!pFormatContext) {
		logging("ERROR could not allocate memory for Format Context");
		return -1;
	}

	logging("Opening %s", argv[1]);
	if (avformat_open_input(&pFormatContext, argv[1], NULL, NULL) != 0) {
		logging("ERROR could not open the file");
		return -1;
	}
	logging("format %s, duration %lld us, bit_rate %lld", pFormatContext->iformat->name, pFormatContext->duration, pFormatContext->bit_rate);

	if (avformat_find_stream_info(pFormatContext, NULL) < 0) {
		logging("ERROR could not get the stream info");
		return -1;
	}


	const AVCodec *pCodec = NULL;
	AVCodecParameters *pCodecParameters = NULL;
	int video_stream_index = -1;
	int videoWidth, videoHeight;

	for (int i = 0; i < pFormatContext->nb_streams; i++) {
		AVCodecParameters *pLocalCodecParameters = NULL;
		pLocalCodecParameters = pFormatContext->streams[i]->codecpar;
		logging("AVStream->time_base before open coded %d/%d", pFormatContext->streams[i]->time_base.num, pFormatContext->streams[i]->time_base.den);
		logging("AVStream->r_frame_rate before open coded %d/%d", pFormatContext->streams[i]->r_frame_rate.num, pFormatContext->streams[i]->r_frame_rate.den);
		logging("AVStream->start_time %" PRId64, pFormatContext->streams[i]->start_time);
		logging("AVStream->duration %" PRId64, pFormatContext->streams[i]->duration);

		logging("finding the proper decoder (CODEC)");

		const AVCodec *pLocalCodec = avcodec_find_decoder(pLocalCodecParameters->codec_id);

		if (pLocalCodec == NULL) {
			logging("ERROR unsupported codec!");
			continue;
		}

		if (pLocalCodecParameters->codec_type == AVMEDIA_TYPE_VIDEO) {
			if (video_stream_index == -1) {
				video_stream_index = i;
				pCodec = pLocalCodec;
				pCodecParameters = pLocalCodecParameters;
				videoWidth = pLocalCodecParameters->width;
				videoHeight = pLocalCodecParameters->height;
				logging("SELECTED Video Codec: resolution %d x %d", pLocalCodecParameters->width, pLocalCodecParameters->height);
			} else {
				logging("Video Codec: resolution %d x %d", pLocalCodecParameters->width, pLocalCodecParameters->height);
			}
		} else if (pLocalCodecParameters->codec_type == AVMEDIA_TYPE_AUDIO) {
			logging("Audio Codec: sample rate %d", pLocalCodecParameters->sample_rate);
		}
		logging("\tCodec %s ID %d bit_rate %lld", pLocalCodec->name, pLocalCodec->id, pLocalCodecParameters->bit_rate);
	}

	if (video_stream_index == -1) {
		logging("File %s does not contain a video stream!", argv[1]);
		return -1;
	}

	AVCodecContext *pCodecContext = avcodec_alloc_context3(pCodec);
	if (!pCodecContext) {
		logging("failed to allocated memory for AVCodecContext");
		return -1;
	}

	if (avcodec_parameters_to_context(pCodecContext, pCodecParameters) < 0) {
		logging("failed to copy codec params to codec context");
		return -1;
	}

	if (avcodec_open2(pCodecContext, pCodec, NULL) < 0) {
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
	pFrameConverted->width = videoWidth;
	pFrameConverted->height = videoHeight;
	pFrameConverted->format = AV_PIX_FMT_RGB24;
	if (av_frame_get_buffer(pFrameConverted, 0)) {
		logging("failed to allocate frame buffer");
		return -1;
	}
	AVPacket *pPacket = av_packet_alloc();
	if (!pPacket) {
		logging("failed to allocate memory for AVPacket");
		return -1;
	}

	struct SwsContext* swsCtx = sws_getContext(
		videoWidth, videoHeight, AV_PIX_FMT_YUV420P,
		videoWidth, videoHeight, AV_PIX_FMT_RGB24,
		0, 0, 0, 0);

	int response = 0;

	while (av_read_frame(pFormatContext, pPacket) >= 0) {
		if (pPacket->stream_index == video_stream_index) {
			response = decode_packet(swsCtx, pPacket, pCodecContext, pFrame, pFrameConverted);
			if (response < 0)
				break;
		}
		av_packet_unref(pPacket);
	}

	logging("Done");
	return 0;
}

uint64_t lastReport = 0;
uint64_t lastFrame = 0;

static int decode_packet(struct SwsContext* swsCtx, AVPacket *pPacket, AVCodecContext *pCodecContext, AVFrame *pFrame, AVFrame *pFrameConverted) {
	int response = avcodec_send_packet(pCodecContext, pPacket);
	if (response < 0) {
		logging("Error while sending a packet to the decoder: %s", av_err2str(response));
		return response;
	}

	while (response >= 0) {
		response = avcodec_receive_frame(pCodecContext, pFrame);
		if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
			break;
		} else if (response < 0) {
			logging("Error while receiving a frame from the decoder: %s", av_err2str(response));
			return response;
		}

		if (response < 0) {
			continue;
		}

		uint64_t currTime = time(NULL);
		if (currTime - lastReport >= 1) {
			logging(
				"Frame %08d FPS %03d (type=%c, size=%d bytes, format=%d) pts %d key_frame %d",
				pCodecContext->frame_num,
				pCodecContext->frame_num-lastFrame,
				av_get_picture_type_char(pFrame->pict_type),
				pFrame->pkt_size,
				pFrame->format,
				pFrame->pts,
				pFrame->key_frame
			);
			lastFrame = pCodecContext->frame_num;
			lastReport = currTime;
		}

		// AV_PIX_FMT_YUV410P

		// const uint8_t* srcSlice[] = { pFrame->data[0], pFrame->data[1], pFrame->data[2] };
		// const int srcStride[] = { pFrame->linesize[0], pFrame->linesize[1], pFrame->linesize[2] };
		// int srcSliceY = 0;
		// int srcSliceH = pFrame->height;
		// uint8_t* dst[] = { pFrameConverted->data[0], pFrameConverted->data[1], pFrameConverted->data[2] };
		// int dstStride[] = { pFrameConverted->linesize[0], pFrameConverted->linesize[1], pFrameConverted->linesize[2] };

		// sws_scale(swsCtx, srcSlice, srcStride, srcSliceY, srcSliceH, dst, dstStride);

		// int r, g, b;
		// int cx = 0, cy = 0;
		// getRGBfromFrame(pFrame, &r, &g, &b, cx, cy);

		// logging("% 4d", r);
		// logging("% 4d", g);
		// logging("% 4d", b);

		// char frame_filename[1024];
		// snprintf(frame_filename, sizeof(frame_filename), "%ld.png", pCodecContext->frame_num);
		// bitmap_t btm = {
		// 	.pixels = (pixel_t*)pFrameConverted->data[0],
		// 	.width = pFrame->width,
		// 	.height = pFrame->height,
		// };
		// save_png_to_file(&btm, frame_filename);
		// save_frame(pFrame->data[0], pFrame->linesize[0], pFrame->width, pFrame->height, frame_filename);
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
	int yValue = yPlane[yIndex];
	int uValue = uPlane[uIndex];
	int vValue = vPlane[vIndex];
	int r = yValue + 1.402 * (vValue - 128);
	int g = yValue - 0.344136 * (uValue - 128) - 0.714136 * (vValue - 128);
	int b = yValue + 1.772 * (uValue - 128);
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
	*or = r;
	*og = g;
	*ob = b;
}

static void save_frame(unsigned char *buf, int wrap, int xsize, int ysize, char *filename) {
	FILE *f;
	int i;
	f = fopen(filename, "w");
	fprintf(f, "P6\n%d %d\n%d\n", xsize, ysize, 255);

	for (i = 0; i < ysize; i++)
		fwrite(buf + i * wrap, 3, xsize, f);
	fclose(f);
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