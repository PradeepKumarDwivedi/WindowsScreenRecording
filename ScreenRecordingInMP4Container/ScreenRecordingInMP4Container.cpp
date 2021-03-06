// ScreenRecordingInMP4Container.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <stdio.h>
#include <stdint.h>
#include"DuplicationManager.h"


extern "C"
{
	#include <libavformat/avformat.h>
	#include <libavutil/channel_layout.h>
	#include <libavutil/libm.h>
	#include <libswscale/swscale.h>
	#include <libavutil/imgutils.h>
    #include <libavutil/opt.h>
    #include <libavutil/internal.h>
	#include<libavformat/movenc.h>
}




const char *file_name;
FILE *log_file;
RECT desktop;
DUPLICATIONMANAGER DuplMgr;
DUPL_RETURN Ret;
AVCodec *video_codec = NULL;

// a wrapper around a single output AVStream
typedef struct OutputStream {
	AVStream *st;
	AVCodecContext *enc;
	/* pts of the next frame that will be generated */
	int64_t next_pts;
	int samples_count;
	AVFrame *frame;
	AVFrame *tmp_frame;
	struct SwsContext *sws_ctx;
} OutputStream;

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt)
{
	AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

}

static int write_frame(AVFormatContext *fmt_ctx, const AVRational *time_base, AVStream *st, AVPacket *pkt)
{
	/* rescale output packet timestamp values from codec to stream timebase */
	av_packet_rescale_ts(pkt, *time_base, st->time_base);
	pkt->stream_index = st->index;
	int num = 0, den = 0;
	/* Write the compressed frame to the media file. */
	log_packet(fmt_ctx, pkt);
	
	int ret = av_interleaved_write_frame(fmt_ctx, pkt);
	return ret;
}

/* Add an output stream. */
static void add_stream(OutputStream *ost, AVFormatContext *oc,
	AVCodec **codec,
	enum AVCodecID codec_id)
{
	AVCodecContext *c;
	int i;

	/* find the encoder */
	*codec = avcodec_find_encoder(codec_id);
	if (!(*codec)) {
		fprintf(log_file, "Could not find encoder for '%s'\n",
			avcodec_get_name(codec_id));
		exit(1);
	}

	ost->st = avformat_new_stream(oc, NULL);
	if (!ost->st) {
		fprintf(log_file, "Could not allocate stream\n");
		exit(1);
	}
	ost->st->id = oc->nb_streams - 1;
	c = avcodec_alloc_context3(*codec);
	if (!c) {
		fprintf(log_file, "Could not alloc an encoding context\n");
		exit(1);
	}
	ost->enc = c;

	c->codec_id = codec_id;
	/* resolution must be a multiple of two */
	// Get the client area for size calculation
	RECT rcClient;
	GetClientRect(NULL, &rcClient);

	int width = 0, height = 0;  //initial values


								// Get a handle to the desktop window
	const HWND hDesktop = GetDesktopWindow();

	// Get the size of screen to the variable desktop
	GetWindowRect(hDesktop, &desktop);

	// The top left corner will have coordinates (0,0)
	// and the bottom right corner will have coordinates
	// (horizontal, vertical)
	width = desktop.right;
	height = desktop.bottom;

	/* Resolution must be a multiple of two. */
	c->width = (width + 7) / 8 * 8;
	c->height = height;
	/* timebase: This is the fundamental unit of time (in seconds) in terms
	* of which frame timestamps are represented. For fixed-fps content,
	* timebase should be 1/framerate and timestamp increments should be
	* identical to 1. */
	//ost->st->time_base = AVRational { 1, STREAM_FRAME_RATE };
	ost->st->time_base.num = 1;
	ost->st->time_base.den = 15;
	c->time_base = ost->st->time_base;

	c->gop_size = 25; /* emit one intra frame every twelve frames at most */
	c->pix_fmt = AV_PIX_FMT_YUV420P;


	av_opt_set(c->priv_data, "crf", "28", 0);
	av_opt_set(c->priv_data, "tune", "zerolatency", 0);
    av_opt_set(c->priv_data, "preset", "ultrafast", 0);
	av_opt_set(c->priv_data, "vprofile", "baseline", 0);

	/* Some formats want stream headers to be separate. */
	if (oc->oformat->flags & AVFMT_GLOBALHEADER)
		c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}

/**************************************************************/
/* video output */

