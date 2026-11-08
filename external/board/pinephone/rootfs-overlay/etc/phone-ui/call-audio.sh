#!/bin/sh
#
# Call audio on the PinePhone's codec (modem.conf's audio_script=): phone-ui runs
# `call-audio.sh earpiece|speaker|bluetooth <level>` as a call is dialed or answered, the
# loudspeaker or a headset is chosen, and the volume keys change the call's level (1 to 16),
# then `call-audio.sh off` once the call ends.
#   earpiece  the modem's voice (its PCM on the codec's AIF2, mono at 8 kHz) to the
#             earpiece, the main microphone (MIC1) to the modem
#   speaker   the same through the loudspeaker (line out, to the amplifier)
#   bluetooth to and from a Bluetooth headset (bluez-alsa's Hands-Free PCMs, the first
#             one listed): the RTL8723CS sends a call's audio (SCO, CVSD at 8 kHz) to its
#             PCM pins, the codec's AIF3, which the codec joins to AIF2 (AIF3 takes AIF2's
#             clocks), no CPU in between. bluez-alsa opens the call's link (SCO) only while
#             its PCM is open: an arecord of it holds it (nothing arrives: the sound is on
#             the pins). The level is the headset's gain (HFP's 0-15)
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

SCO_PID=/run/call-audio-sco.pid

# The headset's call link closed, AIF3 out of the modem's path
bluetooth_off() {
	start-stop-daemon -K -q -p "$SCO_PID" 2>/dev/null || true
	rm -f "$SCO_PID"
	c 'AIF3 ADC Source Capture Route' None
	c 'AIF2 DAC Source Playback Route' AIF2
	c 'AIF2 ADC Mixer AIF2 DAC Rev Capture Switch' off
}

route() {
	bluetooth_off
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
bluetooth)
	pcm=$(bluealsa-cli list-pcms 2>/dev/null | grep -m 1 /hfpag/sink || true)
	# /org/bluealsa/hci0/dev_AA_BB_CC_DD_EE_FF/hfpag/sink
	address=$(echo "$pcm" | sed -n 's|.*/dev_\([0-9A-F_]*\)/.*|\1|p' | tr _ :)
	[ -n "$address" ] || exec "$0" earpiece "$2" # the headset went meanwhile
	level=${2:-$LEVEL_DEFAULT}
	[ "$level" -gt 16 ] && level=16
	bluealsa-cli volume "$pcm" $((level - 1)) 2>/dev/null || true
	boost=0 # the level is the headset's alone
	route # as for the earpiece, then the headset instead of the earpiece and microphone
	start-stop-daemon -S -q -b -m -p "$SCO_PID" -x /usr/bin/arecord -- -q \
		-D "bluealsa:DEV=$address,PROFILE=sco" -f S16_LE -r 8000 -c 1 -t raw /dev/null || true
	c 'Earpiece Playback Switch' off
	c 'Line Out Playback Switch' off
	c 'AIF2 DAC Source Playback Route' 'AIF3+2'         # left: the headset (AIF3), right: the modem
	c 'AIF2 ADC Mixer AIF2 DAC Rev Capture Switch' on    # left <- the modem, right <- the headset
	c 'AIF2 ADC Mixer ADC Capture Switch' off            # not the phone's microphone
	c 'Mic1 Capture Switch' off                          # nor anywhere else
	c 'AIF2 ADC Stereo Capture Route' 'Reverse Stereo'   # the modem's slot (left) <- the headset
	c 'AIF3 ADC Source Capture Route' 'AIF2 ADCL'        # to the headset <- the modem
	;;
off)
	bluetooth_off
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
	echo "usage: $0 earpiece|speaker|bluetooth [level]|off" >&2
	exit 1
	;;
esac
