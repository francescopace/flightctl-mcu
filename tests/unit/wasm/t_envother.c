/* A function from env that the device does not have: refused. malloc is in
 * WAMR's libc-builtin, but an app has no heap. */
__attribute__((import_module("env"), import_name("malloc"))) void *malloc_(unsigned long n);

__attribute__((export_name("_start"))) void start(void)
{
    char *p = malloc_(4);
    p[0] = 1;
}
