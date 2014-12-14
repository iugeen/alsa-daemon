#include "util.h"


void run_sink_A2DP(io_thread_tcb_s *data, audio *cardName);    // phone -> alsa
void run_source_A2DP(io_thread_tcb_s *data);  // alsa  -> headset

void parseConfigFile(audio *audioCards);
audio *audioCards;
