#include "Odroid.h"

#define SDP_FILE_SIZE 10000

AVFormatContext* fmtCtx;
AVCodecContext* decoderCtx;
AVFrame* avframe;
AVPacket packet;
struct SwsContext* swsCtx;

AVFormatContext* fmtCtxAudioO;
AVFormatContext* fmtCtxAudioI;
AVCodecContext* audioEncoder;
AVCodecContext* audioDecoder;
AVPacket packetAudioO;
AVFrame* avframeAudioO;
AVPacket packetAudioI;
AVFrame* avframeAudioI;

int streamID;

Frame* frame;
uint8_t* dataFrame[8];
int linesize[8];

void initNetwork(void)
{
    avformat_network_init();
}

Frame* initVideoReception(char* url)
{
    AVCodecParameters* codecpar;
    AVDictionary* options = NULL;
    AVCodec* dec;

    fmtCtx = NULL;

    fmtCtx = avformat_alloc_context();
    if (fmtCtx == NULL)
    {
        perror("Could not alloc the context - Out of memory ?\n");
        exit(1);
    }

    // fmtCtx->flags |= AVFMT_FLAG_NONBLOCK;

    av_dict_set(&options, "protocol_whitelist", "file,udp,rtp", 0);

    printf("Opening Stream ...");
    if(avformat_open_input(&fmtCtx, url, NULL, &options))
    {
        perror("Could not open the input\n");
        exit(1);
    }
    printf(" Done !\n");

    printf("Waiting for stream information ...\n");
    if(avformat_find_stream_info(fmtCtx, NULL))
    {
        perror("Could not find the informations of the streams\n");
        exit(1);
    }

    streamID = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (streamID < 0)
    {
        perror("Could not find a video stream\n");
        exit(1);
    }

    codecpar = fmtCtx->streams[streamID]->codecpar;
    dec = avcodec_find_decoder(codecpar->codec_id);
    if (dec == NULL)
    {
        perror("Could not find the decoder\n");
        exit(1);
    }

    decoderCtx = avcodec_alloc_context3(dec);
    if (decoderCtx == NULL)
    {
        perror("Could not alloc the context - Out of memory ?\n");
        exit(1);
    }

    avcodec_parameters_to_context(decoderCtx, codecpar);
    if(avcodec_open2(decoderCtx, dec, NULL) < 0)
    {
        perror("Could not open the decoder\n");
        exit(1);
    }

    av_dump_format(fmtCtx, 0, url, 0);


    int x = codecpar->width;
    int y = codecpar->height;
    Frame* dframe = malloc(sizeof(Frame));
    avframe = av_frame_alloc();
    av_init_packet(&packet);
    av_image_alloc(dataFrame, linesize, x, y, AV_PIX_FMT_BGRA, 1);
    av_image_alloc(avframe->data, avframe->linesize, x, y, codecpar->format, 128);

    dframe->width = x;
    dframe->height = y;
    dframe->pitch = linesize[0];
    dframe->data = (char*)dataFrame[0];
    swsCtx = sws_getContext(x, y, codecpar->format,
                            x, y, AV_PIX_FMT_BGRA,
                            SWS_BILINEAR, NULL, NULL, NULL);
    frame = dframe;
    return dframe;
}

