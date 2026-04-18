/**
 * kwm_protocol.h - Klipper WiFi MCU shared protocol definitions
 *
 * SPI frame (Pi5 <-> host ESP32-C5):
 *   Fixed 256-byte full-duplex transactions. Both sides simultaneously
 *   transmit their outgoing frame while receiving the other's frame.
 *   The host ESP asserts DATA_READY GPIO when it has data queued for the Pi.
 *   Pi polls DATA_READY or uses a GPIO interrupt to know when to initiate
 *   a transaction.
 *
 * TCP frame (host ESP <-> MCU ESP, over WiFi 6):
 *   Variable-length stream framing. Host ESP acts as TCP server; each MCU
 *   ESP connects as a client and identifies itself with KWM_CMD_CONNECT.
 *
 * Frame layout (SPI, exactly 256 bytes):
 *   [0..1]   Magic: 0xAB 0xCD
 *   [2]      Command (kwm_cmd_t)
 *   [3]      MCU ID (bits 3:0) | Flags (bits 7:4)
 *   [4]      Sequence number (wraps 0-255)
 *   [5]      Reserved (send as 0)
 *   [6..7]   Payload length, big-endian (0-246)
 *   [8..253] Payload (pad unused bytes with 0)
 *   [254..255] CRC16-CCITT of bytes [0..253]
 *
 * Frame layout (TCP, variable):
 *   [0..1]   Magic: 0xAB 0xCD
 *   [2]      Command (kwm_cmd_t)
 *   [3]      MCU ID (bits 3:0) | Flags (bits 7:4)
 *   [4]      Sequence number
 *   [5]      Reserved
 *   [6..7]   Payload length, big-endian (0-4096)
 *   [8..N]   Payload
 *   [N+1..N+2] CRC16-CCITT of bytes [0..N]
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ── Magic ──────────────────────────────────────────────────────────────── */
#define KWM_MAGIC_0         0xAB
#define KWM_MAGIC_1         0xCD
#define KWM_MAGIC           0xABCD

/* ── SPI frame constants ─────────────────────────────────────────────────── */
#define KWM_SPI_FRAME_LEN   256
#define KWM_SPI_HEADER_LEN  8
#define KWM_SPI_CRC_LEN     2
#define KWM_SPI_PAYLOAD_MAX (KWM_SPI_FRAME_LEN - KWM_SPI_HEADER_LEN - KWM_SPI_CRC_LEN)  /* 246 */

/* ── TCP frame constants ─────────────────────────────────────────────────── */
#define KWM_TCP_HEADER_LEN  8
#define KWM_TCP_CRC_LEN     2
#define KWM_TCP_PAYLOAD_MAX 4096

/* ── Commands ────────────────────────────────────────────────────────────── */
typedef enum {
    KWM_CMD_NOOP        = 0x00,  /* No data – padding frame, always valid   */
    KWM_CMD_DATA        = 0x01,  /* Serial data payload                     */
    KWM_CMD_CONNECT     = 0x02,  /* MCU bridge connected (MCU→host)         */
    KWM_CMD_DISCONNECT  = 0x03,  /* MCU bridge disconnected                 */
    KWM_CMD_STATUS_REQ  = 0x04,  /* Request status from peer                */
    KWM_CMD_STATUS_RSP  = 0x05,  /* Status response                         */
    KWM_CMD_RESET       = 0x06,  /* Request peer reset / flush buffers      */
    KWM_CMD_ACK         = 0x07,  /* Acknowledge (seq echo)                  */
} kwm_cmd_t;

/* ── Flags (upper nibble of byte [3]) ───────────────────────────────────── */
#define KWM_FLAG_NONE       0x00
#define KWM_FLAG_MORE_DATA  0x10  /* Sender has more frames queued          */
#define KWM_FLAG_URGENT     0x20  /* High-priority data                     */

/* ── Max MCU count ───────────────────────────────────────────────────────── */
#define KWM_MAX_MCU         8

