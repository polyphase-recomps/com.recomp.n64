/*
 * Functions the wasm guest imports from its host (module "env"; implemented in n64w_host.c).
 * Pointers are guest addresses; strings and buffers are passed as bytes.
 */
#ifndef N64W_IMPORTS_H
#define N64W_IMPORTS_H

#define N64W_IMPORT(name) __attribute__((import_module("env"), import_name(#name)))

N64W_IMPORT(n64w_log) void n64w_log(const char *line);
N64W_IMPORT(n64w_fatal) void n64w_fatal(const char *line);
N64W_IMPORT(n64w_faulted) int n64w_faulted(void);
N64W_IMPORT(n64w_env_int) int n64w_env_int(const char *name);

N64W_IMPORT(n64w_file_read) unsigned int n64w_file_read(const char *path, void *data, unsigned int size);
N64W_IMPORT(n64w_file_write) int n64w_file_write(const char *path, const void *data, unsigned int size);

N64W_IMPORT(n64w_rom_loaded) int n64w_rom_loaded(void);
N64W_IMPORT(n64w_rom_read) void n64w_rom_read(unsigned int offset, void *dst, unsigned int size);
N64W_IMPORT(n64w_rom_size) unsigned int n64w_rom_size(void);

/* entry: guest function (table index) called as entry(arg) through n64w_coro_entry;
 * stack_top: top of the coroutine's part of the module stack */
N64W_IMPORT(n64w_coro_create) unsigned int n64w_coro_create(unsigned int entry, unsigned int arg, unsigned int stack_top);
N64W_IMPORT(n64w_coro_destroy) void n64w_coro_destroy(unsigned int coro);
N64W_IMPORT(n64w_coro_resume) void n64w_coro_resume(unsigned int coro);
N64W_IMPORT(n64w_coro_yield) void n64w_coro_yield(void);
N64W_IMPORT(n64w_coro_finished) int n64w_coro_finished(unsigned int coro);

#endif /* N64W_IMPORTS_H */
