#include <string.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "pico/critical_section.h"

#include "audio.h"
#include "../../../src/devices/hid_media.h"   // Relative: consumers of these sources (RELOAD_DIR) need no include path

#define SAMPLES_BUFFER_SIZE    2048
#define SAMPLES_CHUNK_SIZE     32
#define SAMPLE_REPETITION_RATE 4
_Static_assert(SAMPLES_CHUNK_SIZE == 32, "audio_buffer_dequeue fills empty_samples with 8 words");

typedef struct {
    critical_section_t cs;
    uint8_t samples[SAMPLES_BUFFER_SIZE];
    uint8_t empty_samples[SAMPLES_CHUNK_SIZE] __attribute__((aligned(4)));
    uint16_t head;
    uint16_t tail;
    uint16_t size;
} audio_buffer_t;

static audio_buffer_t __not_in_flash() audio_buffer;

static uint32_t single_sample = 0;
static uint32_t *single_sample_ptr = &single_sample;
static int pwm_dma_channel, trigger_dma_channel, sample_dma_channel;

static inline void __not_in_flash_func(audio_buffer_init)(audio_buffer_t *audio_buffer) {
    // Dedicated spin lock: the "striped" ones of critical_section_init are shared
    // with the rest of the SDK (the cause of Trinity's T-77 on PicoDVI)
    critical_section_init_with_lock_num(&audio_buffer->cs, (uint)spin_lock_claim_unused(true));
    memset(audio_buffer->samples, 0, SAMPLES_BUFFER_SIZE);
    memset(audio_buffer->empty_samples, 0, SAMPLES_CHUNK_SIZE);
    audio_buffer->head = 0;
    audio_buffer->tail = 0;
    audio_buffer->size = 0;
}

static inline void __not_in_flash_func(audio_buffer_enqueue)(audio_buffer_t *audio_buffer, uint8_t sample) {
    if (audio_buffer->size < SAMPLES_BUFFER_SIZE) {
        audio_buffer->samples[audio_buffer->head++] = sample;
        if (audio_buffer->head >= SAMPLES_BUFFER_SIZE) {
            audio_buffer->head = 0;
        }
        critical_section_enter_blocking(&audio_buffer->cs);
        audio_buffer->size++;
        critical_section_exit(&audio_buffer->cs);
    }
}

static inline uint8_t *__not_in_flash_func(audio_buffer_dequeue)(audio_buffer_t *audio_buffer) {
    if (audio_buffer->size >= SAMPLES_CHUNK_SIZE) {
        uint8_t *samples = &audio_buffer->samples[audio_buffer->tail];
        audio_buffer->tail += SAMPLES_CHUNK_SIZE;
        if (audio_buffer->tail >= SAMPLES_BUFFER_SIZE) {
            audio_buffer->tail = 0;
        }
        critical_section_enter_blocking(&audio_buffer->cs);
        audio_buffer->size -= SAMPLES_CHUNK_SIZE;
        critical_section_exit(&audio_buffer->cs);
        return samples;
    } else {
        int p = audio_buffer->tail == 0 ? SAMPLES_BUFFER_SIZE : audio_buffer->tail;
        int c = audio_buffer->samples[p - 1];
        // Called from the DMA interrupt: no memset, which lives in flash (reported by
        // Neo6502Tangerine); eight explicit word stores (a loop could be turned back
        // into a memset call by GCC)
        const uint32_t w = (uint32_t)(uint8_t)c * 0x01010101u;
        uint32_t *e = (uint32_t *)audio_buffer->empty_samples;
        e[0] = w; e[1] = w; e[2] = w; e[3] = w; e[4] = w; e[5] = w; e[6] = w; e[7] = w;
        return audio_buffer->empty_samples;    
    }
}

static void __not_in_flash_func(audio_dma_irq_handler)() {
    dma_channel_set_read_addr(sample_dma_channel, audio_buffer_dequeue(&audio_buffer), false);
    dma_channel_set_read_addr(trigger_dma_channel, &single_sample_ptr, true);
    dma_channel_acknowledge_irq1(trigger_dma_channel);
}

// PWM clock divider in 1/16ths for sample_freq: a PWM period is 256 steps
// (wrap 255) and a sample lasts SAMPLE_REPETITION_RATE periods; rounded to the
// nearest 1/16 (the fractional divider has 4 bits)
static uint32_t audio_div16(uint32_t sample_freq) {
    const uint64_t f = clock_get_hz(clk_sys);
    const uint64_t den = 256ull * sample_freq * SAMPLE_REPETITION_RATE;
    uint32_t d16 = (uint32_t)((f * 16 + den / 2) / den);
    if (d16 < 16) d16 = 16;
    if (d16 > 255 * 16 + 15) d16 = 255 * 16 + 15;
    return d16;
}

uint32_t audio_pwm_rate_q8(uint16_t sample_freq) {
    const uint64_t f = clock_get_hz(clk_sys);
    const uint64_t den = (uint64_t)SAMPLE_REPETITION_RATE * audio_div16(sample_freq);
    return (uint32_t)((f * 16 + den / 2) / den);   // 256 * f / (256 * rep * d16 / 16): in 1/256 Hz
}