/* ── MCU ID derivation (no pre-flash configuration needed) ──────────────────
 *
 * Each MCU-side ESP derives its ID at runtime from its hardware MAC address.
 * The same firmware binary flashes onto every board.
 *
 * Algorithm: FNV-1a hash of the device-unique MAC bytes (octets 3-5; the
 * first 3 bytes are the Espressif OUI and identical on every board from the
 * same vendor), folded to KWM_MAX_MCU slots.
 *
 * The MCU ID is included in every TCP frame header AND the full 6-byte MAC is
 * sent in the KWM_CMD_CONNECT payload so the host can log "MCU 3 = aa:bb:cc".
 *
 * MCU_ID_CONNECT_PAYLOAD_LEN is the expected CONNECT payload length.
 * ─────────────────────────────────────────────────────────────────────────── */
#define KWM_MCU_ID_CONNECT_PAYLOAD_LEN  6   /* 6 bytes: raw MAC address */

/* ── TCP port ────────────────────────────────────────────────────────────── */
#define KWM_TCP_PORT        8842

/* ── WiFi AP defaults (host ESP) ─────────────────────────────────────────── */
#define KWM_WIFI_SSID       "KlipperMesh"
#define KWM_WIFI_PASSWORD   "klipper42!"
#define KWM_WIFI_CHANNEL    6           /* Override in sdkconfig for 5 GHz  */
#define KWM_WIFI_MAX_STA    KWM_MAX_MCU
#define KWM_AP_IP           "192.168.42.1"
#define KWM_AP_NETMASK      "255.255.255.0"

/* ── SPI pin defaults (host ESP compact board – matches FSPI pad ring) ──────
 *
 * Host board pinout (ESP32-C5 compact / DevKitM-1 style):
 *
 *   GPIO6  = FSPICLK  → Pi SPI0 SCLK  (pin 23)
 *   GPIO0             → Pi SPI0 MOSI  (pin 19)   [MOSI into ESP = data from Pi]
 *   GPIO8             → Pi SPI0 MISO  (pin 21)   [MISO out of ESP = data to Pi]
 *   GPIO10 = FSPICS0  → Pi SPI0 CE0   (pin 24)   [active-low CS from Pi]
 *
 *   NOTE: GPIO0/GPIO8 use GPIO matrix routing (not IO_MUX dedicated pads)
 *   because ESP32-C5 SPI2 slave mode has an IO_MUX direction bug on the
 *   dedicated FSPID/FSPIQ pads (GPIO7/GPIO2) — they default to master
 *   direction, leaving MISO undriven and MOSI fighting the slave output.
 *   GPIO25            → Pi GPIO8      (BCM)       DATA_READY: ESP→Pi interrupt
 *   GPIO26            → Pi GPIO7      (BCM)       HANDSHAKE:  Pi→ESP (optional)
 *
 * Using the hardware FSPI pads avoids GPIO-matrix routing and allows the
 * maximum safe SPI clock speed on this board.
 * ─────────────────────────────────────────────────────────────────────────── */
#define KWM_SPI_HOST        SPI2_HOST
#define KWM_PIN_MOSI        3    /* GPIO3  = GPIO matrix (GPIO0 has XTAL_32K cap)    */
#define KWM_PIN_MISO        1    /* GPIO1  = D1 header pin, GPIO matrix, no FSPI bug */
#define KWM_PIN_SCLK        6    /* GPIO6  = FSPICLK                         */
#define KWM_PIN_CS          10   /* GPIO10 = FSPICS0                         */
#define KWM_PIN_DATA_READY  25   /* GPIO25, output → Pi BCM GPIO25 (pin 22)  */
#define KWM_PIN_HANDSHAKE   26   /* GPIO26, input  ← Pi BCM GPIO24 (pin 18)  */

/* ── Frame structs ───────────────────────────────────────────────────────── */

