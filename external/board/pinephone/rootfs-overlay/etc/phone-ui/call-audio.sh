#!/bin/sh
#
# Call audio on the PinePhone's codec (modem.conf's audio_script=): phone-ui runs
# `call-audio.sh earpiece|speaker <level>` as a call is dialed or answered, the
# loudspeaker is turned on or off, and the volume keys change the call's level (1 to 16),
# then `call-audio.sh off` once the call ends.
#   earpiece  the modem's voice (its PCM on the codec's AIF2, mono at 8 kHz) to the
#             earpiece, the main microphone (MIC1) to the modem
#   speaker   the same through the loudspeaker (line out, to the amplifier)
#   off       the modem's path and the microphone off, the earpiece muted, line out back
#             to the loudspeaker's level for music (S20sound's, phone-ui runs `off` as it
#             starts too)
# The digital paths at 0 dB: the DAC's -13.5 dB of music playback made calls quiet
# (phone-ui sets the DAC back for music after `off`). The level sets the earpiece's or
# line out's amplifier (0-31, 1.5 dB steps), 3 dB a level: 14 is the earpiece at its
# loudest; above it the earpiece takes the rest digitally on AIF2 (up to +6 dB, which
# may clip loud voices).
#

set -e

LEVEL_DEFAULT=14

c() {
	amixer -q -c 0 cset name="$1" "$2"
}

# The amplifier's value for level $2, $1 at the default level; above 31, the rest
# (in amplifier steps) on AIF2's DAC volume
amp() {
	level=${2:-$LEVEL_DEFAULT}
	a=$(($1 - (LEVEL_DEFAULT - level) * 2))
	[ "$a" -lt 0 ] && a=0
	boost=0
	if [ "$a" -gt 31 ]; then
		boost=$((a - 31))
		a=31
	fi
}

route() {
	c 'AIF2 DAC Stereo Playback Route' 'Mix Mono'
	c 'AIF2 DAC Playback Volume' $((160 + boost * 2)) # 0.75 dB steps, 0 dB at 160
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
	amp 31 "$2"
	route
	c 'Line Out Playback Switch' off
	c 'Earpiece Source Playback Route' DACL
	c 'Earpiece Playback Volume' "$a"
	c 'Earpiece Playback Switch' on
	;;
speaker)
	amp 26 "$2"
	route
	c 'Earpiece Playback Switch' off
	c 'Line Out Source Playback Route' 'Mono Differential'
	c 'Line Out Playback Volume' "$a"
	c 'Line Out Playback Switch' on
	;;
off)
	c 'AIF2 Digital DAC Playback Switch' off
	c 'AIF2 Digital ADC Capture Switch' off
	c 'AIF2 ADC Mixer ADC Capture Switch' off
	c 'Mic1 Capture Switch' off
	c 'Earpiece Playback Switch' off
	# The loudspeaker back as S20sound sets it for music
	c 'Line Out Source Playback Route' 'Mono Differential'
	c 'Line Out Playback Volume' 26
	c 'Line Out Playback Switch' on
	;;
*)
	echo "usage: $0 earpiece|speaker [level]|off" >&2
	exit 1
	;;
esac
