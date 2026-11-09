#!/bin/sh
# Trilium's schema (schema.sql) as a C string, tr_schema: gen-schema.sh schema.sql schema.c
set -e
{
	echo '/* Generated from schema.sql by gen-schema.sh */'
	echo 'const char tr_schema[] ='
	sed 's/\\/\\\\/g; s/"/\\"/g; s/.*/"&\\n"/' "$1"
	echo ';'
} > "$2"
