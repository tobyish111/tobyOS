/* audio.c -- Userland audio API for tobyOS.
 *
 * Wraps the audio engine syscalls for easy use from applications.
 */

#include "libtoby_internal.h"

int toby_audio_open(uint32_t sample_rate, uint8_t channels, uint8_t format) {
    return (int)toby_sc3(ABI_SYS_AUDIO_OPEN,
                         (long)sample_rate,
                         (long)channels,
                         (long)format);
}

long toby_audio_write(int fd, const void *samples, size_t count) {
    return toby_sc3(ABI_SYS_AUDIO_WRITE,
                    (long)fd,
                    (long)(uintptr_t)samples,
                    (long)count);
}

int toby_audio_close(int fd) {
    return (int)toby_sc1(ABI_SYS_AUDIO_CLOSE, (long)fd);
}

int toby_audio_set_volume(int fd, int volume) {
    return (int)toby_sc2(ABI_SYS_AUDIO_VOLUME, (long)fd, (long)volume);
}
