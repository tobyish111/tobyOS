/* toby/media.h -- Media playback pipeline for tobyOS userland.
 *
 * Ties together audio/video decode, HTTP fetching, A/V sync,
 * and playback state management.
 */

#ifndef TOBY_MEDIA_H
#define TOBY_MEDIA_H

#include <stdint.h>
#include <stddef.h>
#include <toby/video_decode.h>

#ifdef __cplusplus
extern "C" {
#endif

enum media_state {
    MEDIA_STOPPED,
    MEDIA_PLAYING,
    MEDIA_PAUSED,
    MEDIA_BUFFERING
};

struct media_player;

struct media_player *media_player_create(void);
int                  media_player_open(struct media_player *mp, const char *url);
void                 media_player_play(struct media_player *mp);
void                 media_player_pause(struct media_player *mp);
void                 media_player_seek(struct media_player *mp, int64_t position_ms);
int64_t              media_player_position(struct media_player *mp);
int64_t              media_player_duration(struct media_player *mp);
enum media_state     media_player_state(struct media_player *mp);
struct video_frame  *media_player_get_frame(struct media_player *mp);
void                 media_player_destroy(struct media_player *mp);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_MEDIA_H */