static AVFrame *alloc_picture(enum AVPixelFormat pix_fmt, int width, int height)
{
	AVFrame *picture;
	int ret;

	picture = av_frame_alloc();
	if (!picture)
		return NULL;

	picture->format = pix_fmt;
	picture->width = width;
	picture->height = height;

	/* allocate the buffers for the frame data */
	ret = av_frame_get_buffer(picture, 8);
	if (ret < 0) {
		fprintf(stderr, "Could not allocate frame data.\n");
		exit(1);
	}

	return picture;
}

static void open_video(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg)
{
	int ret;
	AVCodecContext *c = ost->enc;
	AVDictionary *opt = NULL;

	av_dict_copy(&opt, opt_arg, 0);

	/* open the codec */
	ret = avcodec_open2(c, codec, &opt);
	av_dict_free(&opt);
	if (ret < 0) {
		fprintf(log_file, "Could not open video codec: %s\n");
		exit(1);
	}

	
	/* allocate and init a re-usable frame */
	ost->frame = alloc_picture(c->pix_fmt, c->width, c->height);
	if (!ost->frame) {
		fprintf(log_file, "Could not allocate video frame\n");
		exit(1);
	}

	/* If the output format is not YUV420P, then a temporary YUV420P
	* picture is needed too. It is then converted to the required
	* output format. */
	ost->tmp_frame = NULL;
	if (c->pix_fmt != AV_PIX_FMT_YUV420P) {
		ost->tmp_frame = alloc_picture(AV_PIX_FMT_YUV420P, c->width, c->height);
		if (!ost->tmp_frame) {
			fprintf(log_file, "Could not allocate temporary picture\n");
			exit(1);
		}
	}

	/* copy the stream parameters to the muxer */
	ret = avcodec_parameters_from_context(ost->st->codecpar, c);
	if (ret < 0) {
		fprintf(log_file, "Could not copy the stream parameters\n");
		exit(1);
	}

}
static AVFrame* get_video_frame(OutputStream *ost)
{

	AVCodecContext *c = ost->enc;

	DUPL_RETURN Ret;
	BYTE* pBuf = new BYTE[10000000];
	AVRational tmp;
	tmp.num = 1;
	tmp.den = 1;


	/* when we pass a frame to the encoder, it may keep a reference to it
	* internally; make sure we do not overwrite it here */
	if (av_frame_make_writable(ost->frame) < 0)
		exit(1);

	if (c->pix_fmt == AV_PIX_FMT_YUV420P) {
		/* as we only generate a YUV420P picture, we must convert it
		* to the codec pixel format if needed */
		if (!ost->sws_ctx) {
			ost->sws_ctx = sws_getContext(c->width, c->height,
				AV_PIX_FMT_RGB32,
				c->width, c->height,
				AV_PIX_FMT_YUV420P/*c->pix_fmt*/,
				SWS_BICUBIC, NULL, NULL, NULL);
			if (!ost->sws_ctx) {
				fprintf(stderr,
					"Could not initialize the conversion context\n");
				exit(1);
			}
		}

		// Get new frame from desktop duplication
		Ret = DuplMgr.GetFrame(pBuf);
		if (Ret != DUPL_RETURN_SUCCESS)
		{
			fprintf_s(log_file, "Could not get the frame.");
		}

		uint8_t * inData[1] = { (uint8_t *)pBuf }; // RGB have one plane
		int inLinesize[1] = { av_image_get_linesize(AV_PIX_FMT_RGB32, c->width, 0) }; // RGB stride


		int scale_rs = sws_scale(ost->sws_ctx,
			inData, inLinesize,
			0, c->height, ost->frame->data, ost->frame->linesize); //ost->m_picIn.img.plane,ost->m_picIn.img.i_stride

	}

	delete pBuf;
	//ost->m_picIn.i_pts = ost->next_pts++;
	ost->frame->pts = ost->next_pts++;
	//ost->m_picIn
	return ost->frame;
}

