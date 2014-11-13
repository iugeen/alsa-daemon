alsa-daemon
===========

This daemon will handle the a2dp and hfp audio streaming connections and can replace pulseaudio functionality in the system.

Update :
- sbc 1.2 library is included

Run steps:

- stop pulseaudio daemon

SINK :
- ./a2dp-alsa --sink

SOURCE :

  connect source 1 with sink 8 : connect 1 8 

- ./a2dp-alsa --source

- aplay -D MEDIA_playback_0 onclassical_demo_roccato_anonymous-roccato_riflessi_small-version.wav
