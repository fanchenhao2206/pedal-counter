// Tim Fan <hourlilies@icloud.com>
//
// Note: Use esp32-2.0.18 (old) Arduino core by Espressif when compiling for esp32 board
//
// It seems like esp32-3.3.10 (latest) causes crash when running the ssd1306-1.8.8 (latest) 
// ssd1306 OLED display driver by Alexey Dynda, due to the usage of a legacy ADC driver(?)
//
// https://lexus2k.github.io/ssd1306/index.html
// https://docs.espressif.com/projects/arduino-esp32/en/latest/

#include "ssd1306.h"
#include <Preferences.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

#define FONT_DEFAULT ssd1306xled_font6x8
#define FONT_SPECIAL ssd1306xled_font8x16

#define MAX_LAP 99
#define MAX_COUNT UINT16_MAX

#define NAMESPACE_MAIN "main"

typedef struct _button {
    int pin; // GPIO pin number the hardware button is connected to
    bool curr; // On current frame, value of digitalRead(button.pin) == LOW
    bool prev; // On previous frame, value of digitalRead(button.pin) == LOW
    int val; // Value customised to button's function (e.g. mode.val == currentMode)
    int total; // Exclusive to lap button; total laps so far versus selected val lap 
    int startMillis; // Time button was pressed down; to count how long have been pressed down
    int heldMillis; // How long button has been pressed down; updated on each loop
    char display[10]; // The 10 characters to show on each second held down; repeat 10th after 10s
} button;

typedef const uint8_t *font_t;

Preferences preferences;

int counts[100] = { 0 };

button led = {
    .pin = 26, // GPIO_26
    .curr = false // Use button struct to track if LED should be on
};

button mode = {
    .pin = 27, // GPIO_27
    .display = { 'M', '.', 'S', '2', '1', '#', '#', '#', '#', '#' }
    // On second  1 ,  0 ,  0 ,  0 ,  0 ,  1 ,  1 ,  1 ,  1 ,  1
    // 1 if do something, 0 if do nothing, when let go of button at this time
    // So, change mode by holding button for 1 second or less
    // And, save data if hold button for 5 seconds or more
    // If held between 1 and 5 seconds, do nothing; be very sure before saving
}; 

button inc = {
    .pin = 33, // GPIO_33,
    .display = { '+', '>', '=', '=', '=', '=', '=', '=', '=', '=' }
    // On second  1 ,  1 ,  1 ,  1 ,  1 ,  1 ,  1 ,  1 ,  1 ,  1
    // 1 if do something, 0 if do nothing, when let go of button at this time
    // So, increase count by holding button for 1 second or less
    // And, goto next lap if one exists when hold for more than 1 second
};

button dec = {
    .pin = 15, // GPIO_15
    .display = { '-', '<', '=', '=', '=', '=', '=', '=', '=', '=' }
    // On second  1 ,  1 ,  1 ,  1 ,  1 ,  1 ,  1 ,  1 ,  1 ,  1
    // 1 if do something, 0 if do nothing, when let go of button at this time
    // So, decrease count by holding button for 1 second or less
    // And, goto prev lap if one exists when hold for more than 1 second
};

button lap = {
    .pin = 32, // GPIO_32
    .val = 1, // Start from the 1st lap
    .total = 1, // Of 1 laps total
    .display = { 'N', '.', 'L', '2', '1', '#', '#', '#', '#', '#' }
    // On second  1 ,  0 ,  0 ,  0 ,  0 ,  1 ,  1 ,  1 ,  1 ,  1
    // 1 if do something, 0 if do nothing, when let go of button at this time
    // So, make new row (at end) by holding button for 1 second or less
    // And, load data from flash if hold button for 5 seconds or more
    // If held between 1 and 5 seconds, do nothing; be very sure before loading
};

button pedal = {
    .pin = 14, // GPIO_14
    .display = { '+', '+', '+', '+', '+', '+', '+', '+', '+', '+' }
    // On second  1 ,  1 ,  1 ,  1 ,  1 ,  1 ,  1 ,  1 ,  1 ,  1
    // 1 if do something, 0 if do nothing, when let go of button at this time
    // So, increase count by holding button for more than 0 seconds
};

void printCount(int y, font_t font, EFontStyle style, EFontSize size);
void printLap(int y, font_t font, EFontStyle style, EFontSize size, int left_padding);
void printMode(int y, font_t font, EFontStyle style, EFontSize size, int right_padding);
void printStat(int y, font_t font, EFontStyle style, EFontSize size);

static void getFontDimensions(int *width, int *height, font_t font, EFontSize size);