int initAudioStream(char* url, char* path_sdp)
{
    AVCodec* codecAudio = NULL;

    //Open Output audio stream
    fmtCtxAudioO = NULL;
    if(avformat_alloc_output_context2(&fmtCtxAudioO, NULL, NULL, url) < 0)
    {
        perror("Cannot alloc the output context - alloc_output_context2");
        exit(1);
    }

    if(avio_open(&fmtCtxAudioO->pb, url, AVIO_FLAG_WRITE) < 0)
    {
        fprintf(stderr, "Cannot open the IO context %s - avio_open", url);
        exit(1);
    }

    //Finding the codec for the output, and setting his informations
    codecAudio = avcodec_find_encoder(AV_CODEC_ID_MP2);
    if (!codecAudio)
    {
        perror("Could not find the encoder");
        exit(1);
    }
    audioEncoder = avcodec_alloc_context3(codecAudio);
    if (!audioEncoder)
    {
        perror("Could not allocate the decoder");
        exit(1);
    }

    audioEncoder->bit_rate = AUDIO_BITRATE;
    audioEncoder->sample_fmt = AV_SAMPLE_FMT_S16;
    audioEncoder->sample_rate = AUDIO_SAMPLE_RATE;
    audioEncoder->channel_layout = AV_CH_LAYOUT_STEREO;
    audioEncoder->channels = AUDIO_CHANNELS;

    if(avcodec_open2(audioEncoder, codecAudio, NULL) < 0)
    {
        perror("Error opening the encoder");
        exit(1);
    }

    // Open a new stream, and set the codec information
    AVStream* newStream = avformat_new_stream(fmtCtxAudioO, NULL);
    newStream->id = fmtCtxAudioO->nb_streams-1;

    avcodec_parameters_from_context(fmtCtxAudioO->streams[0]->codecpar, audioEncoder);

    //Start the streams
    if(avformat_write_header(fmtCtxAudioO, NULL) < 0)
    {
        perror("Error starting the stream");
        exit(1);
    }

    //Initialize the packet, and alloc the frame
    av_init_packet(&packetAudioO);
    avframeAudioO = av_frame_alloc();

    //Save SDP file
    char sdp_file[SDP_FILE_SIZE];
    av_sdp_create(&fmtCtxAudioO, 1, sdp_file, SDP_FILE_SIZE);
    FILE* fd = fopen(path_sdp, "w");
    if (!fd)
    {
        perror("Error creating the file ");
        exit(1);
    }
    fwrite(sdp_file, strlen(sdp_file), 1, fd);
    fclose(fd);

    //Return file size
    return strlen(sdp_file);
}

void getNewFrame(FILE* fd)
{
    int ret;
    char buf[200];
    clock_t step0, step1, step2, step3, step4, step5;

    while(1)
    {
        step0 = clock();
        ret = avcodec_receive_frame(decoderCtx, avframe);
        if (ret < 0)
        {
            if (ret != AVERROR(EAGAIN))
            {
                perror("Error decoding the packet - avcodec_receive_frame ");
                exit(1);
            }

            step1 = clock();

            if (av_read_frame(fmtCtx, &packet) < 0)
            {
                perror("Error receiving the next packet - av_read_frame ");
                exit(1);
            }

            step2 = clock();

            if (avcodec_send_packet(decoderCtx, &packet) < 0)
            {
                perror("Error sending the packet to the decoder - avcodec_send_packet ");
                exit(1);
            }
            av_packet_unref(&packet);

            step3 = clock();

        } else {
            break;
        }
    }

    step4 = clock();

    sws_scale(swsCtx, (const uint8_t * const*)avframe->data, avframe->linesize,
                0, frame->height, dataFrame, linesize);

    step5 = clock();

    if (fd)
    {
        sprintf(buf, "%ld\t%ld\t%ld\t%ld\n",   (step2 - step1)*1000000/CLOCKS_PER_SEC,
                                            (step4 - step3)*1000000/CLOCKS_PER_SEC,
                                            (step3 - step2)*1000000/CLOCKS_PER_SEC,
                                            (step5 - step4)*1000000/CLOCKS_PER_SEC);
        fwrite(buf, strlen(buf), 1, fd);
    }
}

void endVideoReception(void)
{
    avcodec_send_packet(decoderCtx, NULL);
    avformat_close_input(&fmtCtx);

    sws_freeContext(swsCtx);
    // av_freep(&(avframe->data[0]));
    av_freep(&(dataFrame[0]));
    free(frame);
    av_frame_free(&avframe);
    avformat_free_context(fmtCtx);
    avcodec_free_context(&decoderCtx);

}