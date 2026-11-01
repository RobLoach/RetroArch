# Pulls the callback count, its last-out notify and the teardown wait
# out of audio/drivers/asio.c, so asio_teardown_test.c runs the
# driver's own code.
{ sub(/\r$/, "") }
/^static retro_atomic_int_t g_asio_in_callback;/ { cnt = 1 }
cnt { count = count $0 "\n"; if ($0 ~ /^}/) cnt = 0; next }
/^static void asio_wait_callbacks_out\(void\)/ { wt = 1 }
wt { wait = wait $0 "\n"; if ($0 ~ /^}/) wt = 0; next }
END {
   if (count == "" || wait == "")
   {
      print "extract_teardown.awk: an anchor in asio.c has moved" > "/dev/stderr"
      exit 1
   }
   print "/* Generated from audio/drivers/asio.c by extract_teardown.awk. */"
   printf "%s\n%s", count, wait
}
