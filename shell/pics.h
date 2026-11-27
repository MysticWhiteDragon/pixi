/* Decoding pictures on a worker thread, so scrolling never waits on a JPEG. */
#ifndef PIXI_PICS_H
#define PIXI_PICS_H

void pics_start(void);
void pics_stop(void);

/* Ask for a picture scaled to fit max_dim pixels. Asking again for something
 * already queued only makes it more urgent: the newest request is decoded first. */
void pics_request(const char *path, int max_dim);

/* Take one finished picture, if there is one. rgba is 4 bytes a pixel, top row
 * first, and belongs to the caller (free it). A picture that couldn't be read
 * comes back with rgba == NULL. Returns 0 when nothing is ready. */
int pics_take(char *path, int path_cap, int *max_dim, int *w, int *h, unsigned char **rgba);

#endif
