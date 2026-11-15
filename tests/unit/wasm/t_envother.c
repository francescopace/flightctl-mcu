/* A function from env that the device does not have: refused. */
__attribute__((import_module("env"), import_name("memcpy"))) void *memcpy_(void *d, const void *s, unsigned long n);

__attribute__((export_name("_start"))) void start(void)
{
    char a[4], b[4] = "abc";
    memcpy_(a, b, 4);
}
