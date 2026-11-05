#!/bin/sh
#
# Call audio on the PinePhone's codec (modem.conf's audio_script=): phone-ui runs
# `call-audio.sh earpiece|speaker|off` as a call is dialed or answered, the loudspeaker
# is turned on or off, and the call ends.
#   earpiece  the modem's voice (its PCM on the codec's AIF2, mono at 8 kHz) to the
#             earpiece, the main microphone (MIC1) to the modem
#   speaker   the same through the loudspeaker (line out, to the amplifier)
#   off       the modem's path and the microphone off, the earpiece and line out muted
# The digital paths at 0 dB: the DAC's -13.5 dB of music playback made calls quiet
# (phone-ui sets the DAC back for music after `off`).
#

set -e

c() {
	amixer -q -c 0 cset name="$1" "$2"
}

route() {
	c 'AIF2 DAC Stereo Playback Route' 'Mix Mono'
	c 'AIF2 DAC Playback Volume' 160
	c 'AIF2 Digital DAC Playback Switch' on
	c 'DAC Playback Volume' 160
	c 'Mic1 Capture Switch' on
	c 'ADC Capture Volume' 160
	c 'AIF2 ADC Mixer ADC Capture Switch' on
	c 'AIF2 ADC Stereo Capture Route' 'Mix Mono'
	c 'AIF2 ADC Capture Volume' 160
	c 'AIF2 Digital ADC Capture Switch' on
}

case "$1" in
earpiece)
	route
	c 'Line Out Playback Switch' off
	c 'Earpiece Source Playback Route' DACL
	c 'Earpiece Playback Volume' 31
	c 'Earpiece Playback Switch' on
	;;
speaker)
	route
	c 'Earpiece Playback Switch' off
	c 'Line Out Source Playback Route' 'Mono Differential'
	c 'Line Out Playback Volume' 26
	c 'Line Out Playback Switch' on
	;;
off)
	c 'AIF2 Digital DAC Playback Switch' off
	c 'AIF2 Digital ADC Capture Switch' off
	c 'AIF2 ADC Mixer ADC Capture Switch' off
	c 'Mic1 Capture Switch' off
	c 'Earpiece Playback Switch' off
	c 'Line Out Playback Switch' off
	;;
*)
	echo "usage: $0 earpiece|speaker|off" >&2
	exit 1
	;;
esac
