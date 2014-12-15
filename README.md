alsa-daemon
===========

This daemon will handle the a2dp audio streaming connections and can replace pulseaudio functionality in the system.

Update :
- sbc 1.3 library is included

Mandatory steps:
- stop pulseaudio daemon
- create configuration file (pcm.conf)

e.g: sink=A2dpSink0
     source=A2dpSource0 

Run daemon:
- ./a2dp-alsa

SINK :
- connect device
- connect sink [4 | 16 | 17] with source 1 

SOURCE :
- connect headset
- connect source 1 with sink [8 | 9 |10] 
- aplay -D MEDIA_playback_0 onclassical_demo_roccato_anonymous-roccato_riflessi_small-version.wav
