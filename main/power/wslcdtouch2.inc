// Waveshare S3 Touch LCD 2 implementation
//
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/rtc_io.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_rom_gpio.h>
#include <esp_sleep.h>
#include <freertos/idf_additions.h>

#include "button_gpio.h"
#include "display.h"
#include "hal/adc_types.h"
#include "hal/gpio_types.h"
#include "input.h"
#include "iot_button.h"
#include "power.h"
#include "soc/gpio_num.h"
#include "tusb.h"

#define BATTERY_ADC_CHANNEL ADC_CHANNEL_4
#define BATTERY_ADC_ATTEN ADC_ATTEN_DB_12
#define BATTERY_EMA_ALPHA 0.3f // estimated moving average smoothing factor

#define LCD_BL_LEDC_TIMER LEDC_TIMER_0
#define LCD_BL_LEDC_MODE LEDC_LOW_SPEED_MODE

#define LCD_BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define LCD_BL_LEDC_DUTY_RES LEDC_TIMER_10_BIT
#define LCD_BL_LEDC_DUTY (1024)
#define LCD_BL_LEDC_FREQUENCY (10000)

static adc_oneshot_unit_handle_t adc1_handle = NULL;
static adc_cali_handle_t adc1_cali_chan0_handle = NULL;

static float ema_voltage = 0.0f;

static void button_shutdown(void* arg, void* ctx)
{
    power_backlight_off();
    while (gpio_get_level(GPIO_NUM_0) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    power_shutdown();
}

static esp_err_t boot_button_init(void)
{
    const button_config_t btn_cfg = {
        .long_press_time = CONFIG_BUTTON_LONG_PRESS_TIME_MS,
        .short_press_time = CONFIG_BUTTON_SHORT_PRESS_TIME_MS,
    };
    const button_gpio_config_t btn_gpio_cfg = {
        .gpio_num = 0,
        .active_level = 0,
    };
    button_handle_t btn_handle = NULL;
    // GPIO0 may still be latched low from the previous power_shutdown()
    gpio_hold_dis(GPIO_NUM_0);
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &btn_handle));
    ESP_ERROR_CHECK(iot_button_register_cb(btn_handle, BUTTON_LONG_PRESS_HOLD, NULL, button_shutdown, NULL));
    // No deep sleep wake source is armed: power_shutdown() parks GPIO0 low to
    // hold the display and touch controllers in reset, which is incompatible
    // with waking on that same pin.  The device wakes via the RST button.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    gpio_hold_en(GPIO_NUM_0);
    gpio_deep_sleep_hold_en();
    return ESP_OK;
}

static esp_err_t brightness_init(void)
{
    // A pad held via gpio_hold_en() stays latched across a deep-sleep wake, and
    // gpio_reset_pin() does not clear that.  power_shutdown() latches the
    // backlight low before sleeping, so release it before reconfiguring - else
    // the backlight stays dark after waking and the device looks bricked.
    gpio_hold_dis(CONFIG_DISPLAY_PIN_BL);
    gpio_reset_pin(CONFIG_DISPLAY_PIN_BL);
    gpio_set_direction(CONFIG_DISPLAY_PIN_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_DISPLAY_PIN_BL, 1);
    ledc_timer_config_t ledc_timer = { .speed_mode = LCD_BL_LEDC_MODE,
        .timer_num = LCD_BL_LEDC_TIMER,
        .duty_resolution = LCD_BL_LEDC_DUTY_RES,
        .freq_hz = LCD_BL_LEDC_FREQUENCY, // Set output frequency at 5 kHz
        .clk_cfg = LEDC_AUTO_CLK };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
    ledc_channel_config_t ledc_channel = { .speed_mode = LCD_BL_LEDC_MODE,
        .channel = LCD_BL_LEDC_CHANNEL,
        .timer_sel = LCD_BL_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = CONFIG_DISPLAY_PIN_BL,
        .duty = 0, // Set duty to 0%
        .hpoint = 0 };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
    return ESP_OK;
}

