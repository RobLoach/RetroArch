MAKEFILE_DEFINES='.MAKEFILE_DEFINES'
CONFIG_DEFINES='.CONFIG_DEFINES'
cat /dev/null > "$MAKEFILE_DEFINES" > "$CONFIG_DEFINES"
#cat /dev/null > "${MAKEFILE_DEFINES:=.MAKEFILE_DEFINES}" > "${CONFIG_DEFINES=.CONFIG_DEFINES}"

[ "$PREFIX" ] || PREFIX="/usr/local"

add_define_header()
{ echo "$1=$2" >> "$CONFIG_DEFINES";}

add_define_make()
{ echo "$1=$2" >> "$MAKEFILE_DEFINES";}

add_include_dirs()
{	while [ "$1" ]; do INCLUDE_DIRS="$INCLUDE_DIRS -I$1"; shift; done
	INCLUDE_DIRS="${INCLUDE_DIRS# }"
}

add_library_dirs()
{	while [ "$1" ]; do LIBRARY_DIRS="$LIBRARY_DIRS -L$1"; shift; done
	LIBRARY_DIRS="${LIBRARY_DIRS# }"
}

check_lib() # $1 = language  $2 = HAVE_$2  $3 = lib  $4 = function in lib  $5 = extralibs $6 = headers $7 = critical error message [checked only if non-empty]
{	tmpval="$(eval echo \$HAVE_$2)"
	[ "$tmpval" = 'no' ] && return 0

	if [ "$1" = cxx ]; then
		COMPILER="$CXX"
		TEMP_CODE="$TEMP_CXX"
		TEST_C="extern \"C\" { void $4(void); } int main() { $4(); }"
	else
		COMPILER="$CC"
		TEMP_CODE="$TEMP_C"
		TEST_C="void $4(void); int main(void) { $4(); return 0; }"
	fi

	if [ "$4" ]; then
		ECHOBUF="Checking function $4 in ${3% }"
		if [ "$6" ]; then
			printf %s\\n "$6" "int main(void) { void *p = (void*)$4; return 0; }" > "$TEMP_CODE"
		else
			echo "$TEST_C" > "$TEMP_CODE"
		fi
	else
		ECHOBUF="Checking existence of ${3% }"
		echo "int main(void) { return 0; }" > "$TEMP_CODE"
	fi
	answer='no'
	"$COMPILER" -o \
		"$TEMP_EXE" \
		"$TEMP_CODE" \
		$INCLUDE_DIRS \
		$LIBRARY_DIRS \
		$(printf %s "$5") \
		$CFLAGS \
		$LDFLAGS \
		$(printf %s "$3") >>config.log 2>&1 && answer='yes'
	eval HAVE_$2="$answer"; echo "$ECHOBUF ... $answer"
	rm -f -- "$TEMP_CODE" "$TEMP_EXE"

	[ "$answer" = 'no' ] && {
		[ "$7" ] && die 1 "$7"
		[ "$tmpval" = 'yes' ] && {
			die 1 "Forced to build with library $3, but cannot locate. Exiting ..."
		}

	}

	return 0
}

check_code_c()
{	tmpval="$(eval echo \$HAVE_$1)"
	[ "$tmpval" = 'no' ] && return 0

	ECHOBUF="Checking C code snippet \"$3\""
	answer='no'
	"$CC" -o "$TEMP_EXE" "$TEMP_C" $INCLUDE_DIRS $LIBRARY_DIRS $2 $CFLAGS $LDFLAGS >>config.log 2>&1 && answer='yes'
	eval HAVE_$1="$answer"; echo "$ECHOBUF ... $answer"
	rm -f -- "$TEMP_C" "$TEMP_EXE"
}

check_code_cxx()
{	tmpval="$(eval echo \$HAVE_$1)"
	[ "$tmpval" = 'no' ] && return 0

	ECHOBUF="Checking C++ code snippet \"$3\""
	answer='no'
	"$CXX" -o "$TEMP_EXE" "$TEMP_CXX" $INCLUDE_DIRS $LIBRARY_DIRS $2 $CXXFLAGS $LDFLAGS >>config.log 2>&1 && answer='yes'
	eval HAVE_$1="$answer"; echo "$ECHOBUF ... $answer"
	rm -f -- "$TEMP_CXX" "$TEMP_EXE"
}

