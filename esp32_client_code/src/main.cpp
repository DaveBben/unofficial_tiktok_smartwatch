#define USE_DMA
#define USE_DMA_TO_TFT

// https://github.com/espressif/esp-idf/issues/3497

#include "JPEGDEC.h"
#include <TFT_eSPI.h>
#include <SPI.h>
#include <WiFi.h>
#include "driver/i2s.h"
#include "driver/gpio.h"
#include <WebSocketsClient.h>
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include "driver/gpio.h"

#define SPI_FREQUENCY 80000000
#define SAMPLE_RATE (44100)
#define I2S_NUM (0)
#define I2S_BCK_IO (GPIO_NUM_4)
#define I2S_WS_IO (GPIO_NUM_5)
#define I2S_DO_IO (GPIO_NUM_18)
#define I2S_DI_IO (-1)
#define HEADER_MESSAGE_LENGTH 2

// https://community.platformio.org/t/identifier-is-undefined-setenv-tzset/16162/2
_VOID _EXFUN(tzset, (_VOID));
int _EXFUN(setenv, (const char *__string, const char *__value, int __overwrite));

const uint16_t VIDEO_FRAME_BUFFER_SIZE = 6000;
const uint16_t AUDIO_FRAME_BUFFER_SIZE = 4000;

/**
 * @brief 
 *  Double buffer for DMA
 */
#ifdef USE_DMA
uint16_t dmaBuffer1[2048]; 
uint16_t dmaBuffer2[2048]; 
uint16_t *dmaBufferPtr = dmaBuffer1;
bool dmaBufferSel = 0;
#endif

const char *ssid = "";                       // Enter SSID
const char *password = "^";                    // Enter Password
const char *websockets_server_host = "192.168.86.38"; // Enter server adress
const uint16_t websockets_server_port = 8000;         // Enter server port

WebSocketsClient websocketClient;

JPEGDEC jpeg;

struct Frame
{
    uint8_t video_data[VIDEO_FRAME_BUFFER_SIZE];
    uint8_t audio_data[AUDIO_FRAME_BUFFER_SIZE];
    uint16_t video_data_size;
    uint16_t audio_data_size;
};

/**
 * @brief 
 * Using a circular buffer to store and read frames
 */
struct Circular_Buffer
{
    uint8_t start_pointer;
    uint8_t end_pointer;
    struct Frame *items;
};

#define USE_SERIAL Serial

/*Change to your screen resolution*/
static const uint16_t screenWidth = 135;
static const uint16_t screenHeight = 240;

TFT_eSPI tft = TFT_eSPI(screenWidth, screenHeight); /* TFT instance */

const uint8_t CIRCULAR_BUFFER_SIZE = 4;

int JPEGDraw(JPEGDRAW *pDraw);
void render_image(void *parameter);
void socket_loop(void *parameter);

struct Circular_Buffer *data_buffer;

uint8_t video_frame_buffer[VIDEO_FRAME_BUFFER_SIZE] = {0};
uint8_t audio_frame_buffer[AUDIO_FRAME_BUFFER_SIZE] = {0};

unsigned long lastUpdate = 0;
volatile int fps = 0;
volatile int updates = 0;
volatile uint16_t buttonPressed = 0;
volatile unsigned long lastDebounceTime = 0;

