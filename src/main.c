#include "wokwi-api.h"
#include <stdio.h>
#include <stdlib.h>

typedef enum {
  MODE_COMMAND,
  MODE_DATA,
} chip_mode_t;

typedef struct {
  pin_t    cs_pin;
  pin_t    dc_pin;
  pin_t    rst_pin;
  spi_dev_t spi;
  uint8_t  spi_buffer[1024];

  /* Framebuffer state */
  buffer_t framebuffer;
  uint32_t width;
  uint32_t height;

  /* Command state machine */
  chip_mode_t mode;
  uint8_t command_code;
  uint8_t command_size;
  uint8_t command_index;
  uint8_t command_buf[16];
  bool ram_write;

  // Memory and addressing settings
  uint32_t active_column;
  uint32_t column_start;
  uint32_t column_end;
  uint32_t active_row;
  uint32_t row_start;
  uint32_t row_end;
  uint32_t scanning_direction;
} chip_state_t;

/* Chip command codes */
// https://www.displayfuture.com/Display/datasheet/controller/ST7735.pdf
// Unimplemented commands are not implemented here
#define CMD_NOP      (0x00) // No Operation
#define CMD_CASET    (0x2a) // Column Address Set
#define CMD_RASET    (0x2b) // Row Address Set
#define CMD_RAMWR    (0x2c) // Memory Write
#define CMD_RAMRD    (0x2e) // Memory Read
#define CMD_MADCTL   (0x36) // Memory Access Control

/* Scanning direction bits */
#define SCAN_MY (0b10000000)
#define SCAN_MX (0b01000000)
#define SCAN_MV (0b00100000)

static void chip_pin_change(void *user_data, pin_t pin, uint32_t value);
static void chip_spi_done(void *user_data, uint8_t *buffer, uint32_t count);

void chip_reset(chip_state_t *chip) {
  chip->ram_write = false;
  chip->active_column = 0;
  chip->column_start = 0;
  chip->column_end = 127;

  chip->active_row = 0;
  chip->row_start = 0;
  chip->row_end = 35; // This should get overwritten by the first RESET command, if not, only some of the display will be used with garbage data
}

void chip_init(void) {
  chip_state_t *chip = malloc(sizeof(chip_state_t));

  const pin_watch_config_t watch_config = {
    .edge = BOTH,
    .pin_change = chip_pin_change,
    .user_data = chip,
  };
  
  chip->cs_pin = pin_init("CS", INPUT_PULLUP);
  pin_watch(chip->cs_pin, &watch_config);

  chip->dc_pin = pin_init("DC", INPUT);
  pin_watch(chip->dc_pin, &watch_config);

  chip->rst_pin = pin_init("RST", INPUT_PULLUP);
  pin_watch(chip->rst_pin, &watch_config);

  const spi_config_t spi_config = {
    .sck = pin_init("SCK", INPUT),
    .mosi = pin_init("MOSI", INPUT),
    .miso = NO_PIN,
    .done = chip_spi_done,
    .user_data = chip,
  };
  chip->spi = spi_init(&spi_config);

  chip->framebuffer = framebuffer_init(&chip->width, &chip->height);

  chip_reset(chip);

  printf("ST7735 Driver Chip Initialized! W=%d, H=%d\n", chip->width, chip->height);
}

/* Converts a 16-bit RGB565 (5 bits for red, 6 for green, 5 for blue) into 32-bit RGBA (8-bit per channel) */
uint32_t rgb565_to_rgba(uint16_t value) {
  return 0xff000000  // Alpha
         | ((value & 0x001f) << 19) // Blue
         | ((value & 0x07e0) << 5) // Green
         | ((value & 0xf800) >> 8); // Red
}

void chip_pin_change(void *user_data, pin_t pin, uint32_t value) {
  chip_state_t *chip = (chip_state_t*)user_data;

  // Handle CS pin logic
  // Don't reset command handlers on CS pin toggle
  // This is to allow the chip to process the remaining SPI command)
  if (pin == chip->cs_pin) {
    if (value == LOW) {
      spi_start(chip->spi, chip->spi_buffer, sizeof(chip->spi_buffer));
    } else {
      spi_stop(chip->spi);
    }
  }

  // Handle DC pin logic
  if (pin == chip->dc_pin && chip->mode != value) {
    if (pin_read(chip->cs_pin) == LOW) {  // If our chip is still active (otherwise the cs pin change handler will take care of spi_stop)
      spi_stop(chip->spi); // Process remaining data in SPI buffer
    }
    chip->mode = value;
    if (pin_read(chip->cs_pin) == LOW) {
      spi_start(chip->spi, chip->spi_buffer, sizeof(chip->spi_buffer));
    }
  }

  if (pin == chip->rst_pin && value == LOW) {
    spi_stop(chip->spi); // Process remaining data in SPI buffer
    chip_reset(chip);
  }
}

