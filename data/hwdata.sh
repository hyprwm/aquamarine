#!/bin/sh

while read -r id vendor; do
	[ "${#id}" = 3 ] || exit 1

	printf "\t__AQ_PNP_PROP(\"%s\", \"%s\"),\n" "$id" "$vendor"
done