check_pkgconf()	#$1 = HAVE_$1	$2 = package	$3 = version	$4 = critical error message [checked only if non-empty]
{	tmpval="$(eval echo \$HAVE_$1)"
	[ "$tmpval" = 'no' ] && return 0

	ECHOBUF="Checking presence of package $2"
	[ "$3" ] && ECHOBUF="$ECHOBUF >= $3"

	[ "$PKG_CONF_PATH" = "none" ] && {
		eval HAVE_$1="no"
		echo "$ECHOBUF ... no"
		return 0
	}

	answer='no'
	version='no'
	$PKG_CONF_PATH --atleast-version="${3:-0.0}" "$2" && {
		answer='yes'
		version=$($PKG_CONF_PATH --modversion "$2")
		eval $1_CFLAGS=\"$($PKG_CONF_PATH $2 --cflags)\"
		eval $1_LIBS=\"$($PKG_CONF_PATH $2 --libs)\"
	}
	
	eval HAVE_$1="$answer";
	echo "$ECHOBUF ... $version"
	PKG_CONF_USED="$PKG_CONF_USED $1"
	[ "$answer" = 'no' ] && {
		[ "$4" ] && die 1 "$4"
		[ "$tmpval" = 'yes' ] && \
			die 1 "Forced to build with package $2, but cannot locate. Exiting ..."
	}
}

check_header()	#$1 = HAVE_$1	$2..$5 = header files
{	tmpval="$(eval echo \$HAVE_$1)"
	[ "$tmpval" = 'no' ] && return 0
	CHECKHEADER="$2"
	echo "#include <$2>" > "$TEMP_C"
	[ "$3" != "" ] && CHECKHEADER="$3" && echo "#include <$3>" >> "$TEMP_C"
	[ "$4" != "" ] && CHECKHEADER="$4" && echo "#include <$4>" >> "$TEMP_C"
	[ "$5" != "" ] && CHECKHEADER="$5" && echo "#include <$5>" >> "$TEMP_C"
	echo "int main(void) { return 0; }" >> "$TEMP_C"
	answer='no'
	"$CC" -o "$TEMP_EXE" "$TEMP_C" $INCLUDE_DIRS >>config.log 2>&1 && answer='yes'
	eval HAVE_$1="$answer"; echo "Checking presence of header file $CHECKHEADER ... $answer"
	rm -f -- "$TEMP_C" "$TEMP_EXE"
	[ "$tmpval" = 'yes' ] && [ "$answer" = 'no' ] && \
		die 1 "Build assumed that $2 exists, but cannot locate. Exiting ..."
}

check_macro()	#$1 = HAVE_$1	$2 = macro name
{	tmpval="$(eval echo \$HAVE_$1)"
	[ "$tmpval" = 'no' ] && return 0
	ECHOBUF="Checking presence of predefined macro $2"
	cat << EOF > "$TEMP_C"
#ifndef $2
#error $2 is not defined
#endif
int main(void) { return 0; }
EOF
	answer='no'
	"$CC" -o "$TEMP_EXE" "$TEMP_C" $CFLAGS $INCLUDE_DIRS >>config.log 2>&1 && answer='yes'
	eval HAVE_$1="$answer"; echo "$ECHOBUF ... $answer"
	rm -f -- "$TEMP_C" "$TEMP_EXE"
	[ "$tmpval" = 'yes' ] && [ "$answer" = 'no' ] && \
		die 1 "Build assumed that $2 is defined, but it's not. Exiting ..."
}

