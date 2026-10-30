# Config-file and argument handling checks.  Each case runs VST against a video path that doesn't exist, so a config
# that parses gets as far as "Can't open video" (and a bad one stops earlier with its own message) in well under a second.
# Invoked by ctest with -DVST=<exe> -DSRC=<repo root> -DOUT=<work dir>.
file(MAKE_DIRECTORY ${OUT})
file(READ ${SRC}/bin/VST.cfg sample)
string(REPLACE "\r\n" "\n" sample "${sample}")

# expect_run(<name> <config text> <regex the output must match> [extra args...])
# Every case must exit non-zero (the input is missing), and print the expected message.
function(expect_run name text regex)
  file(WRITE ${OUT}/${name}.cfg "${text}")
  execute_process(
    COMMAND ${VST} --headless --input ${OUT}/no-such-video.avi --config ${OUT}/${name}.cfg --stats ${OUT}/${name}.csv ${ARGN}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(rc EQUAL 0)
    message(FATAL_ERROR "${name}: expected a non-zero exit status\n${out}")
  endif()
  if(NOT out MATCHES "${regex}")
    message(FATAL_ERROR "${name}: output doesn't match \"${regex}\":\n${out}")
  endif()
endfunction()

# Forgiving syntax: out-of-order keys (first two settings moved to the end), a lower-case key, comment-only and blank
# lines, CRLF line endings, and a trailing newline all parse.
string(REGEX MATCH "dataPathPrefix[^\n]*\nL2RDirection[^\n]*\n" head "${sample}")
string(REPLACE "${head}" "" rest "${sample}")
set(messy "# my camera\n\n${rest}\n${head}\n")
string(REPLACE "SLOP =" "slop =" messy "${messy}")
string(REPLACE "\n" "\r\n" messy "${messy}")
expect_run(messy "${messy}" "Can't open video")

# A key missing entirely keeps its default.
string(REGEX REPLACE "nextHeight[^\n]*\n?" "" no_next "${sample}")
expect_run(missing_key "${no_next}" "Can't open video")

string(REPLACE "SLOP =" "SLOPE =" typo "${sample}")
expect_run(unknown_key "${typo}" "unknown_key.cfg:[0-9]+: unknown setting \"SLOPE\"")

string(REGEX REPLACE "SLOP = [0-9]+" "SLOP = fifteen" badval "${sample}")
expect_run(bad_value "${badval}" "can't read a value for SLOP from \"fifteen\"")

string(REGEX REPLACE "AnalysisBoxWidth = [0-9]+" "AnalysisBoxWidth = 1300" wide "${sample}")
expect_run(too_wide "${wide}" "analysis box too wide")

expect_run(bad_arg "${sample}" "--speed-limit needs a whole number" --speed-limit fast)

# LegalDirection (one-way streets) takes Both, L2R, R2L or one of the two compass labels, in any case.
expect_run(legal_compass "${sample}LegalDirection = nw\n" "Can't open video")
expect_run(legal_l2r "${sample}LegalDirection = L2R\n" "Can't open video")
expect_run(legal_bad "${sample}LegalDirection = uphill\n" "LegalDirection must be Both, L2R, R2L, SE or NW")