// Pads left floating during deep sleep can leak current, and a floating LCD CS
// can leave the panel partly active despite SLPIN.  Isolate what we can before
// sleeping; only GPIO0-21 are RTC-capable on the ESP32-S3, so the display SPI
// pins (38-45) and the touch i2c pins (47/48) cannot be isolated this way.
// Deliberately excluded: GPIO0 (ext0 wake source) and CONFIG_DISPLAY_PIN_BL /
// CONFIG_CAMERA_PWDN, which are driven and latched instead.
static const gpio_num_t DEEP_SLEEP_ISOLATE_PINS[] = {
    CONFIG_CAMERA_D0,
    CONFIG_CAMERA_D1,
    CONFIG_CAMERA_D2,
    CONFIG_CAMERA_D3,
    CONFIG_CAMERA_D4,
    CONFIG_CAMERA_D5,
    CONFIG_CAMERA_D6,
    CONFIG_CAMERA_D7,
    CONFIG_CAMERA_XCLK,
    CONFIG_CAMERA_PCLK,
    CONFIG_CAMERA_VSYNC,
    CONFIG_CAMERA_HREF,
    CONFIG_CAMERA_SDA,
    CONFIG_CAMERA_SCL,
    // ESP32-S3 internal USB PHY pads (D-/D+).  On battery the cable is out, but
    // the PHY can keep driving its bus pullup, which leaks.  Safe to isolate:
    // release_isolated_pins() runs in power_init(), which main() calls before
    // serial_init() brings TinyUSB up.
    GPIO_NUM_19,
    GPIO_NUM_20,
    // IMU_INT1 - the IMU is never configured, so nothing drives this usefully
    GPIO_NUM_3,
    // BAT_ADC.  The R19/R20 divider parks this pin near mid-rail, which makes
    // the digital input buffer conduct through both transistors.  Isolating it
    // removes that path; power_init() re-inits the ADC after releasing it.
    GPIO_NUM_5,
    // Not connected to anything on this board, so otherwise left floating
    GPIO_NUM_18,
};

// rtc_gpio_isolate() holds the pad and routes it to the RTC mux, and both
// survive a deep-sleep wake.  Undo that on every boot, otherwise the isolated
// peripheral (here, the whole camera bus) stays dead until a power cycle.
static void release_isolated_pins(void)
{
    for (size_t i = 0; i < sizeof(DEEP_SLEEP_ISOLATE_PINS) / sizeof(DEEP_SLEEP_ISOLATE_PINS[0]); ++i) {
        const gpio_num_t pin = DEEP_SLEEP_ISOLATE_PINS[i];
        if (pin >= 0 && rtc_gpio_is_valid_gpio(pin)) {
            rtc_gpio_hold_dis(pin);
            rtc_gpio_deinit(pin);
        }
    }
}

esp_err_t power_init(void)
{
    // Release any pads latched by the previous power_shutdown()
    release_isolated_pins();
    // Use the BOOT button as a sleep/wakeup button
    ESP_ERROR_CHECK(boot_button_init());
    // Initialise backlight brightness
    ESP_ERROR_CHECK(brightness_init());
    // Initialise the ADC to measure battery level
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));
    JADE_ASSERT(adc1_handle);
    // ADC Config
    adc_oneshot_chan_cfg_t config = {
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, BATTERY_ADC_CHANNEL, &config));
    // Use voltage calibration
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &adc1_cali_chan0_handle) != ESP_OK) {
        JADE_LOGW("ADC calibration not available, continuing without it");
        adc1_cali_chan0_handle = NULL;
    }
    return ESP_OK;
}

