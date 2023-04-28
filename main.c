#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <gtk/gtk.h>

// Definitions
AVFormatContext *formatCntxt = NULL;
AVCodecContext *codecCtxt1 = NULL;
AVCodecContext *codecCtxt2 = NULL;
struct SwsContext *sws_ctx = NULL;
AVPacket *packet = NULL;
AVFrame *frame = NULL;
AVFrame *frameRGB = NULL;
AVCodec *codec = NULL;
int videoStreamIndex = -1;

static GtkWidget *window = NULL;
static GtkWidget *image = NULL;

int bufferSize, frameNumber, imageWidth, imageHeight; // size of circular buffer
int *imageBuffer;                                     // circular buffer
const char *filename;                                 // name of video
int count = 0;                                        // Number of items in the buffer
int writeIndex = 0;                                   // Index for inserting new items
int readIndex = 0;                                    // Index for retrieving items
int flag = 0;                                         // Bool value to check if there are still frames being decoded
int ratePerSec;                                       // Get video rate per second
int displayPerSec = 1;                                // keep track of what image is being displayed
double sleepTime;                                     // calculate time for usleeo

pthread_mutex_t lock; // mutex lock for write/read to buffer
pthread_cond_t full, empty;

void *producer()
{
    int num = 1, iteration = 1;
    // read frame and stores it into AVPacket struct
    while (av_read_frame(formatCntxt, packet) >= 0)
    {
        // recirve att streams
        if (packet->stream_index == videoStreamIndex)
        {
            if (avcodec_send_packet(codecCtxt2, packet) < 0)
            {
                printf("Error sending packet for decoding.\n");
                exit(0);
            }
            while (1)
            {
                // if image frame
                int check = avcodec_receive_frame(codecCtxt2, frame);
                if (check == AVERROR(EAGAIN) || check == AVERROR_EOF)
                {
                    break;
                }
                else if (check < 0)
                {
                    printf("Error while decoding.\n");
                    exit(0);
                }
                // LOCK
                pthread_mutex_lock(&lock);
                if (count == bufferSize)
                {
                    pthread_cond_wait(&full, &lock); // Wait if buffer is full
                }
                // set variables
                frameNumber = codecCtxt2->frame_number;
                imageWidth = frame->linesize[0];
                imageHeight = codecCtxt2->height;
                // scale and change to rgb
                sws_scale(sws_ctx, (uint8_t const *const *)frame->data, frame->linesize, 0,
                          codecCtxt2->height, frameRGB->data, frameRGB->linesize);
                // save to circular buffer
                for (int i = 0; i < bufferSize;)
                {
                    int r = frameRGB->data[0][i];
                    int g = frameRGB->data[0][i + 1];
                    int b = frameRGB->data[0][i + 2];
                    imageBuffer[i++] = r;
                    imageBuffer[i++] = g;
                    imageBuffer[i++] = b;
                    writeIndex = (writeIndex + 3) % bufferSize; // keep track of what is being written
                    count += 3;                                 // count of index filled buffer
                }
                // UNLOCK
                pthread_cond_signal(&empty); // Signal that buffer is not full
                pthread_mutex_unlock(&lock);
            }
        }
    }
    flag = 1; // check if frames are done being sent
    return NULL;
}

void *consumer()
{
    usleep(sleepTime * 1000000); // sleep for some time
    char delete[32];
    char displayImg[32];
    while (flag == 0)
    {
        // LOCK
        pthread_mutex_lock(&lock);
        while (count == 0 && writeIndex < bufferSize)
        {
            pthread_cond_wait(&empty, &lock); // Wait if buffer is empty and producer hasn't finished
        }
        char imageFilename[32];
        // save what is written in circular buffer into image
        sprintf(imageFilename, "images/frame%d.ppm", frameNumber);
        FILE *outFile = fopen(imageFilename, "wb"); // Open file
        if (outFile == NULL)
        {
            exit(1);
        }
        fprintf(outFile, "P3\n%d %d\n255\n", imageWidth, imageHeight);
        // read from circular buffer
        for (int i = 0; i < bufferSize;)
        {
            int r = imageBuffer[i++];
            int g = imageBuffer[i++];
            int b = imageBuffer[i++];
            fprintf(outFile, "%d %d %d ", r, g, b);
            readIndex = (readIndex + 3) % bufferSize; // know what index is being read
            count -= 3;                               // reading so we decrement to remove what we read
        }
        fclose(outFile);
        // UNLOCK
        pthread_cond_signal(&full); // Signal producer that buffer is not full
        pthread_mutex_unlock(&lock);

        if (ratePerSec == frameNumber && flag == 1)
        {
            break;
        }
        printf("Frame num: %d, Displayed num: %d\n", frameNumber, displayPerSec);
        sprintf(displayImg, "images/frame%d.ppm", displayPerSec);
        GdkPixbuf *current_image = gdk_pixbuf_new_from_file(displayImg, NULL); // Load the PPM image using GdkPixbuf
        gtk_image_set_from_pixbuf(GTK_IMAGE(image), current_image);            // Set the image widget to display the new image
        g_object_unref(current_image);                                         // Clean up
        sprintf(delete, "rm images/frame%d.ppm", displayPerSec++);             // delete image after being displayed
        system(delete);
    }
    printf("Done\n");
    gtk_widget_destroy(GTK_WIDGET(window));
    return NULL;
}

