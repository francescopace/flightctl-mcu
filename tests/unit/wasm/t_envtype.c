/* env.clock_ms with a type the device does not have: refused. */
__attribute__((import_module("env"), import_name("clock_ms"))) float clock_ms(void);

__attribute__((export_name("_start"))) float start(void)
{
    return clock_ms();
}
