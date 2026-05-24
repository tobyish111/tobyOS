/* media_session.h -- system-wide media key handling + now-playing info. */

#ifndef TOBYOS_MEDIA_SESSION_H
#define TOBYOS_MEDIA_SESSION_H

#include <tobyos/types.h>

struct media_session {
    int  pid;
    char title[64];
    char artist[32];
    int  playing;           /* 0=paused, 1=playing */
    int  duration_ms;
    int  position_ms;
};

void media_session_init(void);
int  media_session_register(int pid);
void media_session_update(int pid, const char *title, const char *artist,
                          int duration_ms, int pos_ms, int playing);
void media_session_unregister(int pid);
int  media_session_get_active(struct media_session *out);
void media_session_play_pause(void);
void media_session_next(void);
void media_session_prev(void);

#endif /* TOBYOS_MEDIA_SESSION_H */
