/* K4: video results, from one JSON provider to start with.
 *
 * The card needs the same five things from every provider: a title, the watch
 * page, a thumbnail, a duration and the name of the platform. Nothing embeds a
 * player: an <iframe> would make the reader's browser contact the platform,
 * which is the one rule the whole design keeps. */
#ifndef VIDEOSCRAPING_H
#define VIDEOSCRAPING_H

#include <time.h>

enum { VIDEO_RESULT_FIELDS = 6 };

typedef struct {
  char title[256];
  char watch_url[512];
  char thumb_url[512]; /* absolute; the page fetches it through /image-proxy */
  int duration_s;      /* 0 when the provider did not say */
  char channel[128];
  char platform[32];   /* "Dailymotion" - shown on the card, never a logo */
} VideoResult;

/* Fills *out with a malloc'd array (caller frees with free_video_results) and
   sets *count. Returns 0 when at least one provider was asked, 1 when every
   video provider is switched off - the route turns that into the honest "no
   providers enabled" page rather than "no results". */
int scrape_videos(const char *query, VideoResult **out, int *count);
void free_video_results(VideoResult *results, int count);

#endif
