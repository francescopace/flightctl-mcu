/* The module "arduino" (core/src/fc_arduino.c), as wasm3-arduino's examples
 * import it, run once through FC_ENTRY. With FC_ALLOW "relay1 led0": pin 0
 * is relay1 (a GPIO), pin 1 led0.
 *   run()   returns a bit per check that holds (63 when all do)
 *   hold()  drives relay1 high and waits in delay() until it is stopped */
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
    pinMode(0, 1);
    digitalWrite(0, 1);
    pinMode(getPinLED(), 1);
    digitalWrite(getPinLED(), 1);
    r |= 16;
    pinMode(7, 1);      /* no 8th name: logged once */
    digitalWrite(7, 1); /* not opened: logged, and nothing happens */
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
