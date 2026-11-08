# `make analyze`'s report: awk -f analyze.awk analyze.known build/analyze/log
# A finding is known by its file, function and message (not its line, which moves):
# "src/apps/notes.c notes_load: dereference of NULL ‘note’ [CWE-476] [-Wanalyzer-null-dereference]".
# The new ones are printed whole (their paths through the code), the known ones counted, and
# those listed that no longer occur named. Exits 1 if there is a new one.

FILENAME == ARGV[1] {
	if ($0 !~ /^(#|$)/)
		known[$0] = 0
	next
}

# A diagnostic ends where the next one, or its file's or function's heading, starts
/^[^ ].*: (warning|error): / || /: In function / || /^In file included from / || /^[^ ].*: At top level:/ {
	show = 0
}

/: In function / {
	func_ = $0
	sub(/.*: In function ‘/, "", func_)
	sub(/’.*/, "", func_)
}

/^[^ ].*: warning: .*\[-Wanalyzer-/ {
	file = $0
	sub(/:.*/, "", file)
	gsub(/^(\.\.\/)+/, "", file)
	message = $0
	sub(/^[^ ]*: warning: /, "", message)
	key = file " " func_ ": " message
	if (key in known) {
		if (!known[key]++)
			found_known++
	} else if (!(key in new)) {
		new[key] = 1
		count_new++
		show = 1
		print ""
		print "New: " key
	}
}

show { print }

END {
	gone = 0
	for (k in known)
		if (!known[k]) {
			if (!gone++)
				print "\nNo longer found, to remove from analyze.known:"
			print "  " k
		}
	printf "\n-fanalyzer: %d new, %d known (analyze.known)\n", count_new, found_known
	exit count_new > 0
}
