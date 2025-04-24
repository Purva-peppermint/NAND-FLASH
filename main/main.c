#include <stdio.h>
#include <string.h>
#include "driver/spi_master.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lfs.h"

#define PIN_NUM_MISO 13
#define PIN_NUM_MOSI 11
#define PIN_NUM_CLK 12
#define PIN_NUM_CS 10
#define SPI_HOST SPI3_HOST

#define PAGE_SIZE 2048
#define PAGES_PER_BLOCK 64
#define BLOCK_SIZE (PAGE_SIZE * PAGES_PER_BLOCK)

#define CMD_WRITE_ENABLE 0x06
#define CMD_LOAD_PROGRAM 0x02
#define CMD_PROGRAM_EXECUTE 0x10
#define CMD_READ_STATUS 0x0F
#define CMD_PAGE_DATA_READ 0x13
#define CMD_READ_DATA 0x03
#define CMD_BLOCK_ERASE 0xD8

static const char *TAG = "LFS_DRIVER";
static spi_device_handle_t spi;
static lfs_t lfs;
static struct lfs_config cfg;

static void wait_ready(void)
{
    uint8_t cmd[2] = {CMD_READ_STATUS, 0xC0};
    uint8_t status;
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = cmd,
        .rx_buffer = &status,
    };
    do
    {
        spi_device_transmit(spi, &t);
        vTaskDelay(pdMS_TO_TICKS(1));
    } while (status & 0x01);
}

static void write_enable(void)
{
    uint8_t cmd = CMD_WRITE_ENABLE;
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    spi_device_transmit(spi, &t);
}

static void erase_block(uint32_t block)
{
    write_enable();
    uint32_t row_addr = block * PAGES_PER_BLOCK;
    uint8_t cmd[4] = {CMD_BLOCK_ERASE, row_addr >> 16, row_addr >> 8, row_addr};
    spi_transaction_t t = {
        .length = 32,
        .tx_buffer = cmd,
    };
    spi_device_transmit(spi, &t);
    wait_ready();
}

static void load_program_data(uint16_t col, const uint8_t *data, size_t len)
{
    uint8_t cmd[4] = {CMD_LOAD_PROGRAM, col >> 8, col, 0};
    spi_transaction_t t = {
        .length = (4 + len) * 8,
    };
    uint8_t *tx_buf = malloc(len + 4);
    memcpy(tx_buf, cmd, 4);
    memcpy(tx_buf + 4, data, len);
    t.tx_buffer = tx_buf;
    spi_device_transmit(spi, &t);
    free(tx_buf);
}

static void program_execute(uint32_t page)
{
    write_enable();
    uint8_t cmd[4] = {CMD_PROGRAM_EXECUTE, page >> 16, page >> 8, page};
    spi_transaction_t t = {
        .length = 32,
        .tx_buffer = cmd,
    };
    spi_device_transmit(spi, &t);
    wait_ready();
}

static void page_data_read(uint32_t page)
{
    uint8_t cmd[4] = {CMD_PAGE_DATA_READ, page >> 16, page >> 8, page};
    spi_transaction_t t = {
        .length = 32,
        .tx_buffer = cmd,
    };
    spi_device_transmit(spi, &t);
    wait_ready();
}

static void read_data(uint16_t col, uint8_t *buffer, size_t len)
{
    uint8_t cmd[4] = {CMD_READ_DATA, col >> 8, col, 0x00};
    spi_transaction_t t = {
        .length = (4 + len) * 8,
        .tx_buffer = cmd,
        .rxlength = len * 8,
        .rx_buffer = buffer,
    };
    spi_device_transmit(spi, &t);
}

static void initialize_spi(void)
{
    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .max_transfer_sz = 4096};
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 1};
    spi_bus_initialize(SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    spi_bus_add_device(SPI_HOST, &devcfg, &spi);
}

// --- LittleFS Hooks ---
static int lfs_read(const struct lfs_config *c, lfs_block_t block,
                    lfs_off_t offset, void *buffer, lfs_size_t size)
{
    uint32_t page = block * PAGES_PER_BLOCK + offset / PAGE_SIZE;
    uint16_t col = offset % PAGE_SIZE;
    page_data_read(page);
    read_data(col, buffer, size);
    return 0;
}

static int lfs_prog(const struct lfs_config *c, lfs_block_t block,
                    lfs_off_t offset, const void *buffer, lfs_size_t size)
{
    uint32_t page = block * PAGES_PER_BLOCK + offset / PAGE_SIZE;
    uint16_t col = offset % PAGE_SIZE;
    load_program_data(col, buffer, size);
    program_execute(page);
    return 0;
}

static int lfs_erase(const struct lfs_config *c, lfs_block_t block)
{
    erase_block(block);
    return 0;
}

static int lfs_sync(const struct lfs_config *c)
{
    return 0;
}

static void initialize_lfs(lfs_t *lfs, struct lfs_config *cfg)
{
    memset(cfg, 0, sizeof(struct lfs_config));
    cfg->read = lfs_read;
    cfg->prog = lfs_prog;
    cfg->erase = lfs_erase;
    cfg->sync = lfs_sync;
    cfg->read_size = 256;
    cfg->prog_size = 256;
    cfg->block_size = BLOCK_SIZE;
    cfg->block_count = 1024;
    cfg->block_cycles = 1;
    cfg->cache_size = 2048;
    cfg->lookahead_size = 128;
    cfg->name_max = 255;

    if (lfs_mount(lfs, cfg))
    {
        ESP_LOGI(TAG, "Formatting LittleFS...");
        lfs_format(lfs, cfg);
        lfs_mount(lfs, cfg);
    }
    ESP_LOGI(TAG, "LittleFS mounted successfully");
}

void app_main(void)
{
    initialize_spi();

    // Optional manual test before mounting FS
    const char *write_data = "hello";
    write_enable();
    load_program_data(0x1100, (uint8_t *)write_data, strlen(write_data));
    program_execute(0x1100);

    initialize_lfs(&lfs, &cfg);

    // Uncomment for testing LFS file ops
    // lfs_file_t file;
    // const char *text = "Hello from ESP32-S3 + W25N01GV!";
    // char buffer[64] = {0};

    // lfs_file_open(&lfs, &file, "myfile.txt", LFS_O_WRONLY | LFS_O_CREAT);
    // lfs_file_write(&lfs, &file, text, strlen(text));
    // lfs_file_close(&lfs, &file);

    // lfs_file_open(&lfs, &file, "myfile.txt", LFS_O_RDONLY);
    // lfs_file_read(&lfs, &file, buffer, sizeof(buffer));
    // lfs_file_close(&lfs, &file);

    // ESP_LOGI(TAG, "Read from file: %s", buffer);
}