void setup()
{
    // Initialise i2c driver, and then initialise OLED driver (which requires working i2c)
    // Uses ESP32 Feather SCL/SDA pins via pins_arduino.h in arduino-esp32/variants/feather_esp32/ included on compilation by Arduino IDE
    // And uses default SSD1306 display i2c slave address, 0x3C as specified at piico.dev/p14, the SSD1306 display used for project
    ssd1306_128x64_i2c_init();

    // Clear screen, set font, and send i2c commands to print to display
    // Behind the scenes likely invokes the i2c_write commands through:
    // ... ssd1306/src/intf/i2c/ssd1306_i2c.c -> ssd1306_platform_i2cInit();
    // ... ssd1306/src/ssd1306_hal/arduino/platform.cpp -> s_i2c = &Wire; s_i2c->begin(); ssd1306_intf.send = ssd1306_i2cSendByte_Wire;
    // ... arduino-esp32/libraries/Wire/src/Wire.cpp -> TwoWire Wire = TwoWire(0);
    // maybe...
    ssd1306_fillScreen(0x00);
    ssd1306_setFixedFont(FONT_DEFAULT);

    // Initialise ESP32 Feather Serial
    Serial.begin(9600);

    // Do pin assignments
    pinMode(mode.pin, INPUT);
    pinMode(inc.pin, INPUT);
    pinMode(dec.pin, INPUT);
    pinMode(lap.pin, INPUT);
    pinMode(pedal.pin, INPUT);
    pinMode(led.pin, OUTPUT);
}

static void getFontDimensions(int *width, int *height, font_t font, EFontSize size)
{
    int w = 0, h = 0;
    if (font == ssd1306xled_font8x16) {
        w = 8;
        h = 16;
    } else if (font == ssd1306xled_font6x8) {
        w = 6;
        h = 8;
    } else {
        // Bad font, angry error...
        assert(false);
    }
    switch (size) {
        case FONT_SIZE_2X:
            w *= 2;
            h *= 2;
            break;
        case FONT_SIZE_4X:
            w *= 4; 
            h *= 4;
            break;
        case FONT_SIZE_8X:
            w *= 8;
            h *= 8;
            break;
        default:
            break;
    }
    *width = w;
    *height = h;
    return;
}

void printCount(int y, font_t font, EFontStyle style, EFontSize size)
{
    // To print
    char text[16];
    snprintf(text, sizeof(text), "%d", counts[lap.val]);

    // Print it, in middle of row given by y
    int x, font_w, font_h;
    getFontDimensions(&font_w, &font_h, font, size);
    x = (SCREEN_WIDTH - (font_w * strlen(text))) / 2;
    ssd1306_setFixedFont(font);
    ssd1306_printFixedN(x, y, text, style, size);
}

void printLap(int y, font_t font, EFontStyle style, EFontSize size, int left_padding)
{
    // To print
    char text[16];
    snprintf(text, sizeof(text), "%d/%d", lap.val, lap.total);

    // Print it, on left of row given by y, with extra padding
    int x;
    x = left_padding;
    ssd1306_setFixedFont(font);
    ssd1306_printFixedN(x, y, text, style, size);
}

void printMode(int y, font_t font, EFontStyle style, EFontSize size, int right_padding)
{
    // To print
    const char *text = "125x";
    int x, char_w, char_h;
    getFontDimensions(&char_w, &char_h, font, size);
    x = SCREEN_WIDTH - (char_w * strlen(text)) - right_padding;
    ssd1306_setFixedFont(font);

    // Print it, carefully
    for (size_t i = 0; i < strlen(text); i++) {
        char single[2] = { text[i], '\0' };
        int char_x = x + (i * char_w);
        if ((int)i != mode.val) {
            ssd1306_printFixedN(char_x, y, single, style, size);
        }
    }
    for (size_t i = 0; i < strlen(text); i++) {
        char single[2] = { text[i], '\0' };
        int char_x = x + (i * char_w);
        if ((int)i == mode.val) {
            ssd1306_fillRect(char_x, y, char_x + char_w, y + char_h);
            ssd1306_negativeMode();
            ssd1306_printFixedN(char_x, y, single, style, size);
            ssd1306_positiveMode();
        }
    }
}

void printStat(int y, font_t font, EFontStyle style, EFontSize size)
{
    // To print
    char text[16];

    if (inc.curr) snprintf(text, sizeof(text), "%c", inc.display[inc.heldMillis / 1000]);
    else if (pedal.curr) snprintf(text, sizeof(text), "%c", pedal.display[pedal.heldMillis / 1000]);
    else if (dec.curr) snprintf(text, sizeof(text), "%c", dec.display[dec.heldMillis / 1000]);
    else if (mode.curr) snprintf(text, sizeof(text), "%c", mode.display[mode.heldMillis / 1000]);
    else if (lap.curr) snprintf(text, sizeof(text), "%c", lap.display[lap.heldMillis / 1000]);

    else snprintf(text, sizeof(text), " ");

    // Print it, in middle of row given by y
    int x, font_w, font_h;
    getFontDimensions(&font_w, &font_h, font, size);
    x = (SCREEN_WIDTH - (font_w * strlen(text))) / 2;
    ssd1306_setFixedFont(font);
    ssd1306_printFixedN(x, y, text, style, size);
}

