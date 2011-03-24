#ifndef _SOUND_H_
#define _SOUND_H_

#define SOUND_IBUFSZ 128

#define SOUND_OBUFSZ 128


typedef int (*sound_cb)(unsigned char * buffer);

void sound_new_sample(unsigned int sample);

void sound_init(void);
void sound_poweroff(void);

void sound_playback_enable(sound_cb cb);
void sound_playback_disable(void);

void sound_set_mic_stat(int enabled);

extern unsigned char sound_mic_max;
extern unsigned char sound_mic_min;

//callback 
void sound_mic_buffer(unsigned char * b);

#endif