int command_args_size(uint8_t command_code) {
  switch (command_code) {
    case CMD_MADCTL: return 1;
    case CMD_CASET:
    case CMD_RASET:   return 4;
    default:          return 0;
  }
}

void execute_command(chip_state_t *chip) {
  switch (chip->command_code) {

    case CMD_RAMWR:
      chip->ram_write = true;
      break;

    case CMD_MADCTL:
      chip->scanning_direction = chip->command_buf[0] & 0xfc;
      break;

    case CMD_CASET:
    case CMD_RASET: {
      uint16_t arg0 = (chip->command_buf[0] << 8) | chip->command_buf[1];
      uint16_t arg2 = (chip->command_buf[2] << 8) | chip->command_buf[3];
      bool set_row = chip->command_code == CMD_RASET;
      if ((chip->scanning_direction & SCAN_MV) ? !set_row : set_row) {
        chip->active_row = arg0;
        chip->row_start = arg0;
        chip->row_end = arg2;
      } else {
        chip->active_column = arg0;
        chip->column_start = arg0;
        chip->column_end = arg2;
      }
      break;
    }

    default:
      printf("Warning: unknown command 0x%02x\n", chip->command_code);
      break;
  }
}

void process_command(chip_state_t *chip, uint8_t *buffer, uint32_t buffer_size) {
  chip->ram_write = false;
  for (int i = 0; i < buffer_size; i++) {
    chip->command_code = buffer[i];
    chip->command_size = command_args_size(chip->command_code);
    chip->command_index = 0;
    if (!chip->command_size) {
      execute_command(chip);
    }
  }
}

void process_command_args(chip_state_t *chip, uint8_t *buffer, uint32_t buffer_size) {
  for (int i = 0; i < buffer_size; i++) {
    if (chip->command_index < chip->command_size) {
      chip->command_buf[chip->command_index++] = buffer[i];
      if (chip->command_size == chip->command_index) {
        execute_command(chip);
      }
    }
  }
}

void process_data(chip_state_t *chip, const uint16_t *buffer16, uint32_t buffer_size) {
  uint32_t color;

  for (int i = 0; i < buffer_size; i++) {
    int x = chip->active_column;
    int y = chip->active_row;
    if (chip->scanning_direction & SCAN_MV) {
      x = chip->scanning_direction & SCAN_MX ? (chip->width - 1 - x) : x;
      y = chip->scanning_direction & SCAN_MY ? (chip->height - 1 - y) : y;
    } else {
      x = chip->scanning_direction & SCAN_MY ? (chip->width - 1 - x) : x;
      y = chip->scanning_direction & SCAN_MX ? (chip->height - 1 - y) : y;
    }

    color = rgb565_to_rgba(buffer16[i]);
    int pix_index = y * chip->width + x;
    buffer_write(chip->framebuffer, pix_index * sizeof(color), &color, sizeof(color));

    if (chip->scanning_direction & SCAN_MV) {
      chip->active_row++;
      if (chip->active_row > chip->row_end) {
        chip->active_row = chip->row_start;
        chip->active_column++;
        if (chip->active_column > chip->column_end) {
          chip->active_column = chip->column_start;
        }
      }
    } else {
      chip->active_column++;
      if (chip->active_column > chip->column_end) {
        chip->active_column = chip->column_start;
        chip->active_row++;
        if (chip->active_row > chip->row_end) {
          chip->active_row = chip->row_start;
        }
      }
    }
  }
}

void chip_spi_done(void *user_data, uint8_t *buffer, uint32_t count) {
  chip_state_t *chip = (chip_state_t*)user_data;
  if (!count) {
    // This means that we got here from spi_stop, and no data was received
    return;
  }

  if (chip->mode == MODE_DATA) {
    if (chip->ram_write) {
      process_data(chip, (const uint16_t*)buffer, count / 2);
    } else {
      process_command_args(chip, buffer, count);
    }
  } else {
    process_command(chip, buffer, count);
  }


  if (pin_read(chip->cs_pin) == LOW) {
    // Receive the next buffer
    spi_start(chip->spi, chip->spi_buffer, sizeof(chip->spi_buffer));
  }
}
