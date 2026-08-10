/* snd_pcm.h -- /dev/snd: the Linux ALSA PCM ABI over the HDA driver.
 * See src/snd_pcm.c for scope and method. */
#ifndef TOBYOS_SND_PCM_H
#define TOBYOS_SND_PCM_H

#include <tobyos/types.h>

struct file;

/* True once an HDA controller with a codec is bound. When false, opening
 * any node under /dev/snd returns ENOENT -- exactly what a Linux box with
 * no sound card does, so alsa-lib reports "no such device" rather than
 * failing somewhere deeper and stranger. */
bool lxsnd_available(void);

/* open("/dev/snd/controlC0" | "/dev/snd/pcmC0D0p") -> FILE_KIND_SND.
 * `node` is the path after "/dev/", i.e. it starts with "snd/". */
struct file *lxsnd_open(const char *node);
void lxsnd_close(struct file *f);

long lxsnd_ioctl(struct file *f, unsigned long req, unsigned long arg);

/* Writable = the device ring has at least avail_min frames free. */
bool lxsnd_poll_writable(struct file *f);

#endif /* TOBYOS_SND_PCM_H */
