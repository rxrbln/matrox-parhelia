/* SPDX-License-Identifier: MIT */
#ifndef PARHELIA_PUBLIC_H
#define PARHELIA_PUBLIC_H
struct pipe_screen;
struct pipe_screen_config;
struct pipe_screen *parhelia_screen_create(int fd,
                                            const struct pipe_screen_config *config);
#endif