/** Overlay on a raw 256-byte SPI buffer. */
typedef struct __attribute__((packed)) {
    uint8_t  magic[2];          /* 0xAB, 0xCD                              */
    uint8_t  cmd;               /* kwm_cmd_t                               */
    uint8_t  mcu_id_flags;      /* bits[3:0]=mcu_id, bits[7:4]=flags       */
    uint8_t  seq;               /* sequence number                          */
    uint8_t  reserved;
    uint16_t payload_len;       /* big-endian                              */
    uint8_t  payload[KWM_SPI_PAYLOAD_MAX];
    uint16_t crc;               /* big-endian CRC16-CCITT of bytes [0..253]*/
} kwm_spi_frame_t;

_Static_assert(sizeof(kwm_spi_frame_t) == KWM_SPI_FRAME_LEN,
               "kwm_spi_frame_t size mismatch");

/** TCP frame header (followed by payload, then 2-byte CRC). */
typedef struct __attribute__((packed)) {
    uint8_t  magic[2];
    uint8_t  cmd;
    uint8_t  mcu_id_flags;
    uint8_t  seq;
    uint8_t  reserved;
    uint16_t payload_len;       /* big-endian */
} kwm_tcp_header_t;

/* ── Inline helpers ──────────────────────────────────────────────────────── */

static inline uint8_t kwm_mcu_id(const uint8_t mcu_id_flags) {
    return mcu_id_flags & 0x0F;
}

static inline uint8_t kwm_flags(const uint8_t mcu_id_flags) {
    return mcu_id_flags & 0xF0;
}

static inline uint8_t kwm_pack_id_flags(uint8_t mcu_id, uint8_t flags) {
    return (mcu_id & 0x0F) | (flags & 0xF0);
}

static inline uint16_t kwm_be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline void kwm_put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

/* ── CRC16-CCITT (poly 0x1021, init 0xFFFF, no final XOR) ───────────────── */

static inline uint16_t kwm_crc16_update(uint16_t crc, uint8_t byte) {
    crc ^= (uint16_t)byte << 8;
    for (int i = 0; i < 8; i++) {
        if (crc & 0x8000)
            crc = (crc << 1) ^ 0x1021;
        else
            crc <<= 1;
    }
    return crc;
}

static inline uint16_t kwm_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++)
        crc = kwm_crc16_update(crc, data[i]);
    return crc;
}

/** Build and CRC a SPI frame in place. buf must be KWM_SPI_FRAME_LEN bytes. */
static inline void kwm_spi_frame_build(uint8_t *buf, kwm_cmd_t cmd,
                                        uint8_t mcu_id, uint8_t flags,
                                        uint8_t seq,
                                        const uint8_t *payload, uint16_t len) {
    kwm_spi_frame_t *f = (kwm_spi_frame_t *)buf;
    f->magic[0]      = KWM_MAGIC_0;
    f->magic[1]      = KWM_MAGIC_1;
    f->cmd           = (uint8_t)cmd;
    f->mcu_id_flags  = kwm_pack_id_flags(mcu_id, flags);
    f->seq           = seq;
    f->reserved      = 0;
    kwm_put_be16((uint8_t *)&f->payload_len, len);
    if (len && payload)
        memcpy(f->payload, payload, len);
    if (len < KWM_SPI_PAYLOAD_MAX)
        memset(f->payload + len, 0, KWM_SPI_PAYLOAD_MAX - len);
    uint16_t crc = kwm_crc16(buf, KWM_SPI_FRAME_LEN - 2);
    kwm_put_be16((uint8_t *)&f->crc, crc);
}

/** Validate magic and CRC of a received SPI frame. */
static inline bool kwm_spi_frame_valid(const uint8_t *buf) {
    const kwm_spi_frame_t *f = (const kwm_spi_frame_t *)buf;
    if (f->magic[0] != KWM_MAGIC_0 || f->magic[1] != KWM_MAGIC_1)
        return false;
    uint16_t expected = kwm_crc16(buf, KWM_SPI_FRAME_LEN - 2);
    uint16_t received = kwm_be16((const uint8_t *)&f->crc);
    return expected == received;
}