void audio_init(uint8_t audio_pin, uint16_t sample_freq) {
    gpio_set_function(audio_pin, GPIO_FUNC_PWM);

    int audio_pin_slice = pwm_gpio_to_slice_num(audio_pin);
    int audio_pin_channel = pwm_gpio_to_channel(audio_pin);

    const uint32_t d16 = audio_div16(sample_freq);

    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv_int_frac(&config, (uint8_t)(d16 >> 4), (uint8_t)(d16 & 15));
    pwm_config_set_wrap(&config, 255);
    pwm_init(audio_pin_slice, &config, true);

    pwm_dma_channel = dma_claim_unused_channel(true);
    trigger_dma_channel = dma_claim_unused_channel(true);
    sample_dma_channel = dma_claim_unused_channel(true);

    // Setup PWM DMA channel
    dma_channel_config pwm_dma_channel_config = dma_channel_get_default_config(pwm_dma_channel);
    // Transfer 32 bits at a time
    channel_config_set_transfer_data_size(&pwm_dma_channel_config, DMA_SIZE_32);
    // Read from a fixed location, always writes to the same address
    channel_config_set_read_increment(&pwm_dma_channel_config, false);
    channel_config_set_write_increment(&pwm_dma_channel_config, false);
    // Chain to sample DMA channel when done
    channel_config_set_chain_to(&pwm_dma_channel_config, sample_dma_channel);
    // Transfer on PWM cycle end
    channel_config_set_dreq(&pwm_dma_channel_config, DREQ_PWM_WRAP0 + audio_pin_slice);

    dma_channel_configure(pwm_dma_channel, &pwm_dma_channel_config,
                          // Write to PWM slice CC register
                          &pwm_hw->slice[audio_pin_slice].cc,
                          // Read from single_sample
                          &single_sample,
                          // Transfer once per desired sample repetition
                          SAMPLE_REPETITION_RATE,
                          // Don't start yet
                          false);

    // Setup trigger DMA channel
    dma_channel_config trigger_dma_channel_config = dma_channel_get_default_config(trigger_dma_channel);
    // Transfer 32-bits at a time
    channel_config_set_transfer_data_size(&trigger_dma_channel_config, DMA_SIZE_32);
    // Always read and write from and to the same address
    channel_config_set_read_increment(&trigger_dma_channel_config, false);
    channel_config_set_write_increment(&trigger_dma_channel_config, false);
    // Transfer on PWM cycle end
    channel_config_set_dreq(&trigger_dma_channel_config, DREQ_PWM_WRAP0 + audio_pin_slice);

    dma_channel_configure(trigger_dma_channel, &trigger_dma_channel_config,
                          // Write to PWM DMA channel read address trigger
                          &dma_hw->ch[pwm_dma_channel].al3_read_addr_trig,
                          // Read from location containing the address of single_sample
                          &single_sample_ptr,
                          // Need to trigger once for each audio sample but as the PWM DREQ is
                          // used need to multiply by sample repetition rate
                          SAMPLE_REPETITION_RATE * SAMPLES_CHUNK_SIZE,
                          // Don't start yet
                          false);

    // Fire interrupt when trigger DMA channel is done
    dma_channel_set_irq1_enabled(trigger_dma_channel, true);
    irq_set_exclusive_handler(DMA_IRQ_1, audio_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_1, true);

    // Setup sample DMA channel
    dma_channel_config sample_dma_channel_config = dma_channel_get_default_config(sample_dma_channel);
    // Transfer 8-bits at a time
    channel_config_set_transfer_data_size(&sample_dma_channel_config, DMA_SIZE_8);
    // Increment read address to go through audio buffer
    channel_config_set_read_increment(&sample_dma_channel_config, true);
    // Always write to the same address
    channel_config_set_write_increment(&sample_dma_channel_config, false);

    dma_channel_configure(sample_dma_channel, &sample_dma_channel_config,
                          // Write to single_sample
                          (char *)&single_sample + 2 * audio_pin_channel,
                          // Read from audio buffer
                          audio_buffer.samples,
                          // Only do one transfer (once per PWM DMA completion due to chaining)
                          1,
                          // Don't start yet
                          false);

    audio_buffer_init(&audio_buffer);

    // Kick things off with the trigger DMA channel
    dma_channel_start(trigger_dma_channel);
}

/*-- Volume -------------------------------------------------------------------*/

static hid_volume_t volume = {HID_VOLUME_MAX, false};
static uint16_t audio_gain = 256;   // In 256ths (hid_volume_gain)
static volatile uint32_t volume_serial;

void __not_in_flash_func(audio_push_sample)(const uint8_t sample) {
    audio_buffer_enqueue(&audio_buffer, (uint8_t)((sample * audio_gain) >> 8));
}

void audio_set_volume(uint8_t level, bool muted) {
    volume.level = level > HID_VOLUME_MAX ? HID_VOLUME_MAX : level;
    volume.muted = muted;
    audio_gain = hid_volume_gain(&volume);
}

uint8_t audio_volume_level(void) { return volume.level; }
bool audio_volume_muted(void) { return volume.muted; }
uint32_t audio_volume_serial(void) { return volume_serial; }

// Multimedia keys of the keyboard (hid_app.c)
void hid_media_key_down(uint8_t keys) {
    hid_volume_keys(&volume, keys);
    audio_gain = hid_volume_gain(&volume);
    if (keys & (HID_MEDIA_VOL_UP | HID_MEDIA_VOL_DOWN | HID_MEDIA_MUTE)) volume_serial++;
}
