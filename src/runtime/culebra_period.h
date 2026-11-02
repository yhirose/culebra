// The output device's period, compiled into culebra_raudio.c with raudio
// (whose AUDIO_DEVICE_PERIOD_SIZE_IN_FRAMES reads the one asked for here).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// The period to ask for when the device opens, in seconds; 0 for
// miniaudio's own (10 ms).
void culebra_period_ask(double seconds);
// The period the open device runs at, in seconds; 0 when it is not open.
double culebra_period(void);

#ifdef __cplusplus
}
#endif