void loop()
{
    mode.curr = digitalRead(mode.pin) == LOW;
    inc.curr = digitalRead(inc.pin) == LOW;
    pedal.curr = digitalRead(pedal.pin) == LOW;
    dec.curr = digitalRead(dec.pin) == LOW;
    lap.curr = digitalRead(lap.pin) == LOW;

    int modes[4] = { 1, 2, 5, 10 };

    // Just pressed the button! Mark start to count time
    if (mode.curr && !mode.prev) mode.startMillis = millis();
    if (inc.curr && !inc.prev) inc.startMillis = millis();
    if (pedal.curr && !pedal.prev) pedal.startMillis = millis();
    if (dec.curr && !dec.prev) dec.startMillis = millis();
    if (lap.curr && !lap.prev) lap.startMillis = millis();

    // Just released the button! Do action if held for right time
    bool refresh = false;
    if (!mode.curr && mode.prev) { 
        if (mode.heldMillis <= 1000) {
            mode.val += 1; 
            mode.val %= 4; 
        } else if (mode.heldMillis >= 5000) {
            // Saving data to NVS
            preferences.begin(NAMESPACE_MAIN, false);
            char key[6];
            for (int i = 1; i <= lap.total; i++) {
                snprintf(key, sizeof(key), "k%d", i); // Key starts from "k1" to "k99"
                preferences.putUShort(key, (uint16_t)counts[i]);
            }
            preferences.putInt("n", lap.total); // Store nLaps in "n"
            preferences.end();
        }
    }
    if (!inc.curr && inc.prev) {
        if (inc.heldMillis <= 1000) {
            // Increase count in current lap based on selected mode
            counts[lap.val] += modes[mode.val];
        } else if (inc.heldMillis <= 2000) {
            // Goto next lap
            lap.val += 1;
            if (lap.val > lap.total) lap.val = lap.total;
            refresh = true;
        } else {
            // Goto last lap
            lap.val = lap.total;
            refresh = true;
        }
    }
    if (!pedal.curr && pedal.prev) {
        // Increase count in current lap based on selected mode
        counts[lap.val] += modes[mode.val];
    }
    if (!dec.curr && dec.prev) {
        if (dec.heldMillis <= 1000) {
            // Decrease count in current lap based on selected mode
            counts[lap.val] -= modes[mode.val];
        } else if (dec.heldMillis <= 2000) {
            // Goto previous lap
            lap.val -= 1;
            if (lap.val < 1) lap.val = 1;
        } else {
            // Goto first lap
            lap.val = 1;
        }
        refresh = true;
    }
    if (!lap.curr && lap.prev) {
        if (lap.heldMillis <= 1000) {
            // Add new lap and advance to it if are 
            // currently on the last lap in project
            if (lap.total < MAX_LAP) {
                if (lap.val == lap.total) lap.val += 1;
                lap.total += 1;
            }
        } else if (lap.heldMillis >= 5000) {
            // Loading save data from NVS
            preferences.begin(NAMESPACE_MAIN, false);
            lap.total = preferences.getInt("n"); // Get nLaps from preferences
            lap.val = lap.total; // Jump to last lap after finish loading
            char key[6];
            for (int i = 1; i <= lap.total; i++) {
                snprintf(key, sizeof(key), "k%d", i); // Key starts from "k1" to "k99"
                counts[i] = preferences.getUShort(key);
            }
            preferences.end();
        }
        refresh = true;
    }

    // Check out of bounds for count
    if (counts[lap.val] < 0) counts[lap.val] = 0;
    if (counts[lap.val] > MAX_COUNT) counts[lap.val] = MAX_COUNT;

    // Counting how long each button has been held for till now
    mode.heldMillis = mode.curr ? millis() - mode.startMillis: 0;
    inc.heldMillis = inc.curr ? millis() - inc.startMillis: 0;
    pedal.heldMillis = pedal.curr ? millis() - pedal.startMillis: 0;
    dec.heldMillis = dec.curr ? millis() - dec.startMillis: 0;
    lap.heldMillis = lap.curr ? millis() - lap.startMillis: 0;

    // Light up LED to indicate if button held for long enough
    if ((mode.curr && (mode.heldMillis <= 1000 || mode.heldMillis >= 5000)) ||
        (lap.curr && (lap.heldMillis <= 1000 || lap.heldMillis >= 5000)) || 
        (inc.curr || dec.curr || pedal.curr)) {
        digitalWrite(led.pin, HIGH);
    } else {
        digitalWrite(led.pin, LOW);
    }

    // Prepare for next frame
    mode.prev = mode.curr;
    inc.prev = inc.curr;
    pedal.prev = pedal.curr;
    dec.prev = dec.curr;
    lap.prev = lap.curr;

    // Refresh only on frames like lap/dec that can cause ghosting; not every frame
    if (refresh) ssd1306_fillScreen(0x00);

    // Print numbers and indicator onto screen
    printCount(12, FONT_DEFAULT, STYLE_NORMAL, FONT_SIZE_4X);  
    printMode(SCREEN_HEIGHT - 16, FONT_DEFAULT, STYLE_NORMAL, FONT_SIZE_2X, 0);
    printStat(SCREEN_HEIGHT - 16, FONT_DEFAULT, STYLE_NORMAL, FONT_SIZE_2X);
    printLap(SCREEN_HEIGHT - 16, FONT_DEFAULT, STYLE_NORMAL, FONT_SIZE_2X, 0);

    // Advance to next frame
    delay(100);
}