int main(int argc, char *argv[])
{
    pthread_t prod_tid, cons_tid; // threads
    // intialize mutex and conditions
    pthread_mutex_init(&lock, NULL);
    pthread_cond_init(&full, NULL);
    pthread_cond_init(&empty, NULL);

    // get command line arguments
    if (argc < 3)
    {
        printf("Usage: <input file.mp4> <number frames per second> \n");
        exit(0);
    }
    filename = argv[1];
    ratePerSec = atoi(argv[2]);
    sleepTime = (1.0 / ratePerSec);

    // initialize context stuff --> done in main or crash
    formatCntxt = avformat_alloc_context(); // holds file header information
    if (!formatCntxt)
    {
        printf("Failed to allocate memory for Format Context\n");
        exit(0);
    }

    if (avformat_open_input(&formatCntxt, filename, NULL, NULL) != 0)
    { // get header info from video
        printf("Failed to open file");
        exit(0);
    }
    // printf some header information
    printf("Video file \"%s\": format %s, duration %lld us, bit_rate %lld\n", filename, formatCntxt->iformat->name,
           formatCntxt->duration, formatCntxt->bit_rate);
    if (avformat_find_stream_info(formatCntxt, NULL) < 0)
    { // find streams in file (e.g., video, audio, ...)
        // info stored in pFormatContext->streams
        printf("Failed to get the stream info\n");
        exit(0);
    }

    for (int i = 0; i < formatCntxt->nb_streams; i++) // search through all streams in context container
    {
        if (formatCntxt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) // find the video stream
        {
            videoStreamIndex = i;
            break;
        }
    }

    if (videoStreamIndex == -1)
    { // make sure there is a video stream index stored
        printf("Failed to find a video stream\n");
        exit(0);
    }

    codec = avcodec_find_decoder(formatCntxt->streams[videoStreamIndex]->codecpar->codec_id); // if there is, find decoder
    if (codec == NULL)
    {
        printf("Failed to find codec\n");
        exit(0);
    }

    codecCtxt1 = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codecCtxt1, formatCntxt->streams[videoStreamIndex]->codecpar);
    codecCtxt2 = avcodec_alloc_context3(codec);

    if (avcodec_parameters_to_context(codecCtxt2, formatCntxt->streams[videoStreamIndex]->codecpar) < 0)
    { // copy codec context
        printf("Failed to copy codec context.\n");
        exit(0);
    }
    if (
        avcodec_open2(codecCtxt2, codec, NULL) < 0)
    { // open codec
        printf("Failed to open codec.\n");
        exit(0);
    }

    frame = av_frame_alloc();
    if (frame == NULL)
    {
        printf("Failed to create AVFrame\n");
        exit(0);
    }

    frameRGB = av_frame_alloc();
    if (frameRGB == NULL)
    {
        printf("Failed to create AVFrame\n");
        exit(0);
    }

    uint8_t *buffer = NULL;
    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, codecCtxt2->width, codecCtxt2->height, 32); // allocate size of frame manually
    buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));                                            // allocate that size to a buffer
    av_image_fill_arrays(frameRGB->data, frameRGB->linesize, buffer, AV_PIX_FMT_RGB24,
                         codecCtxt2->width, codecCtxt2->height, 32);

    bufferSize = numBytes; // size of array created
    printf("Buffer size %d\n", bufferSize);
    imageBuffer = malloc(bufferSize * sizeof(int));

    packet = av_packet_alloc();
    if (packet == NULL)
    {
        printf("Failed to allocate packet\n");
        exit(0);
    }
    sws_ctx = sws_getContext(codecCtxt2->width, codecCtxt2->height, codecCtxt2->pix_fmt, codecCtxt2->width,
                             codecCtxt2->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);

    // Initialize GTK --> error when not done in main
    gtk_init(&argc, &argv);
    // Create the main window
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    gtk_window_set_title(GTK_WINDOW(window), "Video Player");
    gtk_window_set_default_size(GTK_WINDOW(window), imageWidth, imageHeight);

    // Create the image widget
    image = gtk_image_new();
    gtk_container_add(GTK_CONTAINER(window), image);

    // Connect the "destroy" signal to the main loop quit function
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    // Show the window
    gtk_widget_show_all(window);
    // Start the GTK main loop

    // initialize threads
    if (pthread_create(&prod_tid, NULL, producer, NULL) != 0)
    {
        printf("Error creating producer thread.\n");
        return 1;
    }
    if (pthread_create(&cons_tid, NULL, consumer, NULL) != 0)
    {
        printf("Error creating consumer thread.\n");
        return 1;
    }

    gtk_main();

    // join threads after they return
    if (pthread_join(prod_tid, NULL) != 0)
    {
        printf("Error joining producer thread.\n");
        return 1;
    }
    if (pthread_join(cons_tid, NULL) != 0)
    {
        printf("Error joining consumer thread.\n");
        return 1;
    }

    // Destroy
    pthread_mutex_destroy(&lock);
    pthread_cond_destroy(&full);
    pthread_cond_destroy(&empty);
    free(imageBuffer);
    return 0;
}