uint8_t isBufferEmpty(struct Circular_Buffer *circular_buff)
{
    if (circular_buff->end_pointer == circular_buff->start_pointer)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

uint8_t isBufferFull(struct Circular_Buffer *circular_buff)
{
    if (((circular_buff->end_pointer + 1) % CIRCULAR_BUFFER_SIZE) == circular_buff->start_pointer)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}


void read_data_frame(uint8_t *video_buffer, uint16_t *video_size, uint8_t *audio_buffer, uint16_t *audio_size, Circular_Buffer *circular_buffer)
{
    if (!isBufferEmpty(circular_buffer))
    {

        struct Frame *single_frame = &circular_buffer->items[circular_buffer->start_pointer];
        memcpy(video_buffer, single_frame->video_data, single_frame->video_data_size);
        memcpy(audio_buffer, single_frame->audio_data, single_frame->audio_data_size);
        *video_size = single_frame->video_data_size;
        *audio_size = single_frame->audio_data_size;

        circular_buffer->start_pointer = (circular_buffer->start_pointer + 1) % CIRCULAR_BUFFER_SIZE;
    }
}

void reset_buffer(struct Circular_Buffer *circular_buff)
{
    circular_buff->end_pointer = 1;
    circular_buff->start_pointer = 1;
}

/**
 * @brief Since we are not using the button this is not needed
 * 
 * @param arg 
 */
static void IRAM_ATTR gpio_isr(void *arg)
{


    if ((millis() - lastDebounceTime) > 3000)
    {
        lastDebounceTime = millis();
        buttonPressed = 1;
    }
}

void obtain_time()
{
    if (sntp_enabled())
    {
        sntp_stop();
    }

    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}

void init_clock()
{
    struct tm timeinfo;
    time_t now;
    time(&now);
    localtime_r(&now, &timeinfo);
    while (timeinfo.tm_year < (2021 - 1900))
    {
        Serial.println("Time not set, trying...");
        obtain_time();
        time(&now);
        localtime_r(&now, &timeinfo);
        delay(200);
    }

    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0", 1); //Eastern Time Zone (NY)
    tzset();
}

void display_clock()
{
    char hour[3];
    char minute[3];
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    struct tm timeinfo;
    time_t now;

    time(&now);
    localtime_r(&now, &timeinfo);

    strftime(hour, sizeof(hour), "%I", &timeinfo);
    strftime(minute, sizeof(minute), "%M", &timeinfo);
    // tft.fillScreen(TFT_BLACK);
    tft.setTextSize(24);
    tft.setCursor(screenWidth / 2 - 30, 50);
    tft.println(hour);
    tft.setCursor(screenWidth / 2 - 30, 150);
    tft.println(minute);
}

/**
 * @brief No being using because no button
 * 
 */
void request_video()
{
    websocketClient.sendTXT("play");
}

void websocket_event(WStype_t type, uint8_t *payload, size_t length)
{

    switch (type)
    {
    case WStype_DISCONNECTED:
        USE_SERIAL.printf("[WSc] Disconnected!\n");
        reset_buffer(data_buffer);
        vTaskDelay(100 / portTICK_RATE_MS);
        i2s_zero_dma_buffer(I2S_NUM_0);
        break;
    case WStype_CONNECTED:
        USE_SERIAL.printf("[WSc] Connected to url: %s\n", payload);
        websocketClient.sendTXT("play");
        break;
    case WStype_TEXT:
        USE_SERIAL.printf("[WSc] get text: %s\n", payload);
        if (strcmp((char *)payload, "done") == 0)
        {
            // This delay is a lazy hack
            // basically need to wait until
            // all the i2s transfers are actually
            // done
            vTaskDelay(100 / portTICK_RATE_MS);
            i2s_zero_dma_buffer(I2S_NUM_0);

        }
        break;
    case WStype_BIN:
        if (!isBufferFull(data_buffer))
        {
            if (length > 0)
            {
                //The data will come in packaged. We want to deconstruct and put
                // into audio/video frame

                struct Frame *single_frame = &data_buffer->items[data_buffer->end_pointer];

                // // first two bytes represent video length
                uint16_t video_length = ((uint16_t)payload[0] << 8) | payload[1];
                single_frame->video_data_size = video_length;

                // Serial.printf("Video length is %d\n", video_length);
                memcpy(single_frame->video_data, payload + HEADER_MESSAGE_LENGTH, video_length);
                uint16_t audio_length = length - video_length - HEADER_MESSAGE_LENGTH;
                single_frame->audio_data_size = audio_length;

                // Serial.printf("Audio length is %d\n", audio_length);
                memcpy(single_frame->audio_data, payload + HEADER_MESSAGE_LENGTH + video_length, audio_length);
                data_buffer->end_pointer = (data_buffer->end_pointer + 1) % CIRCULAR_BUFFER_SIZE;
            }
        }
        else
        {
            Serial.println("Buffer is full!");
        }
        break;
    }
}

int JPEGDraw(JPEGDRAW *pDraw)
{
    int iCount;
    iCount = pDraw->iWidth * pDraw->iHeight; // number of pixels to draw in this call
    // Serial.printf("Drawing %d pixels\n", iCount);
    // delay(5000);

    if (dmaBufferSel)
        dmaBufferPtr = dmaBuffer2;
    else
        dmaBufferPtr = dmaBuffer1;
    dmaBufferSel = !dmaBufferSel;


    tft.pushImageDMA(pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight, pDraw->pPixels, dmaBufferPtr);

    return 1;
}

void handle_video(void *pvParameter)
{
    for (;;)
    { // infinite loop
        if (!isBufferEmpty(data_buffer))
        {

            uint16_t video_length, audio_length;
            read_data_frame(video_frame_buffer, &video_length, audio_frame_buffer, &audio_length, data_buffer);
            int i;
            tft.startWrite();
            if (jpeg.openRAM(video_frame_buffer, video_length, JPEGDraw))
            {
                jpeg.setPixelType(RGB565_BIG_ENDIAN);
                // lTime = micros();
                if (jpeg.decode(0, 0, 0))
                {
                    tft.endWrite();
                    updates += 1;
                    size_t written = 0;
                    i2s_write(I2S_NUM_0, audio_frame_buffer, audio_length, &written, portMAX_DELAY);
                }
                else
                {
                    Serial.println("Decode error");
                }
                jpeg.close();
            }
        }

        unsigned long time = millis();

        if (time - lastUpdate >= 1000)
        {
            float overtime = float(time - lastUpdate) / 1000.0;
            fps = floor((float)updates / overtime);

            lastUpdate = time;
            updates = 0;

            Serial.print("FPS: ");
            Serial.println(fps);
        }
    }
}

void setup()
{

    data_buffer = (struct Circular_Buffer *)malloc(sizeof(struct Circular_Buffer));
    data_buffer->items = (struct Frame *)malloc(sizeof *data_buffer->items * CIRCULAR_BUFFER_SIZE);
    data_buffer->end_pointer = 1;
    data_buffer->start_pointer = 1;

    Serial.begin(115200);

    if (data_buffer == NULL or data_buffer->items == NULL)
    {
        Serial.println("Failed to allocate memory on heap");
        return;
    }

    if (dmaBuffer1 == NULL or dmaBuffer2 == NULL)
    {
        Serial.println("Failed to allocate DMA Buffer");
        return;
    }

    WiFi.begin(ssid, password);
    // Wait some time to connect to wifi
    for (int i = 0; i < 10 && WiFi.status() != WL_CONNECTED; i++)
    {
        Serial.print(".");
        delay(1000);
    }

    // Check if connected to wifi
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("No Wifi!");
        return;
    }

    tft.begin();
    tft.initDMA();
    init_clock();
    tft.fillScreen(TFT_BLACK);
    display_clock();

    websocketClient.begin(websockets_server_host, 8080, "/");
    websocketClient.onEvent(websocket_event);

    lastUpdate = millis();

    i2s_config_t i2s_config = {
        .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_TX), // Only TX
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, // 16-bit per channel
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // 2-channels
        .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_I2S_MSB),
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 64,
        .dma_buf_len = 50,
        .use_apll = false,

    };
    i2s_pin_config_t pin_config = {
        .bck_io_num = 13,
        .ws_io_num = 2,
        .data_out_num = 15,
        .data_in_num = -1 // Not used
    };

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);

    size_t written = 0;

    i2s_set_clk(I2S_NUM_0, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

    i2s_zero_dma_buffer(I2S_NUM_0);
    i2s_start(I2S_NUM_0);

    

    // No button, so not using the following

    // install gpio isr service
    // gpio_install_isr_service(0);
    // hook isr handler for specific gpio pin

    // gpio_config_t io_conf = {};
    // io_conf.intr_type = GPIO_INTR_POSEDGE;
    // io_conf.mode = GPIO_MODE_INPUT;
    // io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    // io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    // io_conf.pin_bit_mask = (1ULL << GPIO_NUM_17);
    // gpio_config(&io_conf);

    // gpio_isr_handler_add(GPIO_NUM_17, gpio_isr, (void *)GPIO_NUM_17);




    xTaskCreatePinnedToCore(
        socket_loop,   /* Function to implement the task */
        "handle_loop", /* Name of the task */
        4096,          /* Stack size in words */
        NULL,          /* Task input parameter */
        1,             /* Priority of the task */
        NULL,          /* Task handle. */
        0);            /* Core where the task should run */

    xTaskCreatePinnedToCore(
        handle_video,             /* Function to implement the task */
        "handle_video",           /* Name of the task */
        4096,                     /* Stack size in words */
        NULL,                     /* Task input parameter */
        configMAX_PRIORITIES - 1, /* Priority of the task */
        NULL,                     /* Task handle. */
        1);                       /* Core where the task should run */
}

void loop()
{
}

void socket_loop(void *parameter)
{
    for (;;)
    { // infinite loop
        while (!isBufferFull(data_buffer))
        {
            if (buttonPressed)
            {
                buttonPressed = 0;
                websocketClient.sendTXT("play");
            }
            websocketClient.loop();
            vTaskDelay(5 / portTICK_RATE_MS);
        }

        vTaskDelay(10 / portTICK_RATE_MS);
    }
}