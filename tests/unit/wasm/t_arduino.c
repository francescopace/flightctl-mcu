/* The module "arduino" (core/src/fc_arduino.c), as wasm3-arduino's examples
 * import it, run once through FC_ENTRY. With FC_ALLOW "relay1 led0": pin 0
 * is relay1 (a GPIO), pin 1 led0.
 *   run()     returns a bit per check that holds (63 when all do)
 *   hold()    drives relay1 high and waits in delay() until it is stopped
 *   delays()  delay(0), delay(1), delay(105): 13 waits of a tick */
#include <stdint.h>

#define A(n) __attribute__((import_module("arduino"), import_name(#n)))
A(millis) uint32_t millis(void);
A(delay) void delay(uint32_t ms);
A(pinMode) void pinMode(uint32_t pin, uint32_t mode);
A(digitalWrite) void digitalWrite(uint32_t pin, uint32_t value);
A(getPinLED) uint32_t getPinLED(void);
A(getGreeting) void getGreeting(char *buf, uint32_t len);
A(print) void print(const char *s, uint32_t len);

static char g[64];

__attribute__((export_name("run"))) int32_t run(void)
{
    int32_t r = 0;
    r |= getPinLED() == 1 ? 1 : 0;
    uint32_t t0 = millis();
    delay(250); /* longer than one of fc's waits */
    r |= millis() - t0 >= 250 ? 2 : 0;
    getGreeting(g, sizeof(g));
    uint32_t n = 0;
    while (((volatile char *)g)[n]) { /* not strlen, which this module has not */
        n++;
    }
    r |= n == 25 ? 4 : 0;
    print("greeting: ", 10);
    print(g, n);
    print("\n", 1);
    getGreeting(g, 6);
    r |= g[4] == 'o' && g[5] == '\0' ? 8 : 0; /* cut, and ended */
    digitalWrite(0, 1); /* before pinMode: logged once */
    digitalWrite(0, 1);
    pinMode(0, 1);
    digitalWrite(0, 1);
    pinMode(getPinLED(), 1);
    digitalWrite(getPinLED(), 1);
    r |= 16;
    for (int i = 0; i < 3; i++) { /* logged once per pin, as a blink would flood the log */
        pinMode(7, 1);      /* no 8th name */
        digitalWrite(7, 1); /* not opened: nothing happens */
        pinMode(16, 1);     /* getPinLED() without led0, and any pin past 15 */
        digitalWrite(17, 1);
    }
    r |= 32;
    return r;
}

__attribute__((export_name("hold"))) void hold(void)
{
    pinMode(0, 1);
    digitalWrite(0, 1);
    for (;;) {
        delay(1000);
    }
}

__attribute__((export_name("delays"))) void delays(void)
{
    delay(0);
    delay(1);
    delay(105);
}