esp_err_t power_shutdown(void)
{
    // This board has no PMU and no power-latch, so the deepest 'off' state
    // available is ESP32 deep sleep.  Quiesce everything we can first so that
    // standby current is as low as the hardware allows.  Note this must happen
    // here rather than in the callers, as not every shutdown path turns the
    // backlight off beforehand.
    power_backlight_off();
    display_sleep();
    power_camera_off();

#ifdef CONFIG_DISPLAY_TOUCHSCREEN
    // Stop polling the touch controller so it can enter its own standby state.
    // Nothing to undo on wake - deep sleep exits via a cold boot, which
    // re-initialises the touchscreen from scratch.
    touchscreen_deinit();
#endif

    // The backlight is driven by LEDC and the camera PWDN by the camera driver,
    // both of which stop in deep sleep, leaving the pads floating.  Latch them
    // at their 'off' level for the duration of the sleep.  Global deep-sleep
    // pad hold is already enabled in boot_button_init().
    gpio_reset_pin(CONFIG_DISPLAY_PIN_BL);
    gpio_set_direction(CONFIG_DISPLAY_PIN_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_DISPLAY_PIN_BL, 0);
    gpio_hold_en(CONFIG_DISPLAY_PIN_BL);
#ifdef CONFIG_HAS_CAMERA
    gpio_hold_en(CONFIG_CAMERA_PWDN);
#endif

    for (size_t i = 0; i < sizeof(DEEP_SLEEP_ISOLATE_PINS) / sizeof(DEEP_SLEEP_ISOLATE_PINS[0]); ++i) {
        const gpio_num_t pin = DEEP_SLEEP_ISOLATE_PINS[i];
        if (pin >= 0 && rtc_gpio_is_valid_gpio(pin)) {
            rtc_gpio_isolate(pin);
        }
    }

    // GPIO0 is shared three ways on this board: the BOOT button, LCD_RST and
    // TP_RESET (R14 pulls the net up).  Driving it low parks both the ST7789
    // and the CST816D in hardware reset, which draws less than panel sleep-in
    // plus touch standby, and is a defined state rather than a mode a chip has
    // to be talked back out of.  Must be last - it resets the panel we have
    // just finished issuing commands to.
    gpio_hold_dis(GPIO_NUM_0);
    gpio_reset_pin(GPIO_NUM_0);
    gpio_set_direction(GPIO_NUM_0, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_0, 0);
    gpio_hold_en(GPIO_NUM_0);

    esp_deep_sleep_start();
    return ESP_OK;
}
esp_err_t power_screen_on(void) { return ESP_OK; }
esp_err_t power_screen_off(void) { return ESP_OK; }
esp_err_t power_backlight_on(uint8_t brightness)
{
    if (brightness < BACKLIGHT_MIN) {
        brightness = BACKLIGHT_MIN;
    } else if (brightness > BACKLIGHT_MAX) {
        brightness = BACKLIGHT_MAX;
    }
    uint32_t duty = (brightness * (LCD_BL_LEDC_DUTY - 1)) / 5;
    ESP_ERROR_CHECK(ledc_set_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL));
    return ESP_OK;
}
esp_err_t power_backlight_off(void)
{
    ESP_ERROR_CHECK(ledc_set_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL, 0));
    ESP_ERROR_CHECK(ledc_update_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL));

    return ESP_OK;
}
esp_err_t power_camera_on(void)
{
#ifdef CONFIG_HAS_CAMERA
    // Release the latch applied before deep sleep (see power_shutdown()), which
    // otherwise survives the wake and pins the sensor in power-down.
    gpio_hold_dis(CONFIG_CAMERA_PWDN);
    esp_rom_gpio_pad_select_gpio(CONFIG_CAMERA_PWDN);
    gpio_set_direction(CONFIG_CAMERA_PWDN, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_CAMERA_PWDN, 0);
#endif
    return ESP_OK;
}

esp_err_t power_camera_off(void)
{
#ifdef CONFIG_HAS_CAMERA
    // Assert PWDN to put the sensor into its power-down state
    esp_rom_gpio_pad_select_gpio(CONFIG_CAMERA_PWDN);
    gpio_set_direction(CONFIG_CAMERA_PWDN, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_CAMERA_PWDN, 1);
#endif
    return ESP_OK;
}

uint16_t power_get_vbat(void)
{
    JADE_ASSERT(adc1_handle);
    int raw_adc, voltage;
    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, BATTERY_ADC_CHANNEL, &raw_adc));
    if (adc1_cali_chan0_handle) {
        adc_cali_raw_to_voltage(adc1_cali_chan0_handle, raw_adc, &voltage);
    } else {
        voltage = (raw_adc * 3300) / 4095;
    }
    // Apply Estimated Moving Average (EMA)
    const float ema_v = (ema_voltage > 0) ? ema_voltage : (float)voltage;
    ema_voltage = (BATTERY_EMA_ALPHA * voltage) + ((1.0f - BATTERY_EMA_ALPHA) * ema_v);
    return (uint16_t)ema_voltage;
}

uint8_t power_get_battery_status(void)
{
    const uint16_t vbat = power_get_vbat() * 3; // apply voltage divider
    if (vbat > 3800) {
        return 5;
    } else if (vbat > 3600) {
        return 4;
    } else if (vbat > 3400) {
        return 3;
    } else if (vbat > 3200) {
        return 2;
    } else if (vbat > 3000) {
        return 1;
    }
    return 0;
}

bool power_get_battery_charging(void)
{
    // The charging IC STAT pin is not connected to GPIO.
    // Return USB status as a proxy for charging (Dummy status)
    return usb_is_powered();
}

uint16_t power_get_ibat_charge(void) { return 0; }
uint16_t power_get_ibat_discharge(void) { return 0; }
uint16_t power_get_vusb(void) { return 0; }
uint16_t power_get_iusb(void) { return 0; }
uint16_t power_get_temp(void) { return 0; }

void disable_usb_host(void) {}
void enable_usb_host(void) {}

bool usb_is_powered(void)
{
    // Check if USB is mounted and not suspended (active connection)
    if (tud_mounted() && !tud_suspended()) {
        return true;
    }
    // Fallback: If voltage is near zero (<1V) but chip is on,
    // we must be running on USB power without a battery.
    if (power_get_vbat() < 1000) {
        return true;
    }
    return false;
}