check_switch_c()	#$1 = HAVE_$1	$2 = switch	$3 = critical error message [checked only if non-empty]
{	ECHOBUF="Checking for availability of switch $2 in $CC"
	echo "int main(void) { return 0; }" > $TEMP_C
	answer='no'
	"$CC" -o "$TEMP_EXE" "$TEMP_C" $2 >>config.log 2>&1 && answer='yes'
	eval HAVE_$1="$answer"; echo "$ECHOBUF ... $answer"
	rm -f -- "$TEMP_C" "$TEMP_EXE"
	[ "$answer" = 'no' ] && {
		[ "$3" ] && die 1 "$3"
	}
}

check_switch_cxx()	#$1 = HAVE_$1	$2 = switch	$3 = critical error message [checked only if non-empty]
{	ECHOBUF="Checking for availability of switch $2 in $CXX"
	echo "int main() { return 0; }" > $TEMP_CXX
	answer='no'
	"$CXX" -o "$TEMP_EXE" "$TEMP_CXX" "$2" >>config.log 2>&1 && answer='yes'
	eval HAVE_$1="$answer"; echo "$ECHOBUF ... $answer"
	rm -f -- "$TEMP_CXX" "$TEMP_EXE"
	[ "$answer" = 'no' ] && {
		[ "$3" ] && die 1 "$3"
	}
}

create_config_header()
{   outfile="$1"; shift

	echo "Creating config header: $outfile"
	name=$(echo "QB_${outfile}__" | tr '.[a-z]' '_[A-Z]')
	{	echo "#ifndef $name"
		echo "#define $name"
		echo ""
		echo "#define PACKAGE_NAME \"$PACKAGE_NAME\""

		while [ "$1" ]; do
			case $(eval echo \$HAVE_$1) in
				'yes')
					if [ "$(eval echo \$C89_$1)" = "no" ]; then echo "#if __cplusplus || __STDC_VERSION__ >= 199901L"; fi
					echo "#define HAVE_$1 1"
					if [ "$(eval echo \$C89_$1)" = "no" ]; then echo "#endif"; fi
					;;
				'no') echo "/* #undef HAVE_$1 */";;
			esac
			shift
		done

		while IFS='=' read VAR VAL; do echo "#define $VAR $VAL"; done < "$CONFIG_DEFINES"

		echo "#endif"
	} > "$outfile"
}

create_config_make()
{	outfile="$1"; shift

	echo "Creating make config: $outfile"

	{	if [ "$USE_LANG_C" = 'yes' ]; then
			echo "CC = $CC"
			echo "CFLAGS = $CFLAGS"
		fi
		if [ "$USE_LANG_CXX" = 'yes' ]; then
			echo "CXX = $CXX"
			echo "CXXFLAGS = $CXXFLAGS"
		fi
		echo "WINDRES = $WINDRES"
		echo "ASFLAGS = $ASFLAGS"
		echo "LDFLAGS = $LDFLAGS"
		echo "INCLUDE_DIRS = $INCLUDE_DIRS"
		echo "LIBRARY_DIRS = $LIBRARY_DIRS"
		echo "PACKAGE_NAME = $PACKAGE_NAME"
		echo "PREFIX = $PREFIX"

		while [ "$1" ]; do
			case $(eval echo \$HAVE_$1) in
				'yes')
					if [ "$(eval echo \$C89_$1)" = "no" ]; then echo "ifneq (\$(C89_BUILD),1)"; fi
					echo "HAVE_$1 = 1"
					if [ "$(eval echo \$C89_$1)" = "no" ]; then echo "endif"; fi
					;;
				'no') echo "HAVE_$1 = 0";;
			esac
			
			case "$PKG_CONF_USED" in
				*$1*)
					FLAGS="$(eval echo \$$1_CFLAGS)"
					LIBS="$(eval echo \$$1_LIBS)"
					echo "$1_CFLAGS = ${FLAGS%"${FLAGS##*[! ]}"}"
					echo "$1_LIBS = ${LIBS%"${LIBS##*[! ]}"}"
				;;
			esac
			shift
		done
		while IFS='=' read VAR VAL; do echo "$VAR = $VAL"; done < "$MAKEFILE_DEFINES"

	} > "$outfile"
}

. qb/config.libs.sh

rm -f -- "$MAKEFILE_DEFINES" "$CONFIG_DEFINES"
