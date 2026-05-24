/* system_sounds.h -- play short audio cues for system events. */

#ifndef TOBYOS_SYSTEM_SOUNDS_H
#define TOBYOS_SYSTEM_SOUNDS_H

enum sys_sound {
    SND_STARTUP,
    SND_SHUTDOWN,
    SND_NOTIFICATION,
    SND_ERROR,
    SND_WARNING,
    SND_CLICK,
    SND_CONNECT,
    SND_DISCONNECT,
    SND_COUNT
};

void system_sounds_init(void);
void system_sound_play(enum sys_sound snd);
void system_sound_set_enabled(int enabled);
int  system_sound_is_enabled(void);

#endif /* TOBYOS_SYSTEM_SOUNDS_H */
