#ifndef _PLAYBACK_H_
#define _PLAYBACK_H_

#define SOUND_DISABLE	(-1) /* Special case: immediatly stop playback */

#define SOUND_POWERON 	(-2)
#define SOUND_POWEROFF 	(-3)
#define SOUND_BUTTON	(-4)


// Negative: system sound
// Positive: user sound
void play_sound(int number);

void play_sound_block(int number);


#endif