/*
* encode one video frame and send it to the muxer
* return 1 when encoding is finished, 0 otherwise
*/
static int write_video_frame(AVFormatContext *oc, OutputStream *ost)
{
	int ret;
	AVCodecContext *c;
	AVFrame *frame;
	//x264_picture_t frame;
	int got_packet = 0;
	AVPacket* pkt;

	pkt = av_packet_alloc();
	if (!pkt)
		exit(1);

	c = ost->enc;

	frame = get_video_frame(ost);
	
	av_init_packet(pkt);
	if (frame)
		printf("Send frame video %d\n", frame->pts);

	ret = avcodec_send_frame(c, frame);
	if (ret < 0) {
		fprintf(log_file, "Error sending a video frame for encoding\n");
		exit(1);
	}
	while (ret >= 0)
	{
		ret = avcodec_receive_packet(c, pkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
		{

			/*fprintf(log_file, "Audio encoding Event is set?\n");
			SetEvent(pMySink->m_SoundEncoded);*/

			return 0;
		}
		else if (ret < 0) {
			fprintf(log_file, "Error during encoding\n");
			exit(1);
		}
		if (pkt->size)
		{
			ret = write_frame(oc, &c->time_base, ost->st, pkt);
			if (ret < 0) {
				fprintf(log_file, "Error while writing video frame: %s\n");
				exit(1);
			}
		}

	}
	return (frame || pkt->size) ? 0 : 1;

}


static void close_stream(AVFormatContext *oc, OutputStream *ost)
{
	avcodec_free_context(&ost->enc);
	av_frame_free(&ost->frame);
	av_frame_free(&ost->tmp_frame);
	sws_freeContext(ost->sws_ctx);
}

int main(int argc, char **argv)
{
	OutputStream video_st = { 0 }; 
	const char *codec_name_video,*filename;
	AVOutputFormat *fmt;
	AVFormatContext *oc;
	int ret;
	int have_video = 0;
	int encode_video = 0;
	AVDictionary *opt = NULL;
	int i;
	FF_DISABLE_DEPRECATION_WARNINGS
		av_register_all();  //only for new Dlls
	FF_ENABLE_DEPRECATION_WARNINGS

		avformat_network_init();

	filename = "Content\\ScreenRecorded.mp4";

    /* allocate the output media context */
	ret = avformat_alloc_output_context2(&oc, NULL, NULL /*"ismv"*/, filename);
	if (!oc) {
		fprintf(log_file, "Could not deduce output format from file extension: using MPEG.\n");
		avformat_alloc_output_context2(&oc, NULL, "mpeg", filename);
	}
	if (!oc)
		return 1;

	fmt = oc->oformat;


	UINT Output = 0;

	// Make duplication manager
	Ret = DuplMgr.InitDupl(log_file, Output);
	if (Ret != DUPL_RETURN_SUCCESS)
	{
		fprintf_s(log_file, "Duplication Manager couldn't be initialized.");
		return 0;
	}

	

	fmt->video_codec = AV_CODEC_ID_H264;

	if (fmt->video_codec != AV_CODEC_ID_NONE) {
		add_stream(&video_st, oc, &video_codec, fmt->video_codec);
		have_video = 1;
		encode_video = 1;
	}
	
	if (have_video)
		open_video(oc, video_codec, &video_st, opt);

	av_dump_format(oc, 0, filename, 1);

	// Write MOOV atom at the begining of the MP4 file
	MOVMuxContext *mov = NULL;

	mov = (MOVMuxContext *)oc->priv_data;
	mov->flags |= FF_MOV_FLAG_FASTSTART;
	mov->flags |= FF_MOV_FLAG_EMPTY_MOOV;
	
	/* open the output file, if needed */
	if (!(fmt->flags & AVFMT_NOFILE)) {
		ret = avio_open(&oc->pb, filename, AVIO_FLAG_WRITE);
		if (ret < 0) {
			fprintf(log_file, "Could not open '%s': %s\n");
			/*fprintf(stderr, "Could not open '%s': %s\n", filename,
			av_err2str(ret));*/
			return 1;
		}
	}

	/* Write the stream header, if any. */
	ret = avformat_write_header(oc, &opt);
	if (ret < 0) {
		fprintf(log_file, "Error occurred when opening output file: %s\n");
		return 1;
	}
	int count = 0;
	while (count < 100) //12000
	{
		encode_video = write_video_frame(oc, &video_st);
		count++;
	}

	/* Write the trailer, if any. The trailer must be written before you
	* close the CodecContexts open when you wrote the header; otherwise
	* av_write_trailer() may try to use memory that was freed on
	* av_codec_close(). */
	av_write_trailer(oc);

	/* Close each codec. */
	if (have_video)
		close_stream(oc, &video_st);

	if (!(fmt->flags & AVFMT_NOFILE))
		/* Close the output file. */
		avio_closep(&oc->pb);

	/* free the stream */
	avformat_free_context(oc);
	return 0;